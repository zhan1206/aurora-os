/*
 * hid_event.h - HID Event Routing Interface
 *
 * FIXED (v4.3.8): GUI-002 — HID event routing declarations.
 * Ring buffer queue for mouse and keyboard events.
 */
#ifndef HID_EVENT_H
#define HID_EVENT_H

#include <stdint.h>

/* ================================================================
 * HID Event Types
 * ================================================================ */
typedef enum {
    HID_EVENT_NONE = 0,
    HID_EVENT_KEY_DOWN,
    HID_EVENT_KEY_UP,
    HID_EVENT_MOUSE_MOVE,
    HID_EVENT_MOUSE_BUTTON_DOWN,
    HID_EVENT_MOUSE_BUTTON_UP,
    HID_EVENT_MOUSE_SCROLL,
} hid_event_type_t;

/* ================================================================
 * HID Event Structure
 * ================================================================ */
struct hid_event {
    hid_event_type_t type;
    uint32_t keycode;    /* for keyboard */
    int mouse_x, mouse_y; /* for mouse */
    int mouse_button;     /* 0=left, 1=right, 2=middle */
    int mouse_dz;         /* scroll delta */
    uint64_t timestamp;
};

/* ================================================================
 * HID Event Queue API
 * ================================================================ */

/* Push an event into the queue. Returns 0 on success, -1 if full. */
int hid_event_push(struct hid_event *ev);

/* Pop an event from the queue. Returns 0 on success, -1 if empty. */
int hid_event_pop(struct hid_event *ev);

/* Return the number of events available in the queue. */
int hid_event_available(void);

/* ================================================================
 * Convenience helpers for HID drivers
 * ================================================================ */

/* Push a key-down event. */
void hid_key_down(uint32_t keycode);

/* Push a key-up event. */
void hid_key_up(uint32_t keycode);

/* Push a mouse-move event. */
void hid_mouse_move(int x, int y);

/* Push a mouse-button event. */
void hid_mouse_button(int button, int down);

/* Push a mouse-scroll event. */
void hid_mouse_scroll(int dz);

#endif /* HID_EVENT_H */