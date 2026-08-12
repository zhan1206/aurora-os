/*
 * hid.c - USB HID (Human Interface Device) driver
 *
 * Supports keyboard and mouse boot protocol devices.  Parses HID
 * descriptors, configures interrupt endpoints, and translates
 * USB HID key codes to PS/2 scancodes for the console input system.
 *
 * /* FIXED (v4.4.0): USB (v4.2.6) */
 */
#include "hid.h"
#include "xhci.h"
#include "usb.h"
#include "../include/log.h"
#include "../include/string.h"
#include "../include/arch.h"
#include "../console.h"
#include "../mem.h"
#include <stdint.h>

/* ================================================================
 * USB HID Key Code → ASCII Character Translation Table
 *
 * Maps USB HID boot protocol keyboard usage IDs to ASCII characters
 * (unshifted).  Entries with value 0 map to special keys.
 * ================================================================ */
static const uint8_t hid_key_to_ascii[232] = {
    /* 0x00-0x03: Reserved */
    0,   0,   0,   0,
    /* 0x04: a */ 'a',
    /* 0x05: b */ 'b',
    /* 0x06: c */ 'c',
    /* 0x07: d */ 'd',
    /* 0x08: e */ 'e',
    /* 0x09: f */ 'f',
    /* 0x0A: g */ 'g',
    /* 0x0B: h */ 'h',
    /* 0x0C: i */ 'i',
    /* 0x0D: j */ 'j',
    /* 0x0E: k */ 'k',
    /* 0x0F: l */ 'l',
    /* 0x10: m */ 'm',
    /* 0x11: n */ 'n',
    /* 0x12: o */ 'o',
    /* 0x13: p */ 'p',
    /* 0x14: q */ 'q',
    /* 0x15: r */ 'r',
    /* 0x16: s */ 's',
    /* 0x17: t */ 't',
    /* 0x18: u */ 'u',
    /* 0x19: v */ 'v',
    /* 0x1A: w */ 'w',
    /* 0x1B: x */ 'x',
    /* 0x1C: y */ 'y',
    /* 0x1D: z */ 'z',
    /* 0x1E: 1 */ '1',
    /* 0x1F: 2 */ '2',
    /* 0x20: 3 */ '3',
    /* 0x21: 4 */ '4',
    /* 0x22: 5 */ '5',
    /* 0x23: 6 */ '6',
    /* 0x24: 7 */ '7',
    /* 0x25: 8 */ '8',
    /* 0x26: 9 */ '9',
    /* 0x27: 0 */ '0',
    /* 0x28: Enter */ '\n',
    /* 0x29: Escape */ 27,
    /* 0x2A: Backspace */ '\b',
    /* 0x2B: Tab */ '\t',
    /* 0x2C: Space */ ' ',
    /* 0x2D: - _ */ '-',
    /* 0x2E: = + */ '=',
    /* 0x2F: [ { */ '[',
    /* 0x30: ] } */ ']',
    /* 0x31: \ | */ '\\',
    /* 0x32: Non-US # ~ */ '\\',
    /* 0x33: ; : */ ';',
    /* 0x34: ' " */ '\'',
    /* 0x35: ` ~ */ '`',
    /* 0x36: , < */ ',',
    /* 0x37: . > */ '.',
    /* 0x38: / ? */ '/',
    /* 0x39-0xE7: Special keys (0) */ 0,
};

/* Shifted characters for HID keys 0x04-0x38 */
static const uint8_t hid_key_shifted[] = {
    /* 0x04: a */ 'A',  /* 0x05: b */ 'B',  /* 0x06: c */ 'C',
    /* 0x07: d */ 'D',  /* 0x08: e */ 'E',  /* 0x09: f */ 'F',
    /* 0x0A: g */ 'G',  /* 0x0B: h */ 'H',  /* 0x0C: i */ 'I',
    /* 0x0D: j */ 'J',  /* 0x0E: k */ 'K',  /* 0x0F: l */ 'L',
    /* 0x10: m */ 'M',  /* 0x11: n */ 'N',  /* 0x12: o */ 'O',
    /* 0x13: p */ 'P',  /* 0x14: q */ 'Q',  /* 0x15: r */ 'R',
    /* 0x16: s */ 'S',  /* 0x17: t */ 'T',  /* 0x18: u */ 'U',
    /* 0x19: v */ 'V',  /* 0x1A: w */ 'W',  /* 0x1B: x */ 'X',
    /* 0x1C: y */ 'Y',  /* 0x1D: z */ 'Z',
    /* 0x1E: 1 */ '!',  /* 0x1F: 2 */ '@',  /* 0x20: 3 */ '#',
    /* 0x21: 4 */ '$',  /* 0x22: 5 */ '%',  /* 0x23: 6 */ '^',
    /* 0x24: 7 */ '&',  /* 0x25: 8 */ '*',  /* 0x26: 9 */ '(',
    /* 0x27: 0 */ ')',  /* 0x28: Enter */ '\n', /* 0x29: Esc */ 27,
    /* 0x2A: BS */ '\b',/* 0x2B: Tab */ '\t', /* 0x2C: Space */ ' ',
    /* 0x2D: - */ '_',  /* 0x2E: = */ '+',    /* 0x2F: [ */ '{',
    /* 0x30: ] */ '}',  /* 0x31: \ */ '|',    /* 0x32: Non-US */ '|',
    /* 0x33: ; */ ':',  /* 0x34: ' */ '"',    /* 0x35: ` */ '~',
    /* 0x36: , */ '<',  /* 0x37: . */ '>',    /* 0x38: / */ '?',
};

