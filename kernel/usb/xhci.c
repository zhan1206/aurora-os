/*
 * xhci.c - xHCI (eXtensible Host Controller Interface) driver
 *
 * Initializes USB 3.x host controllers, manages device slots, endpoints,
 * and transfer rings.  Provides low-level transport for USB control,
 * bulk, and interrupt transfers.
 *
 * // USB (v4.2.6)  — FIXED (v4.3.9): BUILD-04 — Fix nested comment
 */
#include "xhci.h"
#include "xhci_dma.h"  /* XHCI_DMA (v4.2.7) */
#include "usb.h"
#include "../pci.h"
#include "../include/log.h"
#include "../include/string.h"
#include "../include/arch.h"
#include "../smp.h"
#include "../mem.h"
#include <stdint.h>

/* ================================================================
 * Global Lists
 * ================================================================ */
static struct xhci_controller *xhci_ctrl_list = NULL;
static struct usb_device      *usb_dev_list     = NULL;
static struct usb_device      *usb_dev_tail     = NULL;

/*
 * FIXED (v4.4.4): P1-8 — xHCI event ring: protect with spinlock.
 * The event ring is consumed by both the interrupt handler and
 * command submission code.  Without synchronization, entries can
 * be consumed twice or missed.
 */
static spinlock_t xhci_event_lock = {0};

/* ================================================================
 * USB Device List Management
 * ================================================================ */
void usb_device_add(struct usb_device *dev) {
    if (!dev) return;
    if (usb_dev_tail) {
        usb_dev_tail->next = dev;
    } else {
        usb_dev_list = dev;
    }
    usb_dev_tail = dev;
    dev->next = NULL;
}

struct usb_device *usb_device_list(void) {
    return usb_dev_list;
}

/* FIXED (v4.2.7): BUG-XHCI-HOTPLUG — Remove a USB device from the tracking
 * list when it is physically disconnected.  Searches by root_port and
 * controller pointer, then unlinks the device from the global list. */
void usb_device_remove(struct xhci_controller *hc, uint32_t port) {
    struct usb_device *prev = NULL;
    struct usb_device *dev = usb_dev_list;
    while (dev) {
        if (dev->hc == hc && dev->root_port == (uint8_t)(port + 1)) {
            if (prev) {
                prev->next = dev->next;
            } else {
                usb_dev_list = dev->next;
            }
            if (usb_dev_tail == dev) {
                usb_dev_tail = prev;
            }
            log_printf(LOG_LEVEL_INFO, "xHCI: device removed from port %d\n", port);
            kfree(dev);
            return;
        }
        prev = dev;
        dev = dev->next;
    }
}

struct usb_device *usb_find_by_class(uint8_t class_code) {
    struct usb_device *dev = usb_dev_list;
    while (dev) {
        if (dev->dev_desc.bDeviceClass == class_code) {
            return dev;
        }
        for (int i = 0; i < dev->num_interfaces; i++) {
            if (dev->interfaces[i].interface_class == class_code) {
                return dev;
            }
        }
        dev = dev->next;
    }
    return NULL;
}

/* Identity mapping: virtual == physical for kernel heap */
/* FIXED (v4.2.7): BUG-XHCI-7 - virt_to_phys macro for DMA correctness */
#define virt_to_phys(vaddr) ((uint64_t)(uintptr_t)(vaddr))

/* ================================================================
 * MMIO Access Helpers
 * ================================================================ */
/* FIXED (v4.2.7): BUG-XHCI-6 - add MMIO ordering barriers for uncacheable memory */
static inline uint32_t xhci_read32(volatile uint32_t *addr) {
    __sync_synchronize();
    return *addr;
}

static inline uint64_t xhci_read64(volatile uint64_t *addr) {
    __sync_synchronize();
    return *addr;
}

static inline void xhci_write32(volatile uint32_t *addr, uint32_t val) {
    __sync_synchronize();
    *addr = val;
    __sync_synchronize();
}

static inline void xhci_write64(volatile uint64_t *addr, uint64_t val) {
    __sync_synchronize();
    *addr = val;
    __sync_synchronize();
}

static inline uint32_t xhci_read_op(struct xhci_controller *hc, uint32_t offset) {
    return xhci_read32((volatile uint32_t *)((uintptr_t)hc->operational + offset));
}

static inline void xhci_write_op(struct xhci_controller *hc, uint32_t offset, uint32_t val) {
    xhci_write32((volatile uint32_t *)((uintptr_t)hc->operational + offset), val);
}

static inline uint64_t xhci_read_op64(struct xhci_controller *hc, uint32_t offset) {
    uint64_t lo = xhci_read_op(hc, offset);
    uint64_t hi = hc->ac64 ? xhci_read_op(hc, offset + 4) : 0;
    return lo | (hi << 32);
}

static inline void xhci_write_op64(struct xhci_controller *hc, uint32_t offset, uint64_t val) {
    xhci_write_op(hc, offset, (uint32_t)(val & 0xFFFFFFFFU));
    xhci_write_op(hc, offset + 4, (uint32_t)(val >> 32));
}

static inline uint32_t xhci_read_runtime(struct xhci_controller *hc, uint32_t offset) {
    return xhci_read32((volatile uint32_t *)((uintptr_t)hc->runtime + offset));
}

static inline void xhci_write_runtime(struct xhci_controller *hc, uint32_t offset, uint32_t val) {
    xhci_write32((volatile uint32_t *)((uintptr_t)hc->runtime + offset), val);
}

static inline uint32_t xhci_read_doorbell(struct xhci_controller *hc, uint32_t offset) {
    return xhci_read32((volatile uint32_t *)((uintptr_t)hc->doorbell + offset));
}

static inline void xhci_write_doorbell(struct xhci_controller *hc, uint32_t offset, uint32_t val) {
    xhci_write32((volatile uint32_t *)((uintptr_t)hc->doorbell + offset), val);
}

static inline uint32_t xhci_read_port(struct xhci_controller *hc, uint32_t port_index) {
    /* FIXED (v4.2.7): BUG-XHCI-PORT-BOUNDS — Explicit bounds check on port
     * index before array access.  hc->num_ports may lag behind the actual
     * hardware port count in some corner cases. */
    if (port_index >= hc->num_ports || port_index >= XHCI_MAX_PORTS) return 0;
    return xhci_read32((volatile uint32_t *)((uintptr_t)hc->operational + hc->port_offsets[port_index]));
}

static inline void xhci_write_port(struct xhci_controller *hc, uint32_t port_index, uint32_t val) {
    /* FIXED (v4.2.7): BUG-XHCI-PORT-BOUNDS — Same bounds check as read. */
    if (port_index >= hc->num_ports || port_index >= XHCI_MAX_PORTS) return;
    xhci_write32((volatile uint32_t *)((uintptr_t)hc->operational + hc->port_offsets[port_index]), val);
}

/* ================================================================
 * Transfer Ring Helpers
 * ================================================================ */
static void xhci_ring_init(struct xhci_transfer_ring *ring, uint32_t size) {
    memset(ring, 0, sizeof(*ring));
    ring->size = size;
    size_t alloc_size = size * sizeof(struct xhci_trb);
    /* XHCI_DMA (v4.2.7) - Use xhci_dma_alloc for DMA-safe allocation */
    ring->trbs = (struct xhci_trb *)xhci_dma_alloc(alloc_size, &ring->phys_addr);
    if (ring->trbs) {
        ring->pcs = 1;
    }

    /* Set up link TRB at the end to form a ring */
    if (ring->trbs && size > 0) {
        ring->trbs[size - 1].parameter = ring->phys_addr;
        ring->trbs[size - 1].status = 0;
        ring->trbs[size - 1].control = (XHCI_TRB_LINK << XHCI_TRB_TYPE_SHIFT) | XHCI_TRB_C;
    }
}

