/*
 * drm.h - Direct Rendering Manager / Kernel Mode Setting interface
 *
 * Provides framebuffer management, display mode setting, and basic
 * 2D rendering primitives for AuroraOS.
 *
 * Uses UEFI GOP framebuffer if available, otherwise falls back to
 * VGA text mode.
 */
#ifndef DRM_H
#define DRM_H

#include <stdint.h>
#include <stddef.h>

/* Maximum number of framebuffers, CRTCs, and connectors */
#define DRM_MAX_FBS        8
#define DRM_MAX_CRTCS      4
#define DRM_MAX_CONNECTORS 4
#define DRM_MAX_MODES     16

/* DRM ioctl command codes */
#define DRM_IOCTL_BASE              0x64
#define DRM_IOCTL_VERSION           (DRM_IOCTL_BASE + 0x00)
#define DRM_IOCTL_MODE_GETRESOURCES (DRM_IOCTL_BASE + 0x01)
#define DRM_IOCTL_MODE_GETCONNECTOR (DRM_IOCTL_BASE + 0x02)
#define DRM_IOCTL_MODE_SETCRTC      (DRM_IOCTL_BASE + 0x03)
#define DRM_IOCTL_MODE_PAGE_FLIP    (DRM_IOCTL_BASE + 0x04)
#define DRM_IOCTL_MODE_DIRTYFB      (DRM_IOCTL_BASE + 0x05)

/* Display mode flags */
#define DRM_MODE_FLAG_PHSYNC   (1 << 0)
#define DRM_MODE_FLAG_NHSYNC   (1 << 1)
#define DRM_MODE_FLAG_PVSYNC   (1 << 2)
#define DRM_MODE_FLAG_NVSYNC   (1 << 3)
#define DRM_MODE_FLAG_INTERLACE (1 << 4)

/* Connector types */
#define DRM_MODE_CONNECTOR_Unknown    0
#define DRM_MODE_CONNECTOR_VGA        1
#define DRM_MODE_CONNECTOR_DVII       2
#define DRM_MODE_CONNECTOR_DVID       3
#define DRM_MODE_CONNECTOR_HDMIA      4
#define DRM_MODE_CONNECTOR_HDMIB      5
#define DRM_MODE_CONNECTOR_eDP        6
#define DRM_MODE_CONNECTOR_DisplayPort 7
#define DRM_MODE_CONNECTOR_BuiltIn    8

/* Connector status */
#define DRM_MODE_CONNECTED        1
#define DRM_MODE_DISCONNECTED     2
#define DRM_MODE_UNKNOWNCONNECTION 3

/* ================================================================
 * Structures
 * ================================================================ */

/* Display mode (timing information) */
struct drm_mode {
    uint32_t clock;         /* pixel clock in kHz */
    uint16_t hdisplay;      /* horizontal visible area */
    uint16_t hsync_start;
    uint16_t hsync_end;
    uint16_t htotal;
    uint16_t vdisplay;      /* vertical visible area */
    uint16_t vsync_start;
    uint16_t vsync_end;
    uint16_t vtotal;
    uint32_t flags;         /* DRM_MODE_FLAG_* */
    char     name[32];      /* human-readable mode name */
};

/* Framebuffer */
struct drm_framebuffer {
    uint32_t fb_id;         /* unique framebuffer ID */
    uint32_t width;
    uint32_t height;
    uint32_t pitch;         /* bytes per line */
    uint32_t bpp;           /* bits per pixel */
    void    *buffer;        /* pixel data */
    size_t   size;          /* total buffer size in bytes */
    int      refcount;
};

/* CRTC (Cathode Ray Tube Controller) */
struct drm_crtc {
    uint32_t                crtc_id;
    struct drm_framebuffer *framebuffer;  /* current framebuffer */
    struct drm_mode        *mode;         /* current mode */
    int32_t                 x;            /* x offset */
    int32_t                 y;            /* y offset */
    int                     active;
};

/* Connector */
struct drm_connector {
    uint32_t         connector_id;
    uint32_t         connector_type;
    int              connected;
    struct drm_mode  modes[DRM_MAX_MODES];
    int              num_modes;
    struct drm_mode *preferred_mode;
    uint32_t         mm_width;
    uint32_t         mm_height;
};

/* DRM device */
struct drm_device {
    struct drm_framebuffer  framebuffers[DRM_MAX_FBS];
    int                     num_fbs;

