/*
 * hid.h - USB Human Interface Device (HID) driver definitions
 *
 * Parses HID descriptors, sets up interrupt endpoints, and processes
 * keyboard and mouse boot protocol reports.  Keyboard events are
 * routed to the console input system.
 *
 * FIXED (v4.4.0): USB (v4.2.6)
 */
#ifndef HID_H
#define HID_H

#include <stdint.h>
#include "usb.h"

/* ================================================================
 * HID Boot Protocol Report Sizes
 * ================================================================ */
#define HID_KEYBOARD_REPORT_SIZE    8
#define HID_MOUSE_REPORT_SIZE       3

/* ================================================================
 * HID Boot Protocol Keyboard Report Structure
 * ================================================================ */
struct hid_keyboard_report {
    uint8_t  modifiers;         /* Byte 0: Modifier keys */
    uint8_t  reserved;          /* Byte 1: Reserved (OEM) */
    uint8_t  keys[6];           /* Bytes 2-7: Key codes */
} __attribute__((packed));

/* Keyboard modifier bits */
#define HID_MOD_LCTRL            0x01
#define HID_MOD_LSHIFT           0x02
#define HID_MOD_LALT             0x04
#define HID_MOD_LGUI             0x08
#define HID_MOD_RCTRL            0x10
#define HID_MOD_RSHIFT           0x20
#define HID_MOD_RALT             0x40
#define HID_MOD_RGUI             0x80

/* ================================================================
 * HID Boot Protocol Mouse Report Structure
 * ================================================================ */
struct hid_mouse_report {
    uint8_t  buttons;           /* Byte 0: Button state */
    int8_t   x_movement;        /* Byte 1: X movement */
    int8_t   y_movement;        /* Byte 2: Y movement */
} __attribute__((packed));

/* Mouse button bits */
#define HID_MOUSE_BTN_LEFT       0x01
#define HID_MOUSE_BTN_RIGHT      0x02
#define HID_MOUSE_BTN_MIDDLE     0x04

/* ================================================================
 * HID Device Tracking
 * ================================================================ */
struct hid_device {
    struct usb_device *usb_dev;
    uint8_t            protocol;       /* Keyboard (1) or Mouse (2) */
    uint8_t            ep_interrupt;   /* Interrupt IN endpoint address */
    uint8_t            ep_id;          /* DCI for interrupt endpoint */
    uint16_t           max_packet_size;
    uint8_t            interval;
    uint8_t            *report_buf;    /* Buffer for interrupt reports */
    uint8_t            initialized;

    /* Keyboard state tracking */
    uint8_t            prev_keys[6];   /* Previous report keys (for press/release detection) */
    uint8_t            prev_modifiers;

    struct hid_device  *next;
};

/* ================================================================
 * HID USB HID Key Code → PS/2 Scancode Translation Table
 *
 * Maps USB HID boot protocol keyboard codes to PS/2 set-1 scancodes.
 * Entries are scancode or 0 for unmapped keys.
 * ================================================================ */
#define HID_KEY_MAX      231

/* ================================================================
 * HID Driver API
 * ================================================================ */
void hid_init(void);
void hid_probe(struct usb_device *dev);
void hid_keyboard_report(struct hid_device *hid, const uint8_t *report,
                         int length);
void hid_mouse_report(struct hid_device *hid, const uint8_t *report,
                      int length);
void hid_poll(void);

#endif /* HID_H */