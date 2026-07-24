/*
 * xhci.h - xHCI (eXtensible Host Controller Interface) definitions
 *
 * Register layouts, TRB structures, command types, and driver data
 * structures for USB 3.x host controllers.  Based on Intel xHCI
 * specification revision 1.2.
 *
 * /* USB (v4.2.6) */
 */
#ifndef XHCI_H
#define XHCI_H

#include <stdint.h>
#include "../pci.h"

/* ================================================================
 * xHCI PCI Class Code
 * ================================================================ */
#define XHCI_PCI_CLASS      0x0C
#define XHCI_PCI_SUBCLASS   0x03
#define XHCI_PCI_PROG_IF    0x30

/* ================================================================
 * xHCI MMIO Register Layout (offsets from Capability Registers base)
 * ================================================================ */

/* ---- Capability Registers (read from BAR0) ---- */
#define XHCI_REG_CAPLENGTH      0x00  /* Capability Register Length (8-bit) */
#define XHCI_REG_RSVD           0x01  /* Reserved */
#define XHCI_REG_HCIVERSION     0x02  /* Interface Version Number (16-bit) */
#define XHCI_REG_HCSPARAMS1     0x04  /* Structural Parameters 1 (32-bit) */
#define XHCI_REG_HCSPARAMS2     0x08  /* Structural Parameters 2 (32-bit) */
#define XHCI_REG_HCSPARAMS3     0x0C  /* Structural Parameters 3 (32-bit) */
#define XHCI_REG_HCCPARAMS1     0x10  /* Capability Parameters 1 (32-bit) */
#define XHCI_REG_DBOFF          0x14  /* Doorbell Offset (32-bit) */
#define XHCI_REG_RTSOFF         0x18  /* Runtime Register Space Offset (32-bit) */
#define XHCI_REG_HCCPARAMS2     0x1C  /* Capability Parameters 2 (32-bit) */

/* ---- Operational Registers (offset = CAPLENGTH) ---- */
#define XHCI_REG_USBCMD         0x00  /* USB Command (32-bit) */
#define XHCI_REG_USBSTS         0x04  /* USB Status (32-bit) */
#define XHCI_REG_PAGESIZE       0x08  /* Page Size (32-bit) */
#define XHCI_REG_RSVD2_START    0x0C  /* Reserved 0x0C-0x13 */
#define XHCI_REG_DNCTRL         0x14  /* Device Notification Control (32-bit) */
#define XHCI_REG_CRCR_LO        0x18  /* Command Ring Control Register (64-bit) */
#define XHCI_REG_CRCR_HI        0x1C
#define XHCI_REG_RSVD3_START    0x20  /* Reserved 0x20-0x2F */
#define XHCI_REG_DCBAAP_LO      0x30  /* Device Context Base Address Array Pointer (64-bit) */
#define XHCI_REG_DCBAAP_HI      0x34
#define XHCI_REG_CONFIG         0x38  /* Configure (32-bit) */

/* ---- Port Register Set (offset varies per port) ---- */
#define XHCI_REG_PORTSC_BASE    0x3F0 /* First Port Status and Control */

/* ---- Doorbell Array (offset = DBOFF) ---- */
/* Each doorbell is 32 bits, doorbell 0 is the command ring doorbell */

/* ---- Runtime Registers (offset = RTSOFF) ---- */
#define XHCI_REG_IMAN_BASE      0x20  /* Interrupter Management (32-bit) */
#define XHCI_REG_IMOD_BASE      0x24  /* Interrupter Moderation (32-bit) */
#define XHCI_REG_ERSTSZ_BASE    0x28  /* Event Ring Segment Table Size (32-bit) */
#define XHCI_REG_ERSTBA_LO_BASE 0x30  /* Event Ring Segment Table Base Address (64-bit) */
#define XHCI_REG_ERSTBA_HI_BASE 0x34
#define XHCI_REG_ERDP_LO_BASE   0x38  /* Event Ring Dequeue Pointer (64-bit) */
#define XHCI_REG_ERDP_HI_BASE   0x3C