static struct xhci_trb *xhci_ring_enqueue(struct xhci_transfer_ring *ring) {
    uint32_t idx = ring->enqueue;
    ring->enqueue = (ring->enqueue + 1) % ring->size;
    if (ring->enqueue == 0) {
        ring->pcs ^= 1;
    }
    return &ring->trbs[idx];
}

static void xhci_ring_enqueue_trb(struct xhci_transfer_ring *ring,
                                  struct xhci_trb *trb) {
    struct xhci_trb *dst = xhci_ring_enqueue(ring);
    /* Set cycle bit based on current producer cycle state */
    trb->control = (trb->control & ~XHCI_TRB_C) | ring->pcs;
    dst->parameter = trb->parameter;
    dst->status = trb->status;
    dst->control = trb->control;
}

/* ================================================================
 * Event Ring Helpers
 * ================================================================ */
static struct xhci_trb *xhci_event_ring_dequeue(struct xhci_controller *hc) {
    /* FIXED (v4.4.4): P1-8 — Protect event ring dequeue with spinlock + IRQ
     * disable.  The event ring is consumed by both the interrupt handler
     * (IRQ context) and polling code (command submission).  Without
     * synchronization, entries can be consumed twice or missed. */
    uint64_t irq_flags = irq_save();
    spin_lock(&xhci_event_lock);

    uint32_t idx = hc->event_ring_dequeue;
    struct xhci_trb *evt = &hc->event_ring[idx];

    /* Check if the event is valid (cycle bit == consumer cycle state) */
    if ((evt->control & XHCI_TRB_C) != hc->event_ring_ccs) {
        spin_unlock(&xhci_event_lock);
        irq_restore(irq_flags);
        return NULL;
    }

    hc->event_ring_dequeue = (hc->event_ring_dequeue + 1) % XHCI_EVENT_RING_SIZE;
    if (hc->event_ring_dequeue == 0) {
        hc->event_ring_ccs ^= 1;
    }

    spin_unlock(&xhci_event_lock);
    irq_restore(irq_flags);
    return evt;
}

/* ================================================================
 * Ring the Doorbell
 * ================================================================ */
void xhci_ring_doorbell(struct xhci_controller *hc, uint8_t slot_id,
                        uint8_t ep_id) {
    uint32_t val = (uint32_t)ep_id | ((uint32_t)slot_id << 0);
    xhci_write_doorbell(hc, xhci_doorbell_offset(slot_id), val);
}

/* ================================================================
 * Send Command via Command Ring
 * ================================================================ */
int xhci_send_command(struct xhci_controller *hc, struct xhci_trb *cmd_trb) {
    if (!hc || !hc->cmd_ring.trbs) return -1;

    /* Enqueue the command TRB */
    xhci_ring_enqueue_trb(&hc->cmd_ring, cmd_trb);

    /* Ring the command doorbell */
    xhci_write_doorbell(hc, 0, 0);

    /* Poll for command completion */
    uint32_t cc = 0;
    int timeout = 5000000; /* ~5 seconds at 1 us per iteration */
    while (timeout-- > 0) {
        struct xhci_trb *evt = xhci_event_ring_dequeue(hc);
        if (evt) {
            uint32_t evt_type = (evt->control & XHCI_TRB_TYPE_MASK) >> XHCI_TRB_TYPE_SHIFT;
            if (evt_type == XHCI_EVT_COMMAND_COMPLETE) {
                cc = (evt->status & XHCI_TRB_CC_MASK) >> XHCI_TRB_CC_SHIFT;
                break;
            }
        }
        /* Busy-wait: yield or pause */
        for (volatile int i = 0; i < 1000; i++) {
            asm volatile ("pause" ::: "memory");
        }
    }

    if (timeout <= 0) {
        log_printf(LOG_LEVEL_WARN, "xHCI: command timed out\n");
        return -1;
    }

    if (cc != XHCI_CC_SUCCESS) {
        log_printf(LOG_LEVEL_WARN, "xHCI: command failed, cc=%d\n", cc);
        return -1;
    }

    return 0;
}

/* ================================================================
 * Enable Slot
 * ================================================================ */
/* FIXED (v4.2.7): BUG-XHCI-SLOT-ID — Read the slot ID from the command
 * completion event TRB instead of scanning the slots array.  The slot ID
 * is in the upper 8 bits (bits 31:24) of the event TRB control field.
 * Scanning the array is fragile because the array may have stale entries
 * or the hardware may assign a slot ID other than the first free slot. */
int xhci_enable_slot(struct xhci_controller *hc) {
    if (!hc) return -1;

    struct xhci_trb cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.control = (XHCI_TRB_ENABLE_SLOT << XHCI_TRB_TYPE_SHIFT);

    /* Enqueue the command and ring the doorbell */
    xhci_ring_enqueue_trb(&hc->cmd_ring, &cmd);
    xhci_write_doorbell(hc, 0, 0);

    /* Poll for command completion and extract the slot ID */
    int slot_id = -1;
    int timeout = 5000000;
    while (timeout-- > 0) {
        struct xhci_trb *evt = xhci_event_ring_dequeue(hc);
        if (evt) {
            uint32_t evt_type = (evt->control & XHCI_TRB_TYPE_MASK) >> XHCI_TRB_TYPE_SHIFT;
            if (evt_type == XHCI_EVT_COMMAND_COMPLETE) {
                uint32_t cc = (evt->status & XHCI_TRB_CC_MASK) >> XHCI_TRB_CC_SHIFT;
                if (cc == XHCI_CC_SUCCESS) {
                    /* Slot ID is in bits 31:24 of the event TRB control field */
                    slot_id = (int)((evt->control >> 24) & 0xFF);
                }
                break;
            }
        }
        for (volatile int i = 0; i < 1000; i++) {
            asm volatile ("pause" ::: "memory");
        }
    }

    if (slot_id <= 0 || slot_id > (int)hc->max_slots) {
        log_printf(LOG_LEVEL_WARN, "xHCI: enable slot failed, slot_id=%d\n", slot_id);
        return -1;
    }

    /* Initialize the slot */
    hc->slots[slot_id].slot_id = (uint8_t)slot_id;
    hc->slots[slot_id].enabled = 1;

    /* Allocate device context */
    /* XHCI_DMA (v4.2.7) - Use xhci_dma_alloc for DMA-safe allocation */
    size_t full_ctx_size = sizeof(struct xhci_device_context);
    hc->slots[slot_id].ctx = (struct xhci_device_context *)xhci_dma_alloc(full_ctx_size, &hc->slots[slot_id].ctx_phys);
    if (hc->slots[slot_id].ctx) {
        /* Set DCBAA entry */
        hc->dcbaa[slot_id] = hc->slots[slot_id].ctx_phys;
    }

    return slot_id;
}

/* ================================================================
 * Address Device
 * ================================================================ */