    struct drm_crtc         crtcs[DRM_MAX_CRTCS];
    int                     num_crtcs;

    struct drm_connector    connectors[DRM_MAX_CONNECTORS];
    int                     num_connectors;

    /* Framebuffer info from UEFI GOP */
    void    *gop_fb;         /* physical framebuffer address */
    uint32_t gop_width;
    uint32_t gop_height;
    uint32_t gop_pitch;
    uint32_t gop_bpp;
    int      gop_available;

    /* Double buffering */
    void    *back_buffer;    /* back buffer for page flipping */
    int      using_double_buf;
};

/* ================================================================
 * Built-in font (8x16 bitmap)
 * ================================================================ */

/* DRM font glyph: 8 pixels wide, 16 pixels tall */
#define DRM_FONT_WIDTH   8
#define DRM_FONT_HEIGHT 16
#define DRM_FONT_GLYPHS 128

/* ================================================================
 * Public API
 * ================================================================ */

/* Initialize the DRM subsystem. Called during boot. */
void drm_init(void);

/* Initialize DRM with UEFI GOP framebuffer info. */
void drm_init_gop(void *fb_addr, uint32_t width, uint32_t height,
                  uint32_t pitch, uint32_t bpp);

/* Create a framebuffer. Returns fb_id or -1 on failure. */
int drm_fb_create(uint32_t width, uint32_t height, uint32_t bpp);

/* Destroy a framebuffer. */
int drm_fb_destroy(int fb_id);

/* Fill a rectangle on a framebuffer with a solid color. */
void drm_fb_fill_rect(int fb_id, int x, int y, int w, int h, uint32_t color);

/* Draw a character on a framebuffer using the built-in font. */
void drm_fb_draw_char(int fb_id, int x, int y, char c, uint32_t fg, uint32_t bg);

/* Draw a string on a framebuffer. */
void drm_fb_draw_text(int fb_id, int x, int y, const char *text,
                      uint32_t fg, uint32_t bg);

/* Present a framebuffer to the screen (double-buffer flip). */
void drm_fb_present(int fb_id);

/* Set the display mode on a CRTC. */
int drm_mode_set(int crtc_id, int fb_id, struct drm_mode *mode);

/* Detect connected displays. Returns number of connected connectors. */
int drm_connector_detect(void);

/* Get the DRM device for direct access. */
struct drm_device *drm_get_device(void);

/* Clear the entire screen. */
void drm_clear_screen(uint32_t color);

/* ================================================================
 * GUI (v4.2.6) — Compositor, Window System & Input Events
 * ================================================================ */

/* Window frame geometry constants */
#define DRM_WIN_BORDER         2
#define DRM_WIN_TITLE_HEIGHT  24

/* Input event types */
#define DRM_EV_KEY             1
#define DRM_EV_MOUSE_MOVE      2
#define DRM_EV_MOUSE_BUTTON    3
#define DRM_EV_MOUSE_SCROLL    4

/* Mouse button codes */
#define DRM_BTN_LEFT    1
#define DRM_BTN_RIGHT   2
#define DRM_BTN_MIDDLE  3

/* Key modifier flags */
#define DRM_MOD_SHIFT   0x01
#define DRM_MOD_CTRL    0x02
#define DRM_MOD_ALT     0x04
#define DRM_MOD_CAPS    0x08

/* ================================================================
 * GUI (v4.2.6) — Structures
 * ================================================================ */

/* Window structure */
struct drm_window {
    struct drm_window *next;        /* linked list (sorted by z_order ascending) */
    int x, y;                       /* top-left position of window frame */
    int width, height;              /* client area size */
    char title[64];                 /* window title */
    int z_order;                    /* Z-order for stacking */
    int visible;                    /* whether the window is visible */
    void *framebuffer;              /* backing framebuffer for client area (32bpp) */
    int dirty;                      /* damage tracking flag */
    struct drm_window *parent;      /* parent window (for dialogs) */
    struct drm_window *children;    /* child windows list head */
    struct drm_window *child_next;  /* sibling link in children list */
};