/* Interrupter stride (from HCCPARAMS1) */
#define XHCI_IMAN(n)    (XHCI_REG_IMAN_BASE     + (n) * 32)
#define XHCI_IMOD(n)    (XHCI_REG_IMOD_BASE     + (n) * 32)
#define XHCI_ERSTSZ(n)  (XHCI_REG_ERSTSZ_BASE   + (n) * 32)
#define XHCI_ERSTBA_LO(n) (XHCI_REG_ERSTBA_LO_BASE + (n) * 32)
#define XHCI_ERSTBA_HI(n) (XHCI_REG_ERSTBA_HI_BASE + (n) * 32)
#define XHCI_ERDP_LO(n) (XHCI_REG_ERDP_LO_BASE  + (n) * 32)
#define XHCI_ERDP_HI(n) (XHCI_REG_ERDP_HI_BASE  + (n) * 32)

/* ---- Port Status and Control Register (PORTSC) ---- */
#define XHCI_PORTSC_CCS             0x00000001U  /* Current Connect Status */
#define XHCI_PORTSC_PED             0x00000002U  /* Port Enabled/Disabled */
#define XHCI_PORTSC_OCA             0x00000004U  /* Over-current Active */
#define XHCI_PORTSC_PR              0x00000010U  /* Port Reset */
#define XHCI_PORTSC_PLS_MASK        0x000000E0U  /* Port Link State */
#define XHCI_PORTSC_PLS_SHIFT       5
#define XHCI_PORTSC_PP              0x00000200U  /* Port Power */
#define XHCI_PORTSC_SPEED_MASK      0x00000F00U  /* Port Speed */
#define XHCI_PORTSC_SPEED_SHIFT     10
#define XHCI_PORTSC_PIC_MASK        0x0000C000U  /* Port Indicator Control */
#define XHCI_PORTSC_LWS             0x00010000U  /* Port Link State Write Strobe */
#define XHCI_PORTSC_CSC             0x00020000U  /* Connect Status Change */
#define XHCI_PORTSC_PEC             0x00040000U  /* Port Enabled/Disabled Change */
#define XHCI_PORTSC_WRC             0x00080000U  /* Warm Port Reset Change */
#define XHCI_PORTSC_OCC             0x00100000U  /* Over-current Change */
#define XHCI_PORTSC_PRC             0x00200000U  /* Port Reset Change */
#define XHCI_PORTSC_PLC             0x00400000U  /* Port Link State Change */
#define XHCI_PORTSC_CEC             0x00800000U  /* Port Config Error Change */
#define XHCI_PORTSC_CAS             0x01000000U  /* Cold Attach Status */
#define XHCI_PORTSC_WCE             0x02000000U  /* Wake on Connect Enable */
#define XHCI_PORTSC_WDE             0x04000000U  /* Wake on Disconnect Enable */
#define XHCI_PORTSC_WOE             0x08000000U  /* Wake on Over-current Enable */
#define XHCI_PORTSC_DR              0x40000000U  /* Device Removable */
#define XHCI_PORTSC_WPR             0x80000000U  /* Warm Port Reset */

/* Port Link States */
#define XHCI_PLS_U0                 0
#define XHCI_PLS_U1                 1
#define XHCI_PLS_U2                 2
#define XHCI_PLS_U3                 3
#define XHCI_PLS_DISABLED           4
#define XHCI_PLS_RX_DETECT          5
#define XHCI_PLS_INACTIVE           6
#define XHCI_PLS_POLLING            7
#define XHCI_PLS_RECOVERY           8
#define XHCI_PLS_HOT_RESET          9
#define XHCI_PLS_COMPLIANCE_MODE    10
#define XHCI_PLS_TEST_MODE          11
#define XHCI_PLS_RESUME             15

/* Port Speed */
#define XHCI_PORT_SPEED_FULL        1
#define XHCI_PORT_SPEED_LOW         2
#define XHCI_PORT_SPEED_HIGH        3
#define XHCI_PORT_SPEED_SUPER       4
#define XHCI_PORT_SPEED_SUPER_PLUS  5