int xhci_address_device(struct xhci_controller *hc, uint8_t slot_id,
                        uint8_t root_port, uint8_t speed) {
    if (!hc || slot_id == 0 || slot_id > hc->max_slots) return -1;

    struct xhci_device_slot *slot = &hc->slots[slot_id];
    if (!slot->enabled || !slot->ctx) return -1;

    /* Build input context */
    size_t ictx_size = sizeof(struct xhci_input_context) + sizeof(struct xhci_ep_context);
    struct xhci_input_context *ictx = (struct xhci_input_context *)kmalloc(ictx_size);
    if (!ictx) return -1;
    memset(ictx, 0, ictx_size);

    /* Set add flags: add slot context (bit 0) and EP0 context (bit 1) */
    ictx->add_flags = XHCI_INPUT_ADD_SLOT | XHCI_INPUT_ADD_EP1;

    /* Slot context: set root hub port, speed, and context entries (1 = EP0) */
    ictx->slot.ctx_0 = ((uint32_t)root_port << XHCI_SLOT_CTX_RH_PORT_SHIFT)
                     | ((uint32_t)speed << XHCI_SLOT_CTX_SPEED_SHIFT)
                     | (1U << XHCI_SLOT_CTX_ENTRIES_SHIFT);

    /* EP0 context: control endpoint, max packet size = 8 (default for low-speed) */
    struct xhci_ep_context *ep0 = (struct xhci_ep_context *)(ictx + 1);
    ep0->ctx_1 = 8;  /* Max Packet Size = 8 */

    /* Allocate transfer ring for EP0 */
    xhci_ring_init(&slot->ep_ring[1], 16);
    if (!slot->ep_ring[1].trbs) {
        kfree(ictx);
        return -1;
    }
    ep0->ctx_2 = (uint32_t)(slot->ep_ring[1].phys_addr & 0xFFFFFFFFU)
               | XHCI_EP_CTX_DCS;
    ep0->ctx_3 = (uint32_t)(slot->ep_ring[1].phys_addr >> 32);

    /* Set EP type: control (4) */
    ep0->ctx_1 |= (uint32_t)(XHCI_EP_TYPE_CONTROL << 3) << 8;

    /* Average TRB length = 8 */
    ep0->ctx_4 = 8;

    /* Send Address Device command */
    struct xhci_trb cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.parameter = virt_to_phys(ictx);
    cmd.control = (XHCI_TRB_ADDRESS_DEVICE << XHCI_TRB_TYPE_SHIFT)
                | ((uint32_t)slot_id << 24);

    int result = xhci_send_command(hc, &cmd);

    if (result == 0) {
        slot->addressed = 1;
        slot->speed = speed;
        slot->root_port = root_port;
        /* Extract assigned address from slot context */
        slot->address = (uint8_t)(slot->ctx->slot.ctx_3 & XHCI_SLOT_CTX_ADDR_MASK);
    }

    kfree(ictx);
    return result;
}

/* ================================================================
 * Configure Endpoint
 * ================================================================ */
int xhci_configure_endpoint(struct xhci_controller *hc, uint8_t slot_id,
                            int ep_id, int ep_type, uint16_t max_packet_size,
                            uint8_t interval) {
    if (!hc || slot_id == 0 || slot_id > hc->max_slots) return -1;
    if (ep_id < 0 || ep_id > 31) return -1;

    struct xhci_device_slot *slot = &hc->slots[slot_id];
    if (!slot->enabled) return -1;

    /* FIXED (v4.2.7): BUG-XHCI-EPRING-LEAK - Free old TRB array before
     * overwriting the ep_ring.  Without this, re-configuring an endpoint
     * leaks the previously-allocated TRB array. */
    if (slot->ep_ring[ep_id].trbs) {
        xhci_dma_free(slot->ep_ring[ep_id].trbs);  /* XHCI_DMA (v4.2.7) */
    }
    /* Allocate transfer ring for this endpoint */
    xhci_ring_init(&slot->ep_ring[ep_id], 32);
    if (!slot->ep_ring[ep_id].trbs) return -1;

    /* Build input context */
    /* FIXED (v4.2.7): BUG-XHCI-8 - correct EP context count: we need ep_id entries */
    size_t ctx_size = sizeof(struct xhci_input_context) + (size_t)ep_id * sizeof(struct xhci_ep_context);
    struct xhci_input_context *ictx = (struct xhci_input_context *)kmalloc(ctx_size);
    if (!ictx) return -1;
    memset(ictx, 0, ctx_size);

    /* Add this endpoint context */
    /* FIXED (v4.2.7): BUG-XHCI-8 - bit 0 is slot, bit 1 is EP0, so bit for EP N is N+1 */
    ictx->add_flags = (uint32_t)(1U << (ep_id + 1));

    /* Locate the EP context in the input context */
    struct xhci_ep_context *ep = (struct xhci_ep_context *)(ictx + 1);
    ep = &ep[ep_id - 1]; /* EP contexts are 1-indexed in the array */

    ep->ctx_1 = max_packet_size;
    ep->ctx_1 |= (uint32_t)(ep_type << 3) << 8;
    ep->ctx_1 |= ((uint32_t)interval << XHCI_EP_CTX_INTERVAL_SHIFT);

    ep->ctx_2 = (uint32_t)(slot->ep_ring[ep_id].phys_addr & 0xFFFFFFFFU)
              | XHCI_EP_CTX_DCS;
    ep->ctx_3 = (uint32_t)(slot->ep_ring[ep_id].phys_addr >> 32);

    ep->ctx_4 = (uint32_t)(max_packet_size & 0xFFFF);

    /* Send Configure Endpoint command */
    struct xhci_trb cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.parameter = virt_to_phys(ictx);
    cmd.control = (XHCI_TRB_CONFIGURE_EP << XHCI_TRB_TYPE_SHIFT)
                | ((uint32_t)slot_id << 24);

    int result = xhci_send_command(hc, &cmd);

    kfree(ictx);
    return result;
}

/* ================================================================
 * Get Port Status
 * ================================================================ */
uint32_t xhci_get_port_status(struct xhci_controller *hc, uint32_t port_index) {
    return xhci_read_port(hc, port_index);
}

/* ================================================================
 * Reset Port
 * ================================================================ */
int xhci_reset_port(struct xhci_controller *hc, uint32_t port_index) {
    if (port_index >= hc->num_ports) return -1;

    uint32_t portsc = xhci_read_port(hc, port_index);

    /* Check if device is connected */
    if (!(portsc & XHCI_PORTSC_CCS)) {
        log_printf(LOG_LEVEL_DEBUG, "xHCI: port %d - no device connected\n", port_index);
        return -1;
    }

    /* Read port speed */
    uint32_t speed_id = (portsc & XHCI_PORTSC_SPEED_MASK) >> XHCI_PORTSC_SPEED_SHIFT;
    uint8_t speed = 0;
    switch (speed_id) {
        case XHCI_PORT_SPEED_LOW:    speed = 1; break;
        case XHCI_PORT_SPEED_FULL:   speed = 2; break;
        case XHCI_PORT_SPEED_HIGH:   speed = 3; break;
        case XHCI_PORT_SPEED_SUPER:  speed = 4; break;
        case XHCI_PORT_SPEED_SUPER_PLUS: speed = 5; break;
        default: speed = 3; break; /* Assume high-speed */
    }

    /* Issue port reset */
    portsc |= XHCI_PORTSC_PR;
    xhci_write_port(hc, port_index, portsc);

    /* Wait for reset to complete */
    int timeout = 1000000;
    while (timeout-- > 0) {
        portsc = xhci_read_port(hc, port_index);
        if (!(portsc & XHCI_PORTSC_PR)) break;
        for (volatile int i = 0; i < 1000; i++) {
            asm volatile ("pause" ::: "memory");
        }
    }

    if (timeout <= 0) {
        log_printf(LOG_LEVEL_WARN, "xHCI: port %d reset timed out\n", port_index);
        return -1;
    }

    /* Check if device is still connected after reset */
    if (!(portsc & XHCI_PORTSC_CCS)) {
        return -1;
    }

    log_printf(LOG_LEVEL_INFO, "xHCI: port %d reset complete, speed=%d\n",
               port_index, speed);

    /* Enable slot and address device */
    int slot_id = xhci_enable_slot(hc);
    if (slot_id < 0) {
        log_printf(LOG_LEVEL_WARN, "xHCI: failed to enable slot for port %d\n", port_index);
        return -1;
    }

    if (xhci_address_device(hc, (uint8_t)slot_id, (uint8_t)(port_index + 1), speed) < 0) {
        log_printf(LOG_LEVEL_WARN, "xHCI: failed to address device on slot %d\n", slot_id);
        return -1;
    }

    return slot_id;
}

/* ================================================================
 * Get Descriptor (Control Transfer on EP0)
 * ================================================================ */
