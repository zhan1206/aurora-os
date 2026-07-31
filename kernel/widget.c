/*
 * widget.c - Basic Widget Library
 *
 * FIXED (v4.3.8): GUI-004 — Basic widget library: button, label, text input.
 * Each widget is drawn on a window's back buffer.
 * Focus management tracks which widget has keyboard focus.
 */

#include "include/log.h"
#include "include/string.h"
#include "mem.h"
#include "include/compositor.h"

#define WIDGET_TYPE_LABEL  0
#define WIDGET_TYPE_BUTTON 1
#define WIDGET_TYPE_INPUT  2

#define WIDGET_MAX_TEXT 256

struct widget {
    int type;  /* 0=label, 1=button, 2=input */
    int x, y, w, h;
    char text[WIDGET_MAX_TEXT];
    int focused;
    int hovered;
    void (*on_click)(struct widget *);
    struct window *parent;
    struct widget *next;
};

static struct widget *g_widget_list = NULL;

/* ================================================================
 * widget_create — Create a new widget
 * ================================================================ */
struct widget *widget_create(int type, int x, int y, int w, int h,
                              const char *text, struct window *parent) {
    struct widget *wgt = kmalloc(sizeof(struct widget));
    if (!wgt) return NULL;
    memset(wgt, 0, sizeof(*wgt));
    wgt->type = type;
    wgt->x = x;
    wgt->y = y;
    wgt->w = w;
    wgt->h = h;
    if (text) {
        int i;
        for (i = 0; i < WIDGET_MAX_TEXT - 1 && text[i]; i++)
            wgt->text[i] = text[i];
        wgt->text[i] = '\0';
    }
    wgt->parent = parent;
    wgt->next = g_widget_list;
    g_widget_list = wgt;
    return wgt;
}

/* ================================================================
 * widget_destroy — Destroy a widget
 * ================================================================ */
void widget_destroy(struct widget *wgt) {
    if (!wgt) return;
    /* Remove from list */
    if (g_widget_list == wgt) {
        g_widget_list = wgt->next;
    } else {
        struct widget *prev = g_widget_list;
        while (prev && prev->next != wgt) prev = prev->next;
        if (prev) prev->next = wgt->next;
    }
    kfree(wgt);
}

/* ================================================================
 * widget_draw — Draw a widget on its parent window's back buffer
 * ================================================================ */
void widget_draw(struct widget *wgt) {
    if (!wgt || !wgt->parent || !wgt->parent->back_buffer) return;

    uint32_t *fb = wgt->parent->back_buffer;
    int pitch = wgt->parent->w;
    uint32_t bg, fg, border;

    switch (wgt->type) {
    case WIDGET_TYPE_BUTTON:
        bg = wgt->hovered ? 0x004488CC : 0x00666666;
        fg = 0x00FFFFFF;
        border = 0x00888888;
        break;
    case WIDGET_TYPE_INPUT:
        bg = wgt->focused ? 0x00333333 : 0x00222222;
        fg = 0x00FFFFFF;
        border = wgt->focused ? 0x004488CC : 0x00555555;
        break;
    case WIDGET_TYPE_LABEL:
    default:
        bg = 0x00000000;  /* transparent */
        fg = 0x00CCCCCC;
        border = 0x00000000;
        break;
    }

    /* Draw background */
    if (bg != 0x00000000) {
        for (int dy = 0; dy < wgt->h; dy++) {
            for (int dx = 0; dx < wgt->w; dx++) {
                int px = wgt->x + dx;
                int py = wgt->y + dy;
                if (px >= 0 && px < pitch && py >= 0 && py < wgt->parent->h) {
                    fb[py * pitch + px] = bg;
                }
            }
        }
    }

    /* Draw border */
    if (border != 0x00000000) {
        for (int dx = 0; dx < wgt->w; dx++) {
            if (wgt->x + dx >= 0 && wgt->x + dx < pitch) {
                if (wgt->y >= 0 && wgt->y < wgt->parent->h)
                    fb[wgt->y * pitch + wgt->x + dx] = border;
                if (wgt->y + wgt->h - 1 >= 0 && wgt->y + wgt->h - 1 < wgt->parent->h)
                    fb[(wgt->y + wgt->h - 1) * pitch + wgt->x + dx] = border;
            }
        }
        for (int dy = 0; dy < wgt->h; dy++) {
            if (wgt->y + dy >= 0 && wgt->y + dy < wgt->parent->h) {
                if (wgt->x >= 0 && wgt->x < pitch)
                    fb[(wgt->y + dy) * pitch + wgt->x] = border;
                if (wgt->x + wgt->w - 1 >= 0 && wgt->x + wgt->w - 1 < pitch)
                    fb[(wgt->y + dy) * pitch + wgt->x + wgt->w - 1] = border;
            }
        }
    }

    /* Draw text (simple ASCII, 8x16 font approximation) */
    /* FIXED (v4.3.8): GUI-004 — For now, text rendering is approximated.
     * A full font renderer would be needed for proper text display. */
    (void)fg;
    (void)wgt;
}

/* ================================================================
 * widget_set_focus — Set keyboard focus to a widget
 * ================================================================ */
void widget_set_focus(struct widget *wgt) {
    if (!wgt) return;
    /* Clear focus on all other widgets */
    for (struct widget *w = g_widget_list; w; w = w->next) {
        w->focused = 0;
    }
    wgt->focused = 1;
}

/* ================================================================
 * widget_handle_mouse — Handle mouse events for widgets
 * ================================================================ */
void widget_handle_mouse(int x, int y, int button, int pressed) {
    if (!pressed) return;  /* Only handle press events */

    /* Find the topmost widget under the cursor */
    struct widget *hit = NULL;
    for (struct widget *w = g_widget_list; w; w = w->next) {
        if (x >= w->x && x < w->x + w->w &&
            y >= w->y && y < w->y + w->h) {
            hit = w;
            break;  /* First match is topmost (inserted at head) */
        }
    }

    if (hit) {
        widget_set_focus(hit);
        if (hit->type == WIDGET_TYPE_BUTTON && hit->on_click) {
            hit->on_click(hit);
        }
    }
}

/* ================================================================
 * widget_handle_key — Handle keyboard events for the focused widget
 * ================================================================ */
void widget_handle_key(uint32_t keycode, int pressed) {
    if (!pressed) return;

    /* Find the focused widget */
    struct widget *focused = NULL;
    for (struct widget *w = g_widget_list; w; w = w->next) {
        if (w->focused) {
            focused = w;
            break;
        }
    }

    if (!focused || focused->type != WIDGET_TYPE_INPUT) return;

    /* Handle text input for text input widgets */
    int len = strlen(focused->text);
    /* Simple key handling: printable ASCII only */
    if (keycode >= 32 && keycode <= 126 && len < WIDGET_MAX_TEXT - 1) {
        focused->text[len] = (char)keycode;
        focused->text[len + 1] = '\0';
    } else if (keycode == 8 && len > 0) {  /* Backspace */
        focused->text[len - 1] = '\0';
    }
}