/* ---- USBCMD Register Bits ---- */
#define XHCI_CMD_RS                 0x00000001U  /* Run/Stop */
#define XHCI_CMD_HCRST              0x00000002U  /* Host Controller Reset */
#define XHCI_CMD_INTE               0x00000004U  /* Interrupter Enable */
#define XHCI_CMD_HSEE               0x00000008U  /* Host System Error Enable */
#define XHCI_CMD_LHCRST             (0x00000020U)/* Light Host Controller Reset (xHCI 1.1+) */
#define XHCI_CMD_CSS                0x00000100U  /* Controller Save State */
#define XHCI_CMD_CRS                0x00000200U  /* Controller Restore State */
#define XHCI_CMD_EWE                0x00000400U  /* Enable Wrap Event */
#define XHCI_CMD_EU3S               0x00000800U  /* Enable U3 MFINDEX Stop */

/* ---- USBSTS Register Bits ---- */
#define XHCI_STS_HCH                0x00000001U  /* HCHalted */
#define XHCI_STS_HSE                0x00000004U  /* Host System Error */
#define XHCI_STS_EINT               0x00000008U  /* Event Interrupt */
#define XHCI_STS_PCD                0x00000010U  /* Port Change Detect */
#define XHCI_STS_SSS                0x00000100U  /* Save State Status */
#define XHCI_STS_RSS                0x00000200U  /* Restore State Status */
#define XHCI_STS_SRE                0x00000400U  /* Save/Restore Error */
#define XHCI_STS_CNR                0x00000800U  /* Controller Not Ready */
#define XHCI_STS_HCE                0x00001000U  /* Host Controller Error */

/* ---- CRCR Register Bits ---- */
#define XHCI_CRCR_RCS               0x00000001U  /* Ring Cycle State */
#define XHCI_CRCR_CS                0x00000002U  /* Command Stop */
#define XHCI_CRCR_CA                0x00000004U  /* Command Abort */
#define XHCI_CRCR_CRR               0x00000008U  /* Command Ring Running */

/* ---- CONFIG Register Bits ---- */
#define XHCI_CONFIG_MAX_SLOTS_MASK  0x000000FFU
#define XHCI_CONFIG_MAX_SLOTS_SHIFT 0

/* ---- HCSPARAMS1 Register Bits ---- */
#define XHCI_HCSPARAMS1_MAX_SLOTS_MASK    0x000000FFU
#define XHCI_HCSPARAMS1_MAX_INTR_MASK     0x00000700U
#define XHCI_HCSPARAMS1_MAX_INTR_SHIFT    8
#define XHCI_HCSPARAMS1_MAX_PORTS_MASK    0x00FF0000U
#define XHCI_HCSPARAMS1_MAX_PORTS_SHIFT   16

/* ---- HCCPARAMS1 Register Bits ---- */
#define XHCI_HCCPARAMS1_AC64              0x00000001U  /* 64-bit Addressing Capability */
#define XHCI_HCCPARAMS1_BNC               0x00000002U  /* BW Negotiation Capability */
#define XHCI_HCCPARAMS1_CSZ               0x00000004U  /* Context Size */
#define XHCI_HCCPARAMS1_PPC               0x00000008U  /* Port Power Control */
#define XHCI_HCCPARAMS1_PIND              0x00000010U  /* Port Indicators */
#define XHCI_HCCPARAMS1_LHRC              0x00000020U  /* Light HC Reset Capability */
#define XHCI_HCCPARAMS1_LTC               0x00000040U  /* Latency Tolerance Messaging Capability */
#define XHCI_HCCPARAMS1_NSS               0x00000080U  /* No Secondary SID Support */
#define XHCI_HCCPARAMS1_PAE               0x00000100U  /* Parse All Event Data */
#define XHCI_HCCPARAMS1_SPC               0x00000200U  /* Stopped - Short Packet Capability */
#define XHCI_HCCPARAMS1_SEC               0x00000400U  /* Stopped EDTLA Capability */
#define XHCI_HCCPARAMS1_CFC               0x00000800U  /* Contiguous Frame ID Capability */
#define XHCI_HCCPARAMS1_MAX_PSA_MASK      0x0000F000U  /* Max Primary Stream Array Size */
#define XHCI_HCCPARAMS1_XHCI_EXT_CAP_MASK 0xFFFF0000U  /* xHCI Extended Capabilities Pointer */