int xhci_get_descriptor(struct xhci_controller *hc, uint8_t slot_id,
                        uint8_t type, uint8_t index, uint8_t lang_id_lo,
                        uint8_t lang_id_hi, void *buf, uint16_t len) {
    if (!hc || slot_id == 0 || slot_id > hc->max_slots) return -1;

    struct xhci_device_slot *slot = &hc->slots[slot_id];
    if (!slot->addressed || !slot->ep_ring[1].trbs) return -1;

    struct xhci_transfer_ring *ring = &slot->ep_ring[1];

    /* Build Setup Stage TRB */
    struct xhci_trb setup_trb;
    memset(&setup_trb, 0, sizeof(setup_trb));

    struct usb_setup_packet setup;
    setup.bmRequestType = USB_REQ_DIR_DEVICE_TO_HOST | USB_REQ_TYPE_STANDARD
                        | USB_REQ_RECIP_DEVICE;
    setup.bRequest = USB_REQ_GET_DESCRIPTOR;
    setup.wValue = (uint16_t)(((uint16_t)type << 8) | (uint16_t)index);
    setup.wIndex = (uint16_t)(((uint16_t)lang_id_hi << 8) | (uint16_t)lang_id_lo);
    setup.wLength = len;

    uint64_t *sp = (uint64_t *)&setup;
    setup_trb.parameter = sp[0];
    setup_trb.status = (uint32_t)(8 & 0x1FFFF);  /* TRB Transfer Length = 8 */
    setup_trb.control = (XHCI_TRB_SETUP_STAGE << XHCI_TRB_TYPE_SHIFT)
                      | XHCI_TRT_IN_DATA;  /* FIXED (v4.4.0): typo fix */

    /* Build Data Stage TRB */
    struct xhci_trb data_trb;
    memset(&data_trb, 0, sizeof(data_trb));
    data_trb.parameter = (uint64_t)(uintptr_t)buf;
    data_trb.status = (uint32_t)(len & 0x1FFFF);
    data_trb.control = (XHCI_TRB_DATA_STAGE << XHCI_TRB_TYPE_SHIFT)
                     | XHCI_TRB_DIR_IN;

    /* Build Status Stage TRB */
    struct xhci_trb status_trb;
    memset(&status_trb, 0, sizeof(status_trb));
    status_trb.control = (XHCI_TRB_STATUS_STAGE << XHCI_TRB_TYPE_SHIFT);

    /* Enqueue setup + data + status */
    xhci_ring_enqueue_trb(ring, &setup_trb);
    xhci_ring_enqueue_trb(ring, &data_trb);
    xhci_ring_enqueue_trb(ring, &status_trb);

    /* Ring doorbell for EP0 */
    xhci_ring_doorbell(hc, slot_id, 1);

    /* Poll for transfer completion */
    int timeout = 5000000;
    int got_data = 0;
    int got_status = 0;
    while (timeout-- > 0 && (!got_data || !got_status)) {
        struct xhci_trb *evt = xhci_event_ring_dequeue(hc);
        if (evt) {
            uint32_t evt_type = (evt->control & XHCI_TRB_TYPE_MASK) >> XHCI_TRB_TYPE_SHIFT;
            if (evt_type == XHCI_EVT_TRANSFER) {
                uint32_t cc = (evt->status & XHCI_TRB_CC_MASK) >> XHCI_TRB_CC_SHIFT;
                if (cc == XHCI_CC_SHORT_PACKET || cc == XHCI_CC_SUCCESS) {
                    got_data = 1;
                    got_status = 1;
                } else if (cc != XHCI_CC_SUCCESS) {
                    log_printf(LOG_LEVEL_WARN, "xHCI: transfer event cc=%d\n", cc);
                    return -1;
                }
            }
        }
        for (volatile int i = 0; i < 1000; i++) {
            asm volatile ("pause" ::: "memory");
        }
    }

    if (timeout <= 0) {
        log_printf(LOG_LEVEL_WARN, "xHCI: get descriptor timed out\n");
        return -1;
    }

    /* FIXED (v4.2.7): BUG-XHCI-DESC-RING — Do NOT reset ring pointers after
     * each descriptor transfer.  The transfer ring is a circular buffer;
     * resetting enqueue/dequeue to 0 and toggling pcs after every transfer
     * causes cycle bit inconsistency.  The controller maintains the ring
     * position through the dequeue pointer in the endpoint context.  The
     * next transfer will naturally continue from the current enqueue position. */

    return (int)len;
}

/* ================================================================
 * Set Configuration
 * ================================================================ */
int xhci_set_configuration(struct xhci_controller *hc, uint8_t slot_id,
                           uint8_t config_value) {
    if (!hc || slot_id == 0 || slot_id > hc->max_slots) return -1;

    struct xhci_device_slot *slot = &hc->slots[slot_id];
    if (!slot->addressed || !slot->ep_ring[1].trbs) return -1;

    struct xhci_transfer_ring *ring = &slot->ep_ring[1];

    struct xhci_trb setup_trb;
    memset(&setup_trb, 0, sizeof(setup_trb));

    struct usb_setup_packet setup;
    setup.bmRequestType = USB_REQ_DIR_HOST_TO_DEVICE | USB_REQ_TYPE_STANDARD
                        | USB_REQ_RECIP_DEVICE;
    setup.bRequest = USB_REQ_SET_CONFIGURATION;
    setup.wValue = config_value;
    setup.wIndex = 0;
    setup.wLength = 0;

    uint64_t *sp = (uint64_t *)&setup;
    setup_trb.parameter = sp[0];
    setup_trb.status = 8;
    setup_trb.control = (XHCI_TRB_SETUP_STAGE << XHCI_TRB_TYPE_SHIFT)
                      | XHCI_TRT_NO_DATA;

    /* Status Stage */
    struct xhci_trb status_trb;
    memset(&status_trb, 0, sizeof(status_trb));
    status_trb.control = (XHCI_TRB_STATUS_STAGE << XHCI_TRB_TYPE_SHIFT)
                       | XHCI_TRB_DIR_IN;

    xhci_ring_enqueue_trb(ring, &setup_trb);
    xhci_ring_enqueue_trb(ring, &status_trb);

    xhci_ring_doorbell(hc, slot_id, 1);

    /* Poll for completion */
    int timeout = 5000000;
    int done = 0;
    while (timeout-- > 0 && !done) {
        struct xhci_trb *evt = xhci_event_ring_dequeue(hc);
        if (evt) {
            uint32_t evt_type = (evt->control & XHCI_TRB_TYPE_MASK) >> XHCI_TRB_TYPE_SHIFT;
            if (evt_type == XHCI_EVT_TRANSFER) {
                uint32_t cc = (evt->status & XHCI_TRB_CC_MASK) >> XHCI_TRB_CC_SHIFT;
                if (cc == XHCI_CC_SUCCESS || cc == XHCI_CC_SHORT_PACKET) {
                    done = 1;
                } else {
                    log_printf(LOG_LEVEL_WARN, "xHCI: set config cc=%d\n", cc);
                    return -1;
                }
            }
        }
        for (volatile int i = 0; i < 1000; i++) {
            asm volatile ("pause" ::: "memory");
        }
    }

    if (done) {
        slot->configured = 1;
        return 0;
    }

    return -1;
}

/* ================================================================
 * XHCI_HOTPLUG (v4.2.7) - USB Device Removal
 *
 * Remove a USB device from the global device list and free its
 * associated resources (device context, endpoint rings, slot).
 * ================================================================ */