/* ================================================================
 * Send ANSI escape sequence for special keys
 * ================================================================ */
static void hid_send_ansi(const char *seq) {
    for (; *seq; seq++) console_input_char(*seq);
}

/* ================================================================
 * Handle a special key (HID code → ANSI escape sequence)
 * ================================================================ */
static void hid_handle_special_key(uint8_t hid_code) {
    switch (hid_code) {
        case 0x4F: hid_send_ansi("\x1b[C");  break; /* Right Arrow */
        case 0x50: hid_send_ansi("\x1b[D");  break; /* Left Arrow */
        case 0x51: hid_send_ansi("\x1b[B");  break; /* Down Arrow */
        case 0x52: hid_send_ansi("\x1b[A");  break; /* Up Arrow */
        case 0x4A: hid_send_ansi("\x1b[H");  break; /* Home */
        case 0x4D: hid_send_ansi("\x1b[F");  break; /* End */
        case 0x4B: /* Page Up */    break;
        case 0x4E: /* Page Down */  break;
        case 0x49: /* Insert */     break;
        case 0x4C: hid_send_ansi("\x1b[3~"); break; /* Delete */
        default:   break;
    }
}

/* ================================================================
 * Global HID Device List
 * ================================================================ */
static struct hid_device *hid_device_list = NULL;

/* ================================================================
 * Parse Keyboard Boot Protocol Report
 * ================================================================ */
void hid_keyboard_report(struct hid_device *hid, const uint8_t *report,
                         int length) {
    if (!hid || !report || length < HID_KEYBOARD_REPORT_SIZE) return;

    uint8_t modifiers = report[0];
    const uint8_t *keys = &report[2];

    /* Determine shift state */
    int shifted = (modifiers & (HID_MOD_LSHIFT | HID_MOD_RSHIFT)) != 0;
    int ctrl = (modifiers & (HID_MOD_LCTRL | HID_MOD_RCTRL)) != 0;

    /* Track modifier changes — only process key press/release here */
    uint8_t modifier_changed = modifiers ^ hid->prev_modifiers;
    (void)modifier_changed;  /* Modifier changes are tracked but not sent as scancodes */

    /* Detect newly pressed keys (present in current, not in previous) */
    for (int i = 0; i < 6; i++) {
        uint8_t key = keys[i];
        if (key == 0) continue;

        int was_pressed = 0;
        for (int j = 0; j < 6; j++) {
            if (hid->prev_keys[j] == key) {
                was_pressed = 1;
                break;
            }
        }

        if (!was_pressed) {
            /* Key newly pressed */
            if (key >= 0x04 && key <= 0x38) {
                /* Printable character range */
                char ch;
                if (shifted && (key - 0x04) < (int)sizeof(hid_key_shifted)) {
                    ch = (char)hid_key_shifted[key - 0x04];
                } else {
                    ch = (char)hid_key_to_ascii[key];
                }
                if (ch && ctrl) {
                    /* Ctrl+letter → ASCII control character */
                    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 1);
                    else if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 1);
                    else if (ch >= '[' && ch <= '_') ch = (char)(ch - '[' + 27);
                }
                if (ch) {
                    console_input_char(ch);
                }
            } else if (key >= 0x39 && key < HID_KEY_MAX) {
                /* Special keys */
                /* Some special keys are in the ASCII table (Enter, Esc, BS, Tab, Space) */
                char ch = (char)hid_key_to_ascii[key];
                if (ch) {
                    console_input_char(ch);
                } else {
                    hid_handle_special_key(key);
                }
            }
        }
    }

    /* Save current key state */
    for (int i = 0; i < 6; i++) {
        hid->prev_keys[i] = keys[i];
    }
    hid->prev_modifiers = modifiers;
}