/* ---- IMAN Register Bits ---- */
#define XHCI_IMAN_IP                      0x00000001U  /* Interrupt Pending */
#define XHCI_IMAN_IE                      0x00000002U  /* Interrupt Enable */

/* ---- IMOD Register ---- */
#define XHCI_IMODI_MASK                   0x0000FFFFU  /* Interrupt Moderation Interval */
#define XHCI_IMODC_MASK                   0xFFFF0000U  /* Interrupt Moderation Counter */

/* ---- ERDP Register Bits ---- */
#define XHCI_ERDP_DESI_MASK               0x00000007U  /* Dequeue ERST Segment Index */
#define XHCI_ERDP_EHB                     0x00000008U  /* Event Handler Busy */

/* ================================================================
 * Transfer Request Block (TRB) - 16 bytes
 * ================================================================ */
struct xhci_trb {
    uint64_t parameter;       /* Parameter (64-bit address or data) */
    uint32_t status;          /* Status (completion code, TRB length, etc.) */
    uint32_t control;         /* Control (TRB type, flags, etc.) */
} __attribute__((packed));

/* TRB Type field (bits 10:15 of Control) */
#define XHCI_TRB_TYPE_SHIFT         10
#define XHCI_TRB_TYPE_MASK          0xFC00U

/* TRB Types */
#define XHCI_TRB_NORMAL             1
#define XHCI_TRB_SETUP_STAGE        2
#define XHCI_TRB_DATA_STAGE         3
#define XHCI_TRB_STATUS_STAGE       4
#define XHCI_TRB_ISOCH              5
#define XHCI_TRB_LINK               6
#define XHCI_TRB_EVENT_DATA         7
#define XHCI_TRB_NOOP               8
#define XHCI_TRB_ENABLE_SLOT        9
#define XHCI_TRB_DISABLE_SLOT      10
#define XHCI_TRB_ADDRESS_DEVICE    11
#define XHCI_TRB_CONFIGURE_EP      12
#define XHCI_TRB_EVALUATE_CONTEXT  13
#define XHCI_TRB_RESET_ENDPOINT    14
#define XHCI_TRB_STOP_ENDPOINT     15
#define XHCI_TRB_SET_TR_DEQUEUE    16
#define XHCI_TRB_RESET_DEVICE      17
#define XHCI_TRB_FORCE_EVENT       18
#define XHCI_TRB_NEGOTIATE_BW      19
#define XHCI_TRB_SET_LATENCY       20
#define XHCI_TRB_GET_PORT_BW       21
#define XHCI_TRB_FORCE_HEADER      22
#define XHCI_TRB_NOOP_CMD          23

/* Transfer TRB types */
#define XHCI_TRB_TRANSFER_NORMAL    1
#define XHCI_TRB_TRANSFER_SETUP     2
#define XHCI_TRB_TRANSFER_DATA      3
#define XHCI_TRB_TRANSFER_STATUS    4
#define XHCI_TRB_TRANSFER_ISOCH     5

/* TRB Control flags */
#define XHCI_TRB_C                           (1U << 0)   /* Cycle bit */
#define XHCI_TRB_TC                          (1U << 1)   /* Toggle Cycle */
#define XHCI_TRB_CHAIN                       (1U << 4)   /* Chain bit */
#define XHCI_TRB_ISP                         (1U << 5)   /* Interrupt on Short Packet */
#define XHCI_TRB_BSR                         (1U << 9)   /* Block Set Address Request */
#define XHCI_TRB_TRT_MASK                    0x0000C000U /* Transfer Type */
#define XHCI_TRB_TRT_SHIFT                   14
#define XHCI_TRB_DIR_IN                      (1U << 16)  /* Direction: IN (device to host) */

/* Transfer Type (TRT) values */
#define XHCI_TRT_NO_DATA                    0
#define XHCI_TRT_RESERVED                   1
#define XHCI_TRT_OUT_DATA                   2
#define XHCI_TRT_IN_DATA                    3