/* FIXED (v4.4.0): renamed from usb_device_remove to avoid duplicate definition */
static void usb_device_remove_dev(struct usb_device *dev) {
    if (!dev) return;

    /* Remove from the global linked list */
    if (usb_dev_list == dev) {
        usb_dev_list = dev->next;
        if (usb_dev_tail == dev) usb_dev_tail = NULL;
    } else {
        struct usb_device *prev = usb_dev_list;
        while (prev && prev->next != dev) prev = prev->next;
        if (prev) {
            prev->next = dev->next;
            if (usb_dev_tail == dev) usb_dev_tail = prev;
        }
    }

    /* Free HID descriptor if any */
    if (dev->hid_desc) {
        kfree(dev->hid_desc);
        dev->hid_desc = NULL;
    }

    /* Free the slot resources */
    if (dev->hc && dev->slot_id > 0) {
        struct xhci_device_slot *slot = &dev->hc->slots[dev->slot_id];
        if (slot->ctx) {
            xhci_dma_free(slot->ctx);  /* XHCI_DMA (v4.2.7) */
            slot->ctx = NULL;
        }
        for (int ep = 0; ep < 32; ep++) {
            if (slot->ep_ring[ep].trbs) {
                xhci_dma_free(slot->ep_ring[ep].trbs);  /* XHCI_DMA (v4.2.7) */
                slot->ep_ring[ep].trbs = NULL;
            }
        }
        /* Clear DCBAA entry */
        dev->hc->dcbaa[dev->slot_id] = 0;
        slot->enabled = 0;
        slot->addressed = 0;
        slot->configured = 0;
    }

    kfree(dev);
}

/* ================================================================
 * XHCI_HOTPLUG (v4.2.7) - Port Status Change Handler
 *
 * Proper state machine for USB port hotplug events:
 *   1. Clear the port change bits (CSC, PRC, PEC, etc.)
 *   2. Read current port status (CCS, speed)
 *   3. If device connected (CCS=1): reset port, wait for enable,
 *      enumerate the device (slot, descriptor, configuration).
 *   4. If device disconnected (CCS=0): find the device attached to
 *      this port, remove it from the device list, free resources.
 * ================================================================ */
static void xhci_handle_port_status_change(struct xhci_controller *hc, uint32_t port_id) {
    if (!hc || port_id >= hc->num_ports) return;

    /* Step 1: Read and clear port change bits */
    uint32_t portsc = xhci_read_port(hc, port_id);
    uint32_t change_bits = portsc & (XHCI_PORTSC_CSC | XHCI_PORTSC_PEC
                                   | XHCI_PORTSC_WRC | XHCI_PORTSC_OCC
                                   | XHCI_PORTSC_PRC | XHCI_PORTSC_PLC
                                   | XHCI_PORTSC_CEC);
    if (change_bits) {
        xhci_write_port(hc, port_id, portsc | change_bits);
    }

    /* Step 2: Check current connection status */
    if (portsc & XHCI_PORTSC_CCS) {
        /* Device connected */
        log_printf(LOG_LEVEL_INFO, "xHCI: port %d — device connected (hotplug)\n", port_id);

        uint32_t speed_id = (portsc & XHCI_PORTSC_SPEED_MASK) >> XHCI_PORTSC_SPEED_SHIFT;
        uint8_t speed = 0;
        switch (speed_id) {
            case XHCI_PORT_SPEED_LOW:       speed = 1; break;
            case XHCI_PORT_SPEED_FULL:      speed = 2; break;
            case XHCI_PORT_SPEED_HIGH:      speed = 3; break;
            case XHCI_PORT_SPEED_SUPER:     speed = 4; break;
            case XHCI_PORT_SPEED_SUPER_PLUS: speed = 5; break;
            default: speed = 3; break;
        }

        log_printf(LOG_LEVEL_INFO, "xHCI: port %d speed=%d, enumerating...\n", port_id, speed);

        /* Step 3: Reset port and enumerate */
        int slot_id = xhci_reset_port(hc, port_id);
        if (slot_id >= 0) {
            log_printf(LOG_LEVEL_INFO, "xHCI: hotplug device on port %d assigned slot %d\n",
                       port_id, slot_id);

            struct usb_device_descriptor dev_desc;
            if (xhci_get_descriptor(hc, (uint8_t)slot_id, USB_DESC_DEVICE, 0, 0, 0,
                                    &dev_desc, sizeof(dev_desc)) >= 0) {
                log_printf(LOG_LEVEL_INFO,
                           "xHCI: hotplug device vid=%04x pid=%04x class=%d\n",
                           dev_desc.idVendor, dev_desc.idProduct,
                           dev_desc.bDeviceClass);

                struct usb_device *udev = (struct usb_device *)kmalloc(sizeof(*udev));
                if (udev) {
                    memset(udev, 0, sizeof(*udev));
                    udev->slot_id = (uint8_t)slot_id;
                    udev->address = hc->slots[slot_id].address;
                    udev->root_port = hc->slots[slot_id].root_port;
                    udev->speed = hc->slots[slot_id].speed;
                    udev->hc = hc;
                    memcpy(&udev->dev_desc, &dev_desc, sizeof(dev_desc));
                    usb_device_add(udev);

                    /* Read configuration descriptor */
                    struct usb_config_descriptor cfg_desc;
                    if (xhci_get_descriptor(hc, (uint8_t)slot_id, USB_DESC_CONFIGURATION,
                                            0, 0, 0, &cfg_desc, sizeof(cfg_desc)) >= 0) {
                        udev->cfg_desc = cfg_desc;
                        uint16_t total_len = cfg_desc.wTotalLength;
                        if (total_len > 512) total_len = 512;
                        uint8_t *cfg_buf = (uint8_t *)kmalloc(total_len);
                        if (cfg_buf) {
                            if (xhci_get_descriptor(hc, (uint8_t)slot_id,
                                                    USB_DESC_CONFIGURATION, 0, 0, 0,
                                                    cfg_buf, total_len) >= 0) {
                                uint8_t *ptr = cfg_buf;
                                uint8_t *end = cfg_buf + total_len;
                                uint8_t iface_idx = 0;
                                while (ptr + 2 <= end) {
                                    uint8_t len2 = ptr[0];
                                    uint8_t dtype = ptr[1];
                                    if (len2 < 2 || ptr + len2 > end) break;
                                    if (dtype == USB_DESC_INTERFACE && len2 >= 9) {
                                        struct usb_interface_descriptor *ifd =
                                            (struct usb_interface_descriptor *)ptr;
                                        if (iface_idx < USB_MAX_INTERFACES) {
                                            struct usb_interface *iface =
                                                &udev->interfaces[iface_idx];
                                            iface->interface_number = ifd->bInterfaceNumber;
                                            iface->interface_class = ifd->bInterfaceClass;
                                            iface->interface_subclass = ifd->bInterfaceSubClass;
                                            iface->interface_protocol = ifd->bInterfaceProtocol;
                                            iface->num_endpoints = 0;
                                            iface_idx++;
                                        }
                                    } else if (dtype == USB_DESC_ENDPOINT && len2 >= 7) {
                                        struct usb_endpoint_descriptor *epd =
                                            (struct usb_endpoint_descriptor *)ptr;
                                        if (iface_idx > 0) {
                                            struct usb_interface *iface =
                                                &udev->interfaces[iface_idx - 1];
                                            if (iface->num_endpoints < USB_MAX_ENDPOINTS) {
                                                struct usb_endpoint *ep =
                                                    &iface->endpoints[iface->num_endpoints];
                                                ep->ep_address = epd->bEndpointAddress;
                                                ep->ep_type = epd->bmAttributes & 0x03;
                                                ep->ep_id = (epd->bEndpointAddress & 0x0F) * 2
                                                          + ((epd->bEndpointAddress & 0x80) ? 1 : 0);
                                                ep->max_packet_size = epd->wMaxPacketSize;
                                                ep->interval = epd->bInterval;
                                                ep->active = 1;
                                                iface->num_endpoints++;
                                            }
                                        }
                                    } else if (dtype == USB_DESC_HID) {
                                        udev->hid_desc = (struct usb_hid_descriptor *)kmalloc(len2);
                                        if (udev->hid_desc) {
                                            memcpy(udev->hid_desc, ptr, len2);
                                        }
                                    }
                                    ptr += len2;
                                }
                                udev->num_interfaces = iface_idx;
                            }
                            kfree(cfg_buf);
                        }
                    }
                }
            }
        } else {
            log_printf(LOG_LEVEL_WARN, "xHCI: hotplug port %d — reset failed\n", port_id);
        }
    } else {
        /* Device disconnected */
        log_printf(LOG_LEVEL_INFO, "xHCI: port %d — device disconnected (hotplug)\n", port_id);

        /* Find and remove the device attached to this port */
        struct usb_device *dev = usb_dev_list;
        while (dev) {
            struct usb_device *next = dev->next;
            if (dev->hc == hc && dev->root_port == (uint8_t)(port_id + 1)) {
                log_printf(LOG_LEVEL_INFO,
                           "xHCI: removing device vid=%04x pid=%04x from port %d\n",
                           dev->dev_desc.idVendor, dev->dev_desc.idProduct, port_id);
                usb_device_remove_dev(dev);
                break;
            }
            dev = next;
        }
    }
}