/* Compositor state */
struct drm_compositor {
    struct drm_window *windows;     /* linked list head, sorted by z_order ascending */
    struct drm_window *active_window; /* currently focused window */
    void *screen_fb;                /* screen back buffer (32bpp) */
    int screen_width;               /* screen width in pixels */
    int screen_height;              /* screen height in pixels */
    int screen_pitch;               /* screen pitch in bytes */
    int dirty_region_x;             /* dirty rectangle x */
    int dirty_region_y;             /* dirty rectangle y */
    int dirty_region_w;             /* dirty rectangle width */
    int dirty_region_h;             /* dirty rectangle height */
    int cursor_x;                   /* mouse cursor x position */
    int cursor_y;                   /* mouse cursor y position */
    int cursor_visible;             /* cursor visibility flag */
    int initialized;                /* compositor initialization flag */
    int next_z_order;               /* auto-incrementing z_order counter */
};

/* Input event */
struct drm_input_event {
    int type;                       /* DRM_EV_KEY, DRM_EV_MOUSE_MOVE, etc. */
    int key_code;                   /* key scancode or character */
    int key_modifiers;              /* DRM_MOD_* flags */
    int mouse_x;                    /* mouse x position */
    int mouse_y;                    /* mouse y position */
    int mouse_button;               /* DRM_BTN_LEFT, DRM_BTN_RIGHT, DRM_BTN_MIDDLE */
    int mouse_dx;                   /* mouse delta x (for scroll) */
    int mouse_dy;                   /* mouse delta y (for scroll) */
};

/* ================================================================
 * GUI (v4.2.6) — Compositor API
 * ================================================================ */

/* Initialize the compositor with screen dimensions. */
void drm_compositor_init(void);

/* Render all visible windows to the screen back buffer. */
void drm_compositor_render(void);

/* Present the rendered frame (copy back buffer to display). */
void drm_compositor_swap(void);

/* ================================================================
 * GUI (v4.2.6) — Window Management API
 * ================================================================ */

/* Create a new window with a backing framebuffer of client area size. */
struct drm_window *drm_window_create(int x, int y, int w, int h, const char *title);

/* Destroy a window and free its resources. */
void drm_window_destroy(struct drm_window *window);

/* Move a window to a new position. */
void drm_window_move(struct drm_window *window, int x, int y);

/* Resize a window (reallocates framebuffer). */
void drm_window_resize(struct drm_window *window, int w, int h);

/* Raise window to top of z-order. */
void drm_window_raise(struct drm_window *window);

/* Change window title. */
void drm_window_set_title(struct drm_window *window, const char *title);

/* Get the window's client framebuffer for drawing. */
void *drm_window_get_fb(struct drm_window *window);

/* Mark window as needing redraw. */
void drm_window_mark_dirty(struct drm_window *window);

/* ================================================================
 * GUI (v4.2.6) — Input Event System API
 * ================================================================ */

/* Initialize the input subsystem. */
void drm_input_init(void);

/* Handle keyboard events. */
void drm_input_handle_key(int key_code, int pressed);

/* Handle mouse movement. */
void drm_input_handle_mouse_move(int dx, int dy);

/* Handle mouse button clicks. */
void drm_input_handle_mouse_button(int button, int pressed);

/* Find the topmost window at the given screen coordinates. */
struct drm_window *drm_find_window_at(int x, int y);

/* Set keyboard focus to the specified window. */
void drm_input_focus_window(struct drm_window *window);

/* Route an input event to the focused window. */
void drm_input_dispatch_event(struct drm_input_event *event);

/* Cycle focus to the next visible window (Alt+Tab). */
void drm_input_cycle_focus(void);

/* ================================================================
 * GUI (v4.2.6) — Drawing Primitives API
 * ================================================================ */

/* Draw a filled rectangle on the screen (compositor back buffer). */
void drm_draw_rect(int x, int y, int w, int h, uint32_t color);

/* Draw a line using Bresenham's algorithm on the screen. */
void drm_draw_line(int x1, int y1, int x2, int y2, uint32_t color);

/* Draw text on the screen at the given position. */
void drm_draw_text(int x, int y, const char *text, uint32_t color);

/* Draw the window frame (title bar + borders) on the screen. */
void drm_draw_window_frame(struct drm_window *window);

/* Fill a rectangle on a raw 32bpp framebuffer. */
void drm_fill_rect(void *fb, int pitch, int x, int y, int w, int h, uint32_t color);

/* Get the compositor instance. */
struct drm_compositor *drm_get_compositor(void);

/* Get window client area width. */
static inline int drm_window_client_width(struct drm_window *w) {
    return w ? w->width : 0;
}

/* Get window client area height. */
static inline int drm_window_client_height(struct drm_window *w) {
    return w ? w->height : 0;
}

#endif /* DRM_H */