/* TRB Status field */
#define XHCI_TRB_LEN_MASK                   0x0001FFFFU  /* TRB Transfer Length */
#define XHCI_TRB_TD_SIZE_MASK               0x001F0000U  /* TD Size */
#define XHCI_TRB_TD_SIZE_SHIFT              17

/* ---- TRB Completion Codes ---- */
#define XHCI_CC_INVALID                     0
#define XHCI_CC_SUCCESS                     1
#define XHCI_CC_DATA_BUFFER_ERROR           2
#define XHCI_CC_BABBLE_DETECTED             3
#define XHCI_CC_USB_TRANSACTION_ERROR       4
#define XHCI_CC_TRB_ERROR                   5
#define XHCI_CC_STALL_ERROR                 6
#define XHCI_CC_RESOURCE_ERROR              7
#define XHCI_CC_BANDWIDTH_ERROR             8
#define XHCI_CC_NO_SLOTS_AVAILABLE          9
#define XHCI_CC_INVALID_STREAM_TYPE         10
#define XHCI_CC_SLOT_NOT_ENABLED            11
#define XHCI_CC_ENDPOINT_NOT_ENABLED        12
#define XHCI_CC_SHORT_PACKET                13
#define XHCI_CC_RING_UNDERRUN               14
#define XHCI_CC_RING_OVERRUN                15
#define XHCI_CC_VF_EVENT_RING_FULL          16
#define XHCI_CC_PARAMETER_ERROR             17
#define XHCI_CC_BANDWIDTH_OVERRUN           18
#define XHCI_CC_CONTEXT_STATE_ERROR         19
#define XHCI_CC_NO_PING_RESPONSE            20
#define XHCI_CC_EVENT_RING_FULL             21
#define XHCI_CC_INCOMPATIBLE_DEVICE         22
#define XHCI_CC_MISSED_SERVICE              23
#define XHCI_CC_COMMAND_RING_STOPPED        24
#define XHCI_CC_COMMAND_ABORTED             25
#define XHCI_CC_STOPPED                     26
#define XHCI_CC_STOPPED_LENGTH_INVALID      27
#define XHCI_CC_STOPPED_SHORT_PACKET        28
#define XHCI_CC_MAX_EXIT_LATENCY_ERROR      29
#define XHCI_CC_ISOCH_BUFFER_OVERRUN        31
#define XHCI_CC_EVENT_LOST                  32
#define XHCI_CC_UNDEFINED                   33
#define XHCI_CC_INVALID_STREAM_ID           34
#define XHCI_CC_SECONDARY_BANDWIDTH_ERROR   35
#define XHCI_CC_SPLIT_TRANSACTION_ERROR     36

/* ---- Event TRB Completion Code location (bits 24:31 of Status) ---- */
#define XHCI_TRB_CC_SHIFT                   24
#define XHCI_TRB_CC_MASK                    0xFF000000U

/* ---- Event TRB Type field (bits 10:15 of Control) ---- */
/* Event TRB Type values */
#define XHCI_EVT_TRANSFER                   32
#define XHCI_EVT_COMMAND_COMPLETE           33
#define XHCI_EVT_PORT_STATUS_CHANGE         34
#define XHCI_EVT_BANDWIDTH_REQUEST          35
#define XHCI_EVT_DOORBELL                   36
#define XHCI_EVT_HOST_CONTROLLER            37
#define XHCI_EVT_DEVICE_NOTIFICATION        38
#define XHCI_EVT_MFINDEX_WRAP               39

/* ================================================================
 * Event Ring Segment Table Entry (ERST) - 16 bytes
 * ================================================================ */
struct xhci_erst_entry {
    uint64_t ring_segment_base;   /* 64-bit physical address of Event Ring Segment */
    uint32_t ring_segment_size;   /* Number of TRBs in the segment */
    uint32_t reserved;            /* Reserved */
} __attribute__((packed));

/* ================================================================
 * Device Context structures
 * ================================================================ */

