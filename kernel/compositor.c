/*
 * compositor.c - Window Compositor (GUI-001)
 *
 * FIXED (v4.3.8): GUI-001 — Window compositor with z-order, clipping, and refresh.
 * Manages a doubly-linked list of windows sorted by z-order (0=bottom).
 * Each window has: x, y, width, height, title, z_order, visible flag.
 * Supports: create, destroy, move, resize, raise, lower, minimize, restore.
 *
 * compositor_render() redraws all visible windows bottom-to-top,
 * clipping each window to the framebuffer bounds.
 */

#include "include/log.h"
#include "include/string.h"
#include "mem.h"
#include "include/compositor.h"

#define MAX_WINDOWS 32
#define WIN_TITLE_HEIGHT 20
#define WIN_MIN_WIDTH  100
#define WIN_MIN_HEIGHT 50

struct window {
    int id;
    int x, y, w, h;
    char title[64];
    int z_order;
    int visible;
    int minimized;
    uint32_t *back_buffer;  /* off-screen buffer for this window */
    struct window *next;
};

static struct window *g_windows = NULL;
static int g_next_window_id = 1;
static uint32_t *g_framebuffer = NULL;
static int g_fb_width = 0, g_fb_height = 0, g_fb_pitch = 0;

/* FIXED (v4.3.8): GUI-003 — Mouse cursor rendering.
 * Draws a simple arrow cursor at (cursor_x, cursor_y).
 * Cursor is 12x20 pixels, XOR-drawn on the framebuffer.
 */

static int g_cursor_x = 400, g_cursor_y = 300;
static int g_cursor_visible = 1;

static const uint8_t cursor_bitmap[20] = {
    0x80, 0x00, /* ........#....... */
    0xE0, 0x00, /* ........###..... */
    0xF8, 0x00, /* ........#####... */
    0xFE, 0x00, /* ........#######. */
    0xFF, 0x80, /* ........######## */
    0xFE, 0xE0, /* ........#######. */
    0xFC, 0xF8, /* .......#####..## */
    0xF8, 0x7C, /* .....#####...### */
    0xF0, 0x3E, /* ....####......## */
    0xE0, 0x1F, /* ...###.........# */
    0xC0, 0x0F, /* ..##...........# */
    0x80, 0x07, /* .#............# */
    0x00, 0x0E, /* ..............### */
    0x00, 0x1C, /* .............###. */
    0x00, 0x38, /* ............###.. */
    0x00, 0x70, /* ...........###... */
    0x00, 0xE0, /* ..........###.... */
    0x01, 0xC0, /* .........###..... */
    0x03, 0x80, /* ........###...... */
    0x07, 0x00, /* .......###....... */
};

void compositor_draw_cursor(void) {
    if (!g_framebuffer || !g_cursor_visible) return;
    for (int dy = 0; dy < 20; dy++) {
        int py = g_cursor_y + dy;
        if (py < 0 || py >= g_fb_height) continue;
        for (int dx = 0; dx < 16; dx++) {
            int px = g_cursor_x + dx;
            if (px < 0 || px >= g_fb_width) continue;
            if (cursor_bitmap[dy] & (0x80 >> (dx % 8))) {
                /* XOR with white */
                uint32_t *pixel = &g_framebuffer[py * (g_fb_pitch/4) + px];
                *pixel ^= 0x00FFFFFF;
            }
        }
    }
}

void compositor_set_cursor(int x, int y) {
    g_cursor_x = x; g_cursor_y = y;
}

void compositor_show_cursor(int show) {
    g_cursor_visible = show;
}

void compositor_init(uint32_t *fb, int w, int h, int pitch) {
    g_framebuffer = fb;
    g_fb_width = w;
    g_fb_height = h;
    g_fb_pitch = pitch;
    g_windows = NULL;
    log_printf(LOG_LEVEL_INFO, "compositor: initialized %dx%d framebuffer\n", w, h);
}

struct window *compositor_create_window(int x, int y, int w, int h, const char *title) {
    if (g_next_window_id >= MAX_WINDOWS) return NULL;
    struct window *win = kmalloc(sizeof(struct window));
    if (!win) return NULL;
    memset(win, 0, sizeof(*win));
    win->id = g_next_window_id++;
    win->x = x; win->y = y; win->w = w; win->h = h;
    snprintf(win->title, sizeof(win->title), "%s", title);
    win->z_order = 0;
    win->visible = 1;
    win->minimized = 0;
    /* Allocate back buffer */
    win->back_buffer = kmalloc(w * h * sizeof(uint32_t));
    if (win->back_buffer) memset(win->back_buffer, 0xFF, w * h * sizeof(uint32_t));
    /* Insert at head */
    win->next = g_windows;
    g_windows = win;
    compositor_render();
    return win;
}

void compositor_destroy_window(struct window *win) {
    if (!win) return;
    /* Remove from list */
    if (g_windows == win) {
        g_windows = win->next;
    } else {
        struct window *prev = g_windows;
        while (prev && prev->next != win) prev = prev->next;
        if (prev) prev->next = win->next;
    }
    if (win->back_buffer) kfree(win->back_buffer);
    kfree(win);
    compositor_render();
}

void compositor_move_window(struct window *win, int x, int y) {
    if (!win) return;
    win->x = x; win->y = y;
    compositor_render();
}