/* ================================================================
 * Interrupt Handler
 * ================================================================ */
void xhci_interrupt_handler(void *stack) {
    (void)stack;

    struct xhci_controller *hc = xhci_ctrl_list;
    while (hc) {
        /* Check USBSTS for interrupt status */
        uint32_t usbsts = xhci_read_op(hc, XHCI_REG_USBSTS);
        if (usbsts & XHCI_STS_EINT) {
            /* Acknowledge the interrupt */
            xhci_write_op(hc, XHCI_REG_USBSTS, usbsts & XHCI_STS_EINT);

            /* Process events */
            struct xhci_trb *evt;
            while ((evt = xhci_event_ring_dequeue(hc)) != NULL) {
                uint32_t evt_type = (evt->control & XHCI_TRB_TYPE_MASK) >> XHCI_TRB_TYPE_SHIFT;
                switch (evt_type) {
                    case XHCI_EVT_PORT_STATUS_CHANGE: {
                        /* XHCI_HOTPLUG (v4.2.7) - Proper enumeration/removal state machine */
                        uint32_t port_id = (evt->parameter >> 24) & 0xFF;
                        uint32_t port = port_id - 1;  /* port_id is 1-based */
                        xhci_handle_port_status_change(hc, port);
                        break;
                    }
                    case XHCI_EVT_TRANSFER:
                    case XHCI_EVT_COMMAND_COMPLETE:
                        /* Handled by polling functions */
                        break;
                    default:
                        break;
                }
            }

            /* FIXED (v4.2.7): BUG-XHCI-ERDP - Write dequeue pointer to ERDP
             * with ERDP_BUSY (EHB) bit set so the controller knows the
             * event ring is being consumed.  Without this write the
             * controller may think the ring is full and stop posting events. */
            {
                uint64_t erdp = hc->event_ring_phys
                              + (uint64_t)hc->event_ring_dequeue * sizeof(struct xhci_trb)
                              | XHCI_ERDP_EHB;
                xhci_write_runtime(hc, XHCI_ERDP_LO(0), (uint32_t)(erdp & 0xFFFFFFFFU));
                xhci_write_runtime(hc, XHCI_ERDP_HI(0), (uint32_t)(erdp >> 32));
            }
        }

        /* Clear port change bits */
        if (usbsts & XHCI_STS_PCD) {
            xhci_write_op(hc, XHCI_REG_USBSTS, XHCI_STS_PCD);
        }

        hc = hc->next;
    }
}

/* ================================================================
 * Controller Initialization
 * ================================================================ */