/* Slot Context (32 bytes) */
struct xhci_slot_context {
    uint32_t ctx_0;       /* DW0: Route String (bits 0:19), Speed (bits 20:23), MTT/ Hub (bits 24:25), Context Entries (bits 27:31) */
    uint32_t ctx_1;       /* DW1: Max Exit Latency (bits 0:15), Root Hub Port Num (bits 16:23), Num Ports (bits 24:31) */
    uint32_t ctx_2;       /* DW2: Parent Hub Slot ID (bits 0:7), Parent Port Number (bits 8:15), TTT (bits 16:17), Interrupter Target (bits 22:31) */
    uint32_t ctx_3;       /* DW3: Device Address (bits 0:7), RsvdZ (bits 8:26), Slot State (bits 27:31) */
    uint32_t ctx_4;       /* DW4: RsvdZ */
    uint32_t ctx_5;       /* DW5: RsvdZ */
    uint32_t ctx_6;       /* DW6: RsvdZ */
    uint32_t ctx_7;       /* DW7: RsvdZ */
} __attribute__((packed));

/* Slot Context field positions */
#define XHCI_SLOT_CTX_ENTRIES_SHIFT    27
#define XHCI_SLOT_CTX_SPEED_SHIFT      20
#define XHCI_SLOT_CTX_SPEED_MASK       0x00F00000U
#define XHCI_SLOT_CTX_RH_PORT_SHIFT    16
#define XHCI_SLOT_CTX_RH_PORT_MASK     0x00FF0000U
#define XHCI_SLOT_CTX_ADDR_SHIFT       0
#define XHCI_SLOT_CTX_ADDR_MASK        0x000000FFU
#define XHCI_SLOT_CTX_STATE_SHIFT      27
#define XHCI_SLOT_CTX_STATE_MASK       0xF8000000U

/* Slot States */
#define XHCI_SLOT_STATE_DISABLED       0
#define XHCI_SLOT_STATE_DEFAULT        1
#define XHCI_SLOT_STATE_ADDRESSED      2
#define XHCI_SLOT_STATE_CONFIGURED     3

/* Endpoint Context (32 bytes) */
struct xhci_ep_context {
    uint32_t ctx_0;       /* DW0: EP State (bits 0:2), RsvdZ (3), Mult (bits 8:9), MaxPStreams (bits 10:14), LSA (15), Interval (bits 16:23), Max ESIT Payload Hi (bits 24:31) */
    uint32_t ctx_1;       /* DW1: Max Packet Size (bits 0:15), Max Burst Size (bits 16:23), EP Type (bits 3:5 of upper byte), CErr (bits 1:2 of upper byte), HID (bit 7 of upper byte) */
    uint32_t ctx_2;       /* DW2: TR Dequeue Pointer Lo (bits 4:31), DCS (bit 0) */
    uint32_t ctx_3;       /* DW3: TR Dequeue Pointer Hi */
    uint32_t ctx_4;       /* DW4: Average TRB Length (bits 0:15), Max ESIT Payload Lo (bits 16:31) */
    uint32_t ctx_5;       /* DW5: RsvdZ */
    uint32_t ctx_6;       /* DW6: RsvdZ */
    uint32_t ctx_7;       /* DW7: RsvdZ */
} __attribute__((packed));

/* EP Context field positions */
#define XHCI_EP_CTX_STATE_SHIFT        0
#define XHCI_EP_CTX_STATE_MASK         0x00000007U
#define XHCI_EP_CTX_MAXP_SHIFT         0
#define XHCI_EP_CTX_MAXP_MASK          0x0000FFFFU
#define XHCI_EP_CTX_MAXB_SHIFT         16
#define XHCI_EP_CTX_MAXB_MASK          0x00FF0000U
#define XHCI_EP_CTX_TYPE_SHIFT         3
#define XHCI_EP_CTX_TYPE_MASK          0x00000038U  /* bits 3:5 in upper byte of DW1 */
#define XHCI_EP_CTX_INTERVAL_SHIFT     16
#define XHCI_EP_CTX_INTERVAL_MASK      0x00FF0000U
#define XHCI_EP_CTX_AVG_TRB_LEN_SHIFT  0
#define XHCI_EP_CTX_AVG_TRB_LEN_MASK   0x0000FFFFU
#define XHCI_EP_CTX_DCS                (1U << 0)   /* Dequeue Cycle State */

