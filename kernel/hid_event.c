/*
 * hid_event.c - HID Event Routing
 *
 * FIXED (v4.3.8): GUI-002 — HID event routing from USB/serial to GUI event queue.
 * Provides a ring buffer queue for mouse and keyboard events.
 * Events are pushed by HID drivers and popped by the compositor/window manager.
 */

#include "include/types.h"
#include "mem.h"
#include "include/log.h"
#include "include/hid_event.h"
#include <string.h>  /* FIXED (v4.4.2): BUILD-05 — for memcpy */

#define HID_EVENT_QUEUE_SIZE 256

/* FIXED (v4.4.2): BUILD-08 — struct hid_event defined in hid_event.h, duplicate removed */
static struct hid_event g_event_queue[HID_EVENT_QUEUE_SIZE];
static int g_event_head = 0;
static int g_event_tail = 0;
static int g_event_count = 0;

int hid_event_push(struct hid_event *ev) {
    if (g_event_count >= HID_EVENT_QUEUE_SIZE) return -1;
    memcpy(&g_event_queue[g_event_tail], ev, sizeof(*ev));
    g_event_tail = (g_event_tail + 1) % HID_EVENT_QUEUE_SIZE;
    g_event_count++;
    return 0;
}

int hid_event_pop(struct hid_event *ev) {
    if (g_event_count == 0) return -1;
    memcpy(ev, &g_event_queue[g_event_head], sizeof(*ev));
    g_event_head = (g_event_head + 1) % HID_EVENT_QUEUE_SIZE;
    g_event_count--;
    return 0;
}

int hid_event_available(void) { return g_event_count; }

/* Keyboard helpers */
void hid_key_down(uint32_t keycode) {
    struct hid_event ev = {0};
    ev.type = HID_EVENT_KEY_DOWN;
    ev.keycode = keycode;
    hid_event_push(&ev);
}
void hid_key_up(uint32_t keycode) {
    struct hid_event ev = {0};
    ev.type = HID_EVENT_KEY_UP;
    ev.keycode = keycode;
    hid_event_push(&ev);
}
void hid_mouse_move(int x, int y) {
    struct hid_event ev = {0};
    ev.type = HID_EVENT_MOUSE_MOVE;
    ev.mouse_x = x;
    ev.mouse_y = y;
    hid_event_push(&ev);
}
void hid_mouse_button(int button, int down) {
    struct hid_event ev = {0};
    ev.type = down ? HID_EVENT_MOUSE_BUTTON_DOWN : HID_EVENT_MOUSE_BUTTON_UP;
    ev.mouse_button = button;
    hid_event_push(&ev);
}
void hid_mouse_scroll(int dz) {
    struct hid_event ev = {0};
    ev.type = HID_EVENT_MOUSE_SCROLL;
    ev.mouse_dz = dz;
    hid_event_push(&ev);
}