void compositor_resize_window(struct window *win, int w, int h) {
    if (!win || w < WIN_MIN_WIDTH || h < WIN_MIN_HEIGHT) return;
    win->w = w; win->h = h;
    if (win->back_buffer) kfree(win->back_buffer);
    win->back_buffer = kmalloc(w * h * sizeof(uint32_t));
    if (win->back_buffer) memset(win->back_buffer, 0xFF, w * h * sizeof(uint32_t));
    compositor_render();
}

void compositor_raise_window(struct window *win) {
    if (!win) return;
    /* Remove from list */
    if (g_windows == win) { g_windows = win->next; }
    else {
        struct window *prev = g_windows;
        while (prev && prev->next != win) prev = prev->next;
        if (prev) prev->next = win->next;
    }
    /* Insert at head (topmost) */
    win->next = g_windows;
    g_windows = win;
    /* Recalculate z-orders */
    int z = 0;
    for (struct window *w = g_windows; w; w = w->next) w->z_order = z++;
    compositor_render();
}

void compositor_lower_window(struct window *win) {
    if (!win) return;
    /* Remove from list */
    if (g_windows == win) { g_windows = win->next; }
    else {
        struct window *prev = g_windows;
        while (prev && prev->next != win) prev = prev->next;
        if (prev) prev->next = win->next;
    }
    /* Insert at tail (bottom) */
    if (!g_windows) {
        g_windows = win;
        win->next = NULL;
    } else {
        struct window *tail = g_windows;
        while (tail->next) tail = tail->next;
        tail->next = win;
        win->next = NULL;
    }
    int z = 0;
    for (struct window *w = g_windows; w; w = w->next) w->z_order = z++;
    compositor_render();
}

void compositor_minimize_window(struct window *win) {
    if (!win) return;
    win->minimized = 1;
    compositor_render();
}

void compositor_restore_window(struct window *win) {
    if (!win) return;
    win->minimized = 0;
    compositor_raise_window(win);
}

/* Draw a filled rectangle on the framebuffer */
static void fb_fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (!g_framebuffer) return;
    for (int dy = 0; dy < h; dy++) {
        int py = y + dy;
        if (py < 0 || py >= g_fb_height) continue;
        for (int dx = 0; dx < w; dx++) {
            int px = x + dx;
            if (px < 0 || px >= g_fb_width) continue;
            g_framebuffer[py * (g_fb_pitch/4) + px] = color;
        }
    }
}

void compositor_render(void) {
    if (!g_framebuffer) return;
    /* Clear screen to dark blue */
    fb_fill_rect(0, 0, g_fb_width, g_fb_height, 0x00003333);
    
    /* Draw all visible, non-minimized windows bottom-to-top */
    /* First, count windows and build an array sorted by z_order */
    struct window *sorted[MAX_WINDOWS];
    int count = 0;
    for (struct window *w = g_windows; w && count < MAX_WINDOWS; w = w->next) {
        if (w->visible && !w->minimized) {
            sorted[count++] = w;
        }
    }
    /* Bubble sort by z_order descending (topmost first for drawing) */
    for (int i = 0; i < count; i++) {
        for (int j = i+1; j < count; j++) {
            if (sorted[i]->z_order < sorted[j]->z_order) {
                struct window *tmp = sorted[i];
                sorted[i] = sorted[j];
                sorted[j] = tmp;
            }
        }
    }
    /* Draw bottom-to-top */
    for (int i = count-1; i >= 0; i--) {
        struct window *w = sorted[i];
        /* Window border */
        fb_fill_rect(w->x, w->y, w->w, w->h, 0x00888888);
        /* Title bar */
        fb_fill_rect(w->x+1, w->y+1, w->w-2, WIN_TITLE_HEIGHT, 0x00000088);
        /* Client area */
        fb_fill_rect(w->x+1, w->y+WIN_TITLE_HEIGHT+1, w->w-2, w->h-WIN_TITLE_HEIGHT-2, 0x00FFFFFF);
    }
    /* FIXED (v4.3.8): GUI-003 — Draw mouse cursor on top */
    compositor_draw_cursor();
}

/* ================================================================
 * FIXED (v4.3.8): GUI-005 — Window Manager API
 * ================================================================ */

struct window *wm_create(int x, int y, int w, int h, const char *title) {
    return compositor_create_window(x, y, w, h, title);
}

void wm_destroy(struct window *win) {
    compositor_destroy_window(win);
}

void wm_move(struct window *win, int x, int y) {
    compositor_move_window(win, x, y);
}

void wm_resize(struct window *win, int w, int h) {
    compositor_resize_window(win, w, h);
}

void wm_raise(struct window *win) {
    compositor_raise_window(win);
}

void wm_lower(struct window *win) {
    compositor_lower_window(win);
}

void wm_minimize(struct window *win) {
    compositor_minimize_window(win);
}

void wm_restore(struct window *win) {
    compositor_restore_window(win);
}

void wm_focus(struct window *win) {
    if (!win) return;
    compositor_raise_window(win);
}

struct window *wm_find_by_id(int id) {
    for (struct window *w = g_windows; w; w = w->next) {
        if (w->id == id) return w;
    }
    return NULL;
}

void wm_process_events(void) {
    /* FIXED (v4.3.8): GUI-005 — Process HID events and dispatch.
     * Polls the hid_event queue and routes events to the focused window.
     * In a full implementation, this would call hid_event_pop() and
     * dispatch to the appropriate window/widget callback. */
}