/* EP States */
#define XHCI_EP_STATE_DISABLED         0
#define XHCI_EP_STATE_RUNNING          1
#define XHCI_EP_STATE_HALTED           2
#define XHCI_EP_STATE_STOPPED          3
#define XHCI_EP_STATE_ERROR            4

/* EP Types */
#define XHCI_EP_TYPE_NOT_VALID         0
#define XHCI_EP_TYPE_ISOCH_OUT         1
#define XHCI_EP_TYPE_BULK_OUT          2
#define XHCI_EP_TYPE_INTERRUPT_OUT     3
#define XHCI_EP_TYPE_CONTROL           4
#define XHCI_EP_TYPE_ISOCH_IN          5
#define XHCI_EP_TYPE_BULK_IN           6
#define XHCI_EP_TYPE_INTERRUPT_IN      7

/* ================================================================
 * Input Context
 * ================================================================ */
struct xhci_input_context {
    uint32_t drop_flags;          /* Drop Context flags (bits 0:31) */
    uint32_t add_flags;           /* Add Context flags (bits 0:31) */
    uint32_t rsvd[6];             /* RsvdZ */
    struct xhci_slot_context slot; /* Slot context */
    /* Endpoint contexts follow: EP1 OUT, EP1 IN, EP2 OUT, EP2 IN, ... */
} __attribute__((packed));

/* Input Context add/drop flags */
#define XHCI_INPUT_ADD_SLOT        (1U << 0)
#define XHCI_INPUT_ADD_EP1         (1U << 1)
#define XHCI_INPUT_ADD_EP2         (1U << 2)
#define XHCI_INPUT_ADD_EP3         (1U << 3)
#define XHCI_INPUT_ADD_EP4         (1U << 4)
#define XHCI_INPUT_ADD_EP5         (1U << 5)
#define XHCI_INPUT_ADD_EP6         (1U << 6)
#define XHCI_INPUT_ADD_EP7         (1U << 7)
#define XHCI_INPUT_ADD_EP8         (1U << 8)
#define XHCI_INPUT_ADD_EP9         (1U << 9)
#define XHCI_INPUT_ADD_EP10        (1U << 10)
#define XHCI_INPUT_ADD_EP11        (1U << 11)
#define XHCI_INPUT_ADD_EP12        (1U << 12)
#define XHCI_INPUT_ADD_EP13        (1U << 13)
#define XHCI_INPUT_ADD_EP14        (1U << 14)
#define XHCI_INPUT_ADD_EP15        (1U << 15)

/* ================================================================
 * Output Device Context
 * ================================================================ */
struct xhci_device_context {
    struct xhci_slot_context slot;
    struct xhci_ep_context   ep[31];  /* 31 endpoint contexts */
} __attribute__((packed));

/* ================================================================
 * Doorbell
 * ================================================================ */

/* Doorbell values */
#define XHCI_DB_TARGET_MASK        0x000000FFU
#define XHCI_DB_STREAM_ID_SHIFT    16
#define XHCI_DB_STREAM_ID_MASK     0xFFFF0000U

/* Doorbell register: each slot gets a 32-bit doorbell.
 * Offset = DBOFF + (slot_id * 4), slot 0 = command ring */
static inline uint32_t xhci_doorbell_offset(uint32_t slot_id) {
    return slot_id * 4;
}

/* ================================================================
 * Transfer Ring
 * ================================================================ */
#define XHCI_TRB_RING_SIZE          256
#define XHCI_EVENT_RING_SIZE        256
#define XHCI_CMD_RING_SIZE          256
#define XHCI_MAX_SLOTS              64
#define XHCI_MAX_PORTS              32
#define XHCI_MAX_SCRATCHPAD_BUFFERS 32

