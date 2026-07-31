/*
 * compositor.h - Window Compositor Interface
 *
 * FIXED (v4.3.8): GUI-001 — Window compositor declarations.
 * Provides window creation, destruction, z-order management,
 * clipping, refresh, mouse cursor, and Window Manager API.
 */
#ifndef COMPOSITOR_H
#define COMPOSITOR_H

#include <stdint.h>

/* Forward declaration */
struct window;

/* ================================================================
 * Compositor API
 * ================================================================ */

/* Initialize the compositor with a framebuffer. */
void compositor_init(uint32_t *fb, int w, int h, int pitch);

/* Create a new window. Returns window pointer or NULL on failure. */
struct window *compositor_create_window(int x, int y, int w, int h, const char *title);

/* Destroy a window and free its resources. */
void compositor_destroy_window(struct window *win);

/* Move a window to a new position. */
void compositor_move_window(struct window *win, int x, int y);

/* Resize a window (reallocates back buffer). */
void compositor_resize_window(struct window *win, int w, int h);

/* Raise window to top of z-order. */
void compositor_raise_window(struct window *win);

/* Lower window to bottom of z-order. */
void compositor_lower_window(struct window *win);

/* Minimize a window (hide from rendering). */
void compositor_minimize_window(struct window *win);

/* Restore a minimized window. */
void compositor_restore_window(struct window *win);

/* Redraw all visible windows bottom-to-top. */
void compositor_render(void);

/* ================================================================
 * FIXED (v4.3.8): GUI-003 — Mouse Cursor API
 * ================================================================ */

/* Draw the mouse cursor on the framebuffer. */
void compositor_draw_cursor(void);

/* Set the cursor position. */
void compositor_set_cursor(int x, int y);

/* Show or hide the cursor. */
void compositor_show_cursor(int show);

/* ================================================================
 * FIXED (v4.3.8): GUI-005 — Window Manager API
 * ================================================================ */

/* Create a window via the window manager. */
struct window *wm_create(int x, int y, int w, int h, const char *title);

/* Destroy a window via the window manager. */
void wm_destroy(struct window *win);

/* Move a window via the window manager. */
void wm_move(struct window *win, int x, int y);

/* Resize a window via the window manager. */
void wm_resize(struct window *win, int w, int h);

/* Raise a window to the top via the window manager. */
void wm_raise(struct window *win);

/* Lower a window to the bottom via the window manager. */
void wm_lower(struct window *win);

/* Minimize a window via the window manager. */
void wm_minimize(struct window *win);

/* Restore a minimized window via the window manager. */
void wm_restore(struct window *win);

/* Focus a window. */
void wm_focus(struct window *win);

/* Find a window by its ID. */
struct window *wm_find_by_id(int id);

/* Process HID events and dispatch to focused window. */
void wm_process_events(void);

#endif /* COMPOSITOR_H */