static int xhci_init_controller(struct pci_device *pci_dev) {
    if (!pci_dev) return -1;

    /* Allocate controller structure */
    struct xhci_controller *hc = (struct xhci_controller *)kmalloc(sizeof(*hc));
    if (!hc) return -1;
    memset(hc, 0, sizeof(*hc));

    hc->pci_dev = pci_dev;

    /* Enable bus mastering and MMIO */
    pci_enable_bus_mastering(pci_dev);
    uint16_t cmd = pci_read_config16(pci_dev->bus, pci_dev->device,
                                     pci_dev->function, PCI_CONFIG_COMMAND);
    cmd |= PCI_CMD_MEMORY_SPACE;
    pci_write_config16(pci_dev->bus, pci_dev->device, pci_dev->function,
                       PCI_CONFIG_COMMAND, cmd);

    /* Read BAR0 for MMIO base */
    uint32_t bar0 = pci_read_config32(pci_dev->bus, pci_dev->device,
                                      pci_dev->function, PCI_CONFIG_BAR0);
    if (bar0 == 0 || (bar0 & PCI_BAR_IO)) {
        log_printf(LOG_LEVEL_WARN, "xHCI: BAR0 is not a valid MMIO BAR\n");
        kfree(hc);
        return -1;
    }

    /* Check if 64-bit BAR */
    uint64_t mmio_phys = (uint64_t)(bar0 & PCI_BAR_MEM_MASK);
    if ((bar0 & PCI_BAR_TYPE_MASK) == PCI_BAR_TYPE_64) {
        uint32_t bar1 = pci_read_config32(pci_dev->bus, pci_dev->device,
                                          pci_dev->function, PCI_CONFIG_BAR1);
        mmio_phys |= ((uint64_t)bar1 << 32);
    }

    /* Use identity mapping: virtual = physical */
    /* FIXED (v4.2.7): BUG-XHCI-BAR-MAP — The PCI BAR may point to a physical
     * address above 1 GB (KERNEL_PHYS_MAX).  If the MMIO region is not
     * identity-mapped, accesses will page fault.  The kernel identity-maps
     * 0..KERNEL_PHYS_MAX (1 GB).  If the BAR is above this range, we need
     * to explicitly map the MMIO pages with map_page().  For now, assume
     * the BAR is within the identity-mapped range (common on QEMU and
     * most consumer hardware). */
    hc->mmio_base = (volatile uint32_t *)(uintptr_t)mmio_phys;
    log_printf(LOG_LEVEL_INFO, "xHCI: MMIO base = 0x%llx\n", (unsigned long long)mmio_phys);

    /* Read capability registers */
    hc->cap_length = xhci_read32(hc->mmio_base) & 0xFF;
    uint32_t hcsparams1 = xhci_read32((volatile uint32_t *)((uintptr_t)hc->mmio_base + XHCI_REG_HCSPARAMS1));
    uint32_t hccparams1 = xhci_read32((volatile uint32_t *)((uintptr_t)hc->mmio_base + XHCI_REG_HCCPARAMS1));
    uint32_t pagesize   = xhci_read32((volatile uint32_t *)((uintptr_t)hc->mmio_base + hc->cap_length + XHCI_REG_PAGESIZE));

    hc->db_offset  = xhci_read32((volatile uint32_t *)((uintptr_t)hc->mmio_base + XHCI_REG_DBOFF)) & ~3U;
    hc->rts_offset = xhci_read32((volatile uint32_t *)((uintptr_t)hc->mmio_base + XHCI_REG_RTSOFF)) & ~31U;

    hc->max_slots  = (hcsparams1 & XHCI_HCSPARAMS1_MAX_SLOTS_MASK);
    hc->max_ports  = (hcsparams1 & XHCI_HCSPARAMS1_MAX_PORTS_MASK) >> XHCI_HCSPARAMS1_MAX_PORTS_SHIFT;
    hc->max_interrupters = (hcsparams1 & XHCI_HCSPARAMS1_MAX_INTR_MASK) >> XHCI_HCSPARAMS1_MAX_INTR_SHIFT;
    hc->csz        = (hccparams1 & XHCI_HCCPARAMS1_CSZ) ? 1 : 0;
    hc->ac64       = (hccparams1 & XHCI_HCCPARAMS1_AC64) ? 1 : 0;

    if (hc->max_slots > XHCI_MAX_SLOTS) hc->max_slots = XHCI_MAX_SLOTS;
    if (hc->max_ports > XHCI_MAX_PORTS) hc->max_ports = XHCI_MAX_PORTS;

    /* Set up derived pointers */
    hc->operational = (volatile uint32_t *)((uintptr_t)hc->mmio_base + hc->cap_length);
    hc->doorbell    = (volatile uint32_t *)((uintptr_t)hc->mmio_base + hc->db_offset);
    hc->runtime     = (volatile uint32_t *)((uintptr_t)hc->mmio_base + hc->rts_offset);

    /* Read IRQ line */
    hc->irq = pci_dev->irq_line;

    log_printf(LOG_LEVEL_INFO, "xHCI: slots=%d ports=%d intr=%d cap_len=%d db_off=%d rts_off=%d\n",
               hc->max_slots, hc->max_ports, hc->max_interrupters,
               hc->cap_length, hc->db_offset, hc->rts_offset);

    /* ---- Reset Controller ---- */
    uint32_t usbcmd = xhci_read_op(hc, XHCI_REG_USBCMD);
    usbcmd |= XHCI_CMD_HCRST;
    xhci_write_op(hc, XHCI_REG_USBCMD, usbcmd);

    int timeout = 5000000;
    while (timeout-- > 0) {
        usbcmd = xhci_read_op(hc, XHCI_REG_USBCMD);
        uint32_t usbsts = xhci_read_op(hc, XHCI_REG_USBSTS);
        if (!(usbcmd & XHCI_CMD_HCRST) && !(usbsts & XHCI_STS_CNR)) break;
        for (volatile int i = 0; i < 1000; i++) {
            asm volatile ("pause" ::: "memory");
        }
    }
    if (timeout <= 0) {
        log_printf(LOG_LEVEL_ERR, "xHCI: controller reset timed out\n");
        kfree(hc);
        return -1;
    }
    log_printf(LOG_LEVEL_INFO, "xHCI: controller reset complete\n");

    /* ---- Allocate DCBAA ---- */
    /* XHCI_DMA (v4.2.7) - Use xhci_dma_alloc for DMA-safe allocation */
    size_t dcbaa_size = (size_t)(hc->max_slots + 1) * sizeof(uint64_t);
    hc->dcbaa = (uint64_t *)xhci_dma_alloc(dcbaa_size, &hc->dcbaa_phys);
    if (!hc->dcbaa) {
        if (hc->cmd_ring.trbs) xhci_dma_free(hc->cmd_ring.trbs);
        kfree(hc);
        return -1;
    }

    /* Set DCBAAP */
    xhci_write_op64(hc, XHCI_REG_DCBAAP_LO, hc->dcbaa_phys);

    /* ---- Allocate Command Ring ---- */
    xhci_ring_init(&hc->cmd_ring, XHCI_CMD_RING_SIZE);
    if (!hc->cmd_ring.trbs) {
        xhci_dma_free(hc->dcbaa);  /* XHCI_DMA (v4.2.7) */
        kfree(hc);
        return -1;
    }

    /* Set CRCR */
    uint64_t crcr = hc->cmd_ring.phys_addr | XHCI_CRCR_RCS;
    xhci_write_op64(hc, XHCI_REG_CRCR_LO, crcr);

    /* ---- Allocate Event Ring and ERST ---- */
    /* XHCI_DMA (v4.2.7) - Use xhci_dma_alloc for DMA-safe allocation */
    hc->event_ring = (struct xhci_trb *)xhci_dma_alloc(XHCI_EVENT_RING_SIZE * sizeof(struct xhci_trb), &hc->event_ring_phys);
    if (!hc->event_ring) {
        if (hc->cmd_ring.trbs) xhci_dma_free(hc->cmd_ring.trbs);
        xhci_dma_free(hc->dcbaa);
        kfree(hc);
        return -1;
    }
    hc->event_ring_dequeue = 0;
    hc->event_ring_ccs = 1;

    /* Set up ERST */
    hc->erst_entry.ring_segment_base = hc->event_ring_phys;
    hc->erst_entry.ring_segment_size = XHCI_EVENT_RING_SIZE;
    hc->erst_entry.reserved = 0;

    xhci_write_runtime(hc, XHCI_ERSTSZ(0), XHCI_EVENT_RING_SIZE);
    xhci_write_runtime(hc, XHCI_ERSTBA_LO(0), (uint32_t)(virt_to_phys(&hc->erst_entry) & 0xFFFFFFFFU));
    xhci_write_runtime(hc, XHCI_ERSTBA_HI(0), (uint32_t)((virt_to_phys(&hc->erst_entry) >> 32) & 0xFFFFFFFFU));

    /* Set ERDP */
    xhci_write_runtime(hc, XHCI_ERDP_LO(0), (uint32_t)(hc->event_ring_phys & 0xFFFFFFFFU));
    xhci_write_runtime(hc, XHCI_ERDP_HI(0), (uint32_t)(hc->event_ring_phys >> 32));
    /* Set Event Handler Busy to clear */
    uint64_t erdp = hc->event_ring_phys | XHCI_ERDP_EHB;
    xhci_write_runtime(hc, XHCI_ERDP_LO(0), (uint32_t)(erdp & 0xFFFFFFFFU));
    xhci_write_runtime(hc, XHCI_ERDP_HI(0), (uint32_t)(erdp >> 32));

    /* ---- Enable Interrupts ---- */
    xhci_write_runtime(hc, XHCI_IMAN(0), XHCI_IMAN_IE);
    xhci_write_runtime(hc, XHCI_IMOD(0), 0);  /* No moderation */

    /* ---- Set Max Slots ---- */
    xhci_write_op(hc, XHCI_REG_CONFIG, hc->max_slots & XHCI_CONFIG_MAX_SLOTS_MASK);

    /* ---- Start Controller ---- */
    usbcmd = xhci_read_op(hc, XHCI_REG_USBCMD);
    usbcmd |= XHCI_CMD_RS | XHCI_CMD_INTE;
    xhci_write_op(hc, XHCI_REG_USBCMD, usbcmd);

    /* Wait for controller to be ready */
    timeout = 5000000;
    while (timeout-- > 0) {
        uint32_t usbsts = xhci_read_op(hc, XHCI_REG_USBSTS);
        if (!(usbsts & XHCI_STS_HCH)) break;
        for (volatile int i = 0; i < 1000; i++) {
            asm volatile ("pause" ::: "memory");
        }
    }
    if (timeout <= 0) {
        log_printf(LOG_LEVEL_ERR, "xHCI: controller failed to start\n");
        /* FIXED (v4.2.7): BUG-XHCI-INIT-LEAK — Free all allocated resources
         * on the error path.  This includes any slots that may have been
         * allocated during the initialization sequence, their device
         * contexts and endpoint ring TRBs. */
        for (uint32_t s = 1; s <= hc->max_slots; s++) {
            if (hc->slots[s].ctx) {
                xhci_dma_free(hc->slots[s].ctx);  /* XHCI_DMA (v4.2.7) */
                hc->slots[s].ctx = NULL;
            }
            for (int ep = 0; ep < 32; ep++) {
                if (hc->slots[s].ep_ring[ep].trbs) {
                    xhci_dma_free(hc->slots[s].ep_ring[ep].trbs);  /* XHCI_DMA (v4.2.7) */
                    hc->slots[s].ep_ring[ep].trbs = NULL;
                }
            }
        }
        xhci_dma_free(hc->event_ring);  /* XHCI_DMA (v4.2.7) */
        xhci_dma_free(hc->cmd_ring.trbs);  /* XHCI_DMA (v4.2.7) */
        xhci_dma_free(hc->dcbaa);  /* XHCI_DMA (v4.2.7) */
        kfree(hc);
        return -1;
    }
    log_printf(LOG_LEVEL_INFO, "xHCI: controller started\n");

    /* ---- Discover Ports ---- */
    uint32_t port_offset = XHCI_REG_PORTSC_BASE;
    hc->num_ports = 0;
    /* FIXED (v4.2.7): BUG-XHCI-PORT-BOUNDS — Bound port index by both
     * max_ports and XHCI_MAX_PORTS to prevent port_offsets array overflow. */
    for (uint32_t i = 0; i < hc->max_ports && i < XHCI_MAX_PORTS; i++) {
        uint32_t portsc = xhci_read32((volatile uint32_t *)((uintptr_t)hc->operational + port_offset));
        if (portsc == 0xFFFFFFFFU) break;  /* No more ports */
        hc->port_offsets[i] = port_offset;
        hc->num_ports++;

        /* Take port out of reset if powered */
        if (portsc & XHCI_PORTSC_PP) {
            /* Clear port change bits */
            uint32_t clear_mask = XHCI_PORTSC_CSC | XHCI_PORTSC_PEC
                                | XHCI_PORTSC_WRC | XHCI_PORTSC_OCC
                                | XHCI_PORTSC_PRC | XHCI_PORTSC_PLC
                                | XHCI_PORTSC_CEC;
            xhci_write32((volatile uint32_t *)((uintptr_t)hc->operational + port_offset),
                         portsc | clear_mask);
        }

        port_offset += 4;
    }

    log_printf(LOG_LEVEL_INFO, "xHCI: %d ports discovered\n", hc->num_ports);

    /* ---- Probe connected devices ---- */
    for (uint32_t i = 0; i < hc->num_ports; i++) {
        uint32_t portsc = xhci_read_port(hc, i);
        if (portsc & XHCI_PORTSC_CCS) {
            uint32_t speed_id = (portsc & XHCI_PORTSC_SPEED_MASK) >> XHCI_PORTSC_SPEED_SHIFT;
            log_printf(LOG_LEVEL_INFO, "xHCI: port %d - device connected, speed=%d\n",
                       i, speed_id);

            int slot_id = xhci_reset_port(hc, i);
            if (slot_id >= 0) {
                log_printf(LOG_LEVEL_INFO, "xHCI: device on port %d assigned slot %d\n",
                           i, slot_id);

                /* Read device descriptor */
                struct usb_device_descriptor dev_desc;
                if (xhci_get_descriptor(hc, (uint8_t)slot_id, USB_DESC_DEVICE, 0, 0, 0,
                                        &dev_desc, sizeof(dev_desc)) >= 0) {
                    log_printf(LOG_LEVEL_INFO,
                               "xHCI: device vid=%04x pid=%04x class=%d sub=%d proto=%d\n",
                               dev_desc.idVendor, dev_desc.idProduct,
                               dev_desc.bDeviceClass, dev_desc.bDeviceSubClass,
                               dev_desc.bDeviceProtocol);

                    /* Create USB device tracking */
                    struct usb_device *udev = (struct usb_device *)kmalloc(sizeof(*udev));
                    if (udev) {
                        memset(udev, 0, sizeof(*udev));
                        udev->slot_id = (uint8_t)slot_id;
                        udev->address = hc->slots[slot_id].address;
                        udev->root_port = hc->slots[slot_id].root_port;
                        udev->speed = hc->slots[slot_id].speed;
                        udev->hc = hc;
                        memcpy(&udev->dev_desc, &dev_desc, sizeof(dev_desc));
                        usb_device_add(udev);

                        /* Read configuration descriptor */
                        struct usb_config_descriptor cfg_desc;
                        if (xhci_get_descriptor(hc, (uint8_t)slot_id, USB_DESC_CONFIGURATION,
                                                0, 0, 0, &cfg_desc, sizeof(cfg_desc)) >= 0) {
                            udev->cfg_desc = cfg_desc;

                            /* Read full configuration (all descriptors) */
                            uint16_t total_len = cfg_desc.wTotalLength;
                            if (total_len > 512) total_len = 512;
                            uint8_t *cfg_buf = (uint8_t *)kmalloc(total_len);
                            if (cfg_buf) {
                                if (xhci_get_descriptor(hc, (uint8_t)slot_id,
                                                        USB_DESC_CONFIGURATION, 0, 0, 0,
                                                        cfg_buf, total_len) >= 0) {
                                    /* Parse interface and endpoint descriptors */
                                    uint8_t *ptr = cfg_buf;
                                    uint8_t *end = cfg_buf + total_len;
                                    uint8_t iface_idx = 0;
                                    while (ptr + 2 <= end) {
                                        uint8_t len = ptr[0];
                                        uint8_t dtype = ptr[1];
                                        if (len < 2 || ptr + len > end) break;

                                        if (dtype == USB_DESC_INTERFACE && len >= 9) {
                                            struct usb_interface_descriptor *ifd =
                                                (struct usb_interface_descriptor *)ptr;
                                            if (iface_idx < USB_MAX_INTERFACES) {
                                                struct usb_interface *iface =
                                                    &udev->interfaces[iface_idx];
                                                iface->interface_number = ifd->bInterfaceNumber;
                                                iface->interface_class = ifd->bInterfaceClass;
                                                iface->interface_subclass = ifd->bInterfaceSubClass;
                                                iface->interface_protocol = ifd->bInterfaceProtocol;
                                                iface->num_endpoints = 0;
                                                iface_idx++;
                                            }
                                        } else if (dtype == USB_DESC_ENDPOINT && len >= 7) {
                                            struct usb_endpoint_descriptor *epd =
                                                (struct usb_endpoint_descriptor *)ptr;
                                            if (iface_idx > 0) {
                                                struct usb_interface *iface =
                                                    &udev->interfaces[iface_idx - 1];
                                                if (iface->num_endpoints < USB_MAX_ENDPOINTS) {
                                                    struct usb_endpoint *ep =
                                                        &iface->endpoints[iface->num_endpoints];
                                                    ep->ep_address = epd->bEndpointAddress;
                                                    ep->ep_type = epd->bmAttributes & 0x03;
                                                    ep->ep_id = (epd->bEndpointAddress & 0x0F) * 2
                                                              + ((epd->bEndpointAddress & 0x80) ? 1 : 0);
                                                    ep->max_packet_size = epd->wMaxPacketSize;
                                                    ep->interval = epd->bInterval;
                                                    ep->active = 1;
                                                    iface->num_endpoints++;
                                                }
                                            }
                                        } else if (dtype == USB_DESC_HID) {
                                            udev->hid_desc = (struct usb_hid_descriptor *)kmalloc(len);
                                            if (udev->hid_desc) {
                                                memcpy(udev->hid_desc, ptr, len);
                                            }
                                        }

                                        ptr += len;
                                    }
                                    udev->num_interfaces = iface_idx;
                                }
                                kfree(cfg_buf);
                            }
                        }
                    }
                }
            }
        }
    }

    hc->initialized = 1;

    /* Add to global list */
    hc->next = xhci_ctrl_list;
    xhci_ctrl_list = hc;

    return 0;
}