struct xhci_transfer_ring {
    struct xhci_trb *trbs;        /* Virtual address of TRB array */
    uint64_t         phys_addr;   /* Physical address of TRB array */
    uint32_t         size;        /* Number of TRBs in the ring */
    uint32_t         enqueue;     /* Enqueue pointer (index) */
    uint32_t         dequeue;     /* Dequeue pointer (index) */
    uint8_t          pcs;         /* Producer Cycle State */
};

/* ================================================================
 * Device Slot
 * ================================================================ */
struct xhci_device_slot {
    uint8_t  slot_id;
    uint8_t  enabled;
    uint8_t  addressed;
    uint8_t  configured;
    uint8_t  speed;
    uint8_t  address;
    uint8_t  root_port;
    struct xhci_device_context *ctx;       /* Device context (output) */
    uint64_t ctx_phys;
    struct xhci_transfer_ring  ep_ring[32]; /* Transfer rings for each endpoint */
};

/* ================================================================
 * xHCI Controller
 * ================================================================ */
struct xhci_controller {
    struct pci_device       *pci_dev;
    volatile uint32_t       *mmio_base;          /* MMIO base (BAR0) */
    volatile uint32_t       *operational;        /* Operational registers base */
    volatile uint32_t       *doorbell;           /* Doorbell array base */
    volatile uint32_t       *runtime;            /* Runtime registers base */
    uint32_t                 cap_length;
    uint32_t                 db_offset;
    uint32_t                 rts_offset;
    uint32_t                 max_slots;
    uint32_t                 max_ports;
    uint32_t                 max_interrupters;
    uint8_t                  csz;                /* Context Size: 0=32, 1=64 bytes */
    uint8_t                  ac64;               /* 64-bit addressing */

    /* Device Context Base Address Array */
    uint64_t                *dcbaa;
    uint64_t                 dcbaa_phys;

    /* Command Ring */
    struct xhci_transfer_ring cmd_ring;

    /* Event Ring */
    struct xhci_erst_entry   erst_entry;
    struct xhci_trb          *event_ring;
    uint64_t                 event_ring_phys;
    uint32_t                 event_ring_dequeue;
    uint8_t                  event_ring_ccs;     /* Consumer Cycle State */

    /* Scratchpad buffers */
    uint64_t                 *scratchpad_buf_array;
    uint64_t                 scratchpad_buf_array_phys;
    void                     *scratchpad_bufs[XHCI_MAX_SCRATCHPAD_BUFFERS];
    uint32_t                 num_scratchpads;

    /* Device slots */
    struct xhci_device_slot  slots[XHCI_MAX_SLOTS + 1]; /* 1-based indexing */

    /* Ports */
    uint32_t                 port_offsets[XHCI_MAX_PORTS];
    uint32_t                 num_ports;

    /* Interrupt */
    int                      irq;
    int                      initialized;
    struct xhci_controller   *next;
};

/* ================================================================
 * Function Declarations
 * ================================================================ */
void xhci_init(void);
int  xhci_enable_slot(struct xhci_controller *hc);
int  xhci_address_device(struct xhci_controller *hc, uint8_t slot_id,
                         uint8_t root_port, uint8_t speed);
int  xhci_configure_endpoint(struct xhci_controller *hc, uint8_t slot_id,
                             int ep_id, int ep_type, uint16_t max_packet_size,
                             uint8_t interval);
void xhci_ring_doorbell(struct xhci_controller *hc, uint8_t slot_id,
                        uint8_t ep_id);
uint32_t xhci_get_port_status(struct xhci_controller *hc, uint32_t port_index);
int  xhci_reset_port(struct xhci_controller *hc, uint32_t port_index);
int  xhci_send_command(struct xhci_controller *hc, struct xhci_trb *cmd_trb);
int  xhci_get_descriptor(struct xhci_controller *hc, uint8_t slot_id,
                         uint8_t type, uint8_t index, uint8_t lang_id_lo,
                         uint8_t lang_id_hi, void *buf, uint16_t len);
int  xhci_set_configuration(struct xhci_controller *hc, uint8_t slot_id,
                            uint8_t config_value);
void xhci_interrupt_handler(void *stack);

/* xHCI controller lookup */
struct xhci_controller *xhci_get_controller(void);

#endif /* XHCI_H */