/* ================================================================
 * Parse Mouse Boot Protocol Report
 * ================================================================ */
void hid_mouse_report(struct hid_device *hid, const uint8_t *report,
                      int length) {
    if (!hid || !report || length < HID_MOUSE_REPORT_SIZE) return;
    (void)hid;
    (void)report;
    (void)length;

    /* Mouse report handling:
     *   report[0] = buttons
     *   report[1] = X movement (signed)
     *   report[2] = Y movement (signed)
     *
     * For now, mouse events are not routed to the console since
     * the console is currently keyboard-only.  This is a stub
     * that can be extended when graphical mouse support is added.
     */
}

/* ================================================================
 * Probe a USB HID Device
 * ================================================================ */
void hid_probe(struct usb_device *dev) {
    if (!dev) return;

    /* Check if this is a HID device */
    int is_hid = 0;
    for (int i = 0; i < dev->num_interfaces; i++) {
        if (dev->interfaces[i].interface_class == USB_CLASS_HID) {
            is_hid = 1;
            break;
        }
    }

    if (!is_hid && dev->dev_desc.bDeviceClass != USB_CLASS_HID) {
        return;
    }

    /* Allocate HID device structure */
    struct hid_device *hid = (struct hid_device *)kmalloc(sizeof(*hid));
    if (!hid) return;
    memset(hid, 0, sizeof(*hid));

    hid->usb_dev = dev;

    /* Determine protocol */
    for (int i = 0; i < dev->num_interfaces; i++) {
        struct usb_interface *iface = &dev->interfaces[i];
        if (iface->interface_class == USB_CLASS_HID) {
            hid->protocol = iface->interface_protocol;
            break;
        }
    }

    /* If no protocol specified via interface, check device descriptor */
    if (hid->protocol == 0) {
        hid->protocol = dev->dev_desc.bDeviceProtocol;
    }

    log_printf(LOG_LEVEL_INFO,
               "HID: probing device vid=%04x pid=%04x protocol=%d\n",
               dev->dev_desc.idVendor, dev->dev_desc.idProduct,
               hid->protocol);

    /* Find interrupt IN endpoint */
    for (int i = 0; i < dev->num_interfaces; i++) {
        struct usb_interface *iface = &dev->interfaces[i];
        if (iface->interface_class == USB_CLASS_HID) {
            for (int j = 0; j < iface->num_endpoints; j++) {
                struct usb_endpoint *ep = &iface->endpoints[j];
                if (ep->ep_type == USB_EP_TYPE_INTERRUPT &&
                    (ep->ep_address & USB_EP_DIR_IN)) {
                    hid->ep_interrupt = ep->ep_address;
                    hid->ep_id = ep->ep_id;
                    hid->max_packet_size = ep->max_packet_size;
                    hid->interval = ep->interval;
                    log_printf(LOG_LEVEL_DEBUG,
                               "HID: interrupt IN EP addr=0x%02x id=%d mps=%d interval=%d\n",
                               hid->ep_interrupt, hid->ep_id,
                               hid->max_packet_size, hid->interval);
                    break;
                }
            }
            break;
        }
    }

    /* Set configuration if not already configured */
    if (!dev->configured && dev->hc) {
        if (xhci_set_configuration(dev->hc, dev->slot_id,
                                   dev->cfg_desc.bConfigurationValue) == 0) {
            dev->configured = 1;
        }
    }

    /* Configure interrupt endpoint */
    if (hid->ep_id > 0 && dev->hc) {
        int ep_type = (hid->ep_interrupt & USB_EP_DIR_IN)
                      ? XHCI_EP_TYPE_INTERRUPT_IN
                      : XHCI_EP_TYPE_INTERRUPT_OUT;
        if (xhci_configure_endpoint(dev->hc, dev->slot_id, hid->ep_id,
                                    ep_type, hid->max_packet_size,
                                    hid->interval) == 0) {
            log_printf(LOG_LEVEL_DEBUG,
                       "HID: interrupt EP configured (slot=%d, ep=%d)\n",
                       dev->slot_id, hid->ep_id);

            /* Allocate report buffer */
            hid->report_buf = (uint8_t *)kmalloc(hid->max_packet_size);
            if (hid->report_buf) {
                memset(hid->report_buf, 0, hid->max_packet_size);
            }
        }
    }

    /* FIXED (v4.2.7): BUG-HID-INIT — Only mark the device as initialized
     * if the report buffer was successfully allocated.  Previously
     * initialized was set to 1 even when kmalloc failed, causing the
     * poll function to dereference a NULL report_buf. */
    if (hid->report_buf) {
        hid->initialized = 1;
    } else {
        hid->initialized = 0;
    }

    /* Add to global list (even if uninitialized — poll will skip it) */
    hid->next = hid_device_list;
    hid_device_list = hid;

    if (hid->protocol == USB_PROTOCOL_HID_KEYBOARD) {
        log_printf(LOG_LEVEL_INFO, "HID: keyboard ready\n");
    } else if (hid->protocol == USB_PROTOCOL_HID_MOUSE) {
        log_printf(LOG_LEVEL_INFO, "HID: mouse ready\n");
    } else {
        log_printf(LOG_LEVEL_INFO, "HID: device ready (protocol=%d)\n",
                   hid->protocol);
    }
}