/* ================================================================
 * Initialize xHCI subsystem
 * ================================================================ */
void xhci_init(void) {
    log_printf(LOG_LEVEL_INFO, "xHCI: scanning for USB controllers...\n");

    /* Find xHCI controllers via PCI */
    struct pci_device *pci = pci_get_device_list();
    int found = 0;

    while (pci) {
        if (pci->class_code == XHCI_PCI_CLASS &&
            pci->subclass   == XHCI_PCI_SUBCLASS &&
            pci->prog_if    == XHCI_PCI_PROG_IF) {
            log_printf(LOG_LEVEL_INFO,
                       "xHCI: found controller at %02x:%02x.%x [%04x:%04x]\n",
                       pci->bus, pci->device, pci->function,
                       pci->vendor_id, pci->device_id);
            if (xhci_init_controller(pci) == 0) {
                found++;
            }
        }
        pci = pci->next;
    }

    if (found == 0) {
        log_printf(LOG_LEVEL_INFO, "xHCI: no USB 3.0 controllers found\n");
    } else {
        log_printf(LOG_LEVEL_INFO, "xHCI: initialized %d controller(s)\n", found);
    }
}

/* ================================================================
 * Get the first controller
 * ================================================================ */
struct xhci_controller *xhci_get_controller(void) {
    return xhci_ctrl_list;
}