/* ================================================================
 * Poll HID Devices for Reports
 * ================================================================ */
void hid_poll(void) {
    struct hid_device *hid = hid_device_list;
    while (hid) {
        if (hid->initialized && hid->usb_dev && hid->usb_dev->hc
            && hid->ep_id > 0 && hid->report_buf) {
            /* Poll the interrupt endpoint via event ring */
            struct xhci_controller *hc = hid->usb_dev->hc;
            struct xhci_trb *evt = NULL;

            /* Check for pending transfer events */
            /* We iterate through unprocessed events in the event ring */
            uint32_t start = hc->event_ring_dequeue;
            uint32_t idx = start;
            uint8_t ccs = hc->event_ring_ccs;
            int done = 0;

            while (!done) {
                struct xhci_trb *e = &hc->event_ring[idx];
                if ((e->control & XHCI_TRB_C) != ccs) {
                    break;
                }
                uint32_t evt_type = (e->control & XHCI_TRB_TYPE_MASK) >> XHCI_TRB_TYPE_SHIFT;
                if (evt_type == XHCI_EVT_TRANSFER) {
                    uint32_t cc = (e->status & XHCI_TRB_CC_MASK) >> XHCI_TRB_CC_SHIFT;
                    uint32_t trb_len = e->status & 0x1FFFF;
                    if (cc == XHCI_CC_SUCCESS || cc == XHCI_CC_SHORT_PACKET) {
                        /* Process the report */
                        int report_len = hid->max_packet_size - (int)trb_len;
                        if (report_len > 0 && report_len <= (int)hid->max_packet_size) {
                            if (hid->protocol == USB_PROTOCOL_HID_KEYBOARD) {
                                hid_keyboard_report(hid, hid->report_buf, report_len);
                            } else if (hid->protocol == USB_PROTOCOL_HID_MOUSE) {
                                hid_mouse_report(hid, hid->report_buf, report_len);
                            }
                        }
                    }
                }
                idx = (idx + 1) % XHCI_EVENT_RING_SIZE;
                if (idx == 0) ccs ^= 1;
                if (idx == start) break;
            }

            /* FIXED (v4.2.7): BUG-HID-POLL-ADVANCE - Advance the
             * event ring dequeue pointer and update the cycle bit
             * after processing events.  Without this, the same events
             * are processed repeatedly on every poll. */
            hc->event_ring_dequeue = idx;
            hc->event_ring_ccs = ccs;
        }
        hid = hid->next;
    }
}

/* ================================================================
 * Initialize HID Subsystem
 * ================================================================ */
void hid_init(void) {
    log_printf(LOG_LEVEL_INFO, "HID: initializing USB HID subsystem...\n");

    /* Probe all USB devices for HID class */
    struct usb_device *dev = usb_device_list();
    int probed = 0;
    while (dev) {
        int is_hid = 0;
        for (int i = 0; i < dev->num_interfaces; i++) {
            if (dev->interfaces[i].interface_class == USB_CLASS_HID) {
                is_hid = 1;
                break;
            }
        }
        if (is_hid || dev->dev_desc.bDeviceClass == USB_CLASS_HID) {
            hid_probe(dev);
            probed++;
        }
        dev = dev->next;
    }

    if (probed == 0) {
        log_printf(LOG_LEVEL_INFO, "HID: no HID devices found\n");
    } else {
        log_printf(LOG_LEVEL_INFO, "HID: probed %d device(s)\n", probed);
    }
}