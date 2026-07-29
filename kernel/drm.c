/*
 * drm.c - Direct Rendering Manager / Kernel Mode Setting
 *
 * Provides framebuffer management, display mode setting, and basic
 * 2D rendering primitives.
 *
 * Uses UEFI GOP framebuffer if available, otherwise falls back to
 * VGA text mode console.
 */
#include "drm.h"
#include "mem.h"
#include "include/log.h"
#include "console.h"
#include <string.h>

/* ================================================================
 * Built-in font: 8x16 bitmap font (ASCII 32-126 subset)
 *
 * Each glyph is 16 bytes (8 pixels wide, 16 pixels tall).
 * Bit 7 of each byte = leftmost pixel.
 * ================================================================ */
static const unsigned char font_data[][16] = {
    /* 32 ' ' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 33 '!' */ {0x00,0x00,0x18,0x3C,0x3C,0x3C,0x18,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00},
    /* 34 '"' */ {0x00,0x66,0x66,0x66,0x24,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 35 '#' */ {0x00,0x00,0x00,0x6C,0x6C,0xFE,0x6C,0x6C,0x6C,0xFE,0x6C,0x6C,0x00,0x00,0x00,0x00},
    /* 36 '$' */ {0x18,0x18,0x7C,0xC6,0xC2,0xC0,0x7C,0x06,0x86,0xC6,0x7C,0x18,0x18,0x00,0x00,0x00},
    /* 37 '%' */ {0x00,0x00,0x00,0x00,0xC2,0xC6,0x0C,0x18,0x30,0x60,0xC6,0x86,0x00,0x00,0x00,0x00},
    /* 38 '&' */ {0x00,0x00,0x38,0x6C,0x6C,0x38,0x76,0xDC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00},
    /* 39 ''' */ {0x00,0x30,0x30,0x30,0x60,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 40 '(' */ {0x00,0x00,0x0C,0x18,0x30,0x30,0x30,0x30,0x30,0x30,0x18,0x0C,0x00,0x00,0x00,0x00},
    /* 41 ')' */ {0x00,0x00,0x30,0x18,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x18,0x30,0x00,0x00,0x00,0x00},
    /* 42 '*' */ {0x00,0x00,0x00,0x00,0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 43 '+' */ {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 44 ',' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x18,0x30,0x00,0x00,0x00},
    /* 45 '-' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFE,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 46 '.' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00},
    /* 47 '/' */ {0x00,0x00,0x00,0x00,0x02,0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00,0x00,0x00,0x00},
    /* 48 '0' */ {0x00,0x00,0x38,0x6C,0xC6,0xC6,0xD6,0xD6,0xC6,0xC6,0x6C,0x38,0x00,0x00,0x00,0x00},
    /* 49 '1' */ {0x00,0x00,0x18,0x38,0x78,0x18,0x18,0x18,0x18,0x18,0x18,0x7E,0x00,0x00,0x00,0x00},
    /* 50 '2' */ {0x00,0x00,0x7C,0xC6,0x06,0x0C,0x18,0x30,0x60,0xC0,0xC6,0xFE,0x00,0x00,0x00,0x00},
    /* 51 '3' */ {0x00,0x00,0x7C,0xC6,0x06,0x06,0x3C,0x06,0x06,0x06,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* 52 '4' */ {0x00,0x00,0x0C,0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x0C,0x0C,0x1E,0x00,0x00,0x00,0x00},
    /* 53 '5' */ {0x00,0x00,0xFE,0xC0,0xC0,0xC0,0xFC,0x06,0x06,0x06,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* 54 '6' */ {0x00,0x00,0x38,0x60,0xC0,0xC0,0xFC,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* 55 '7' */ {0x00,0x00,0xFE,0xC6,0x06,0x06,0x0C,0x18,0x30,0x30,0x30,0x30,0x00,0x00,0x00,0x00},
    /* 56 '8' */ {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* 57 '9' */ {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7E,0x06,0x06,0x06,0x0C,0x78,0x00,0x00,0x00,0x00},
    /* 58 ':' */ {0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00},
    /* 59 ';' */ {0x00,0x00,0x00,0x00,0x18,0x18,0x00,0x00,0x00,0x18,0x18,0x30,0x00,0x00,0x00,0x00},
    /* 60 '<' */ {0x00,0x00,0x00,0x06,0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x06,0x00,0x00,0x00,0x00},
    /* 61 '=' */ {0x00,0x00,0x00,0x00,0x00,0xFE,0x00,0x00,0xFE,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 62 '>' */ {0x00,0x00,0x00,0x60,0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x60,0x00,0x00,0x00,0x00},
    /* 63 '?' */ {0x00,0x00,0x7C,0xC6,0xC6,0x0C,0x18,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00},
    /* 64 '@' */ {0x00,0x00,0x00,0x7C,0xC6,0xC6,0xDE,0xDE,0xDE,0xDC,0xC0,0x7C,0x00,0x00,0x00,0x00},
    /* 65 'A' */ {0x00,0x00,0x10,0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00},
    /* 66 'B' */ {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x66,0x66,0x66,0x66,0xFC,0x00,0x00,0x00,0x00},
    /* 67 'C' */ {0x00,0x00,0x3C,0x66,0xC2,0xC0,0xC0,0xC0,0xC0,0xC2,0x66,0x3C,0x00,0x00,0x00,0x00},
    /* 68 'D' */ {0x00,0x00,0xF8,0x6C,0x66,0x66,0x66,0x66,0x66,0x66,0x6C,0xF8,0x00,0x00,0x00,0x00},
    /* 69 'E' */ {0x00,0x00,0xFE,0x66,0x62,0x68,0x78,0x68,0x60,0x62,0x66,0xFE,0x00,0x00,0x00,0x00},
    /* 70 'F' */ {0x00,0x00,0xFE,0x66,0x62,0x68,0x78,0x68,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00},
    /* 71 'G' */ {0x00,0x00,0x3C,0x66,0xC2,0xC0,0xC0,0xDE,0xC6,0xC6,0x66,0x3A,0x00,0x00,0x00,0x00},
    /* 72 'H' */ {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00},
    /* 73 'I' */ {0x00,0x00,0x3C,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    /* 74 'J' */ {0x00,0x00,0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0xCC,0xCC,0xCC,0x78,0x00,0x00,0x00,0x00},
    /* 75 'K' */ {0x00,0x00,0xE6,0x66,0x6C,0x6C,0x78,0x78,0x6C,0x66,0x66,0xE6,0x00,0x00,0x00,0x00},
    /* 76 'L' */ {0x00,0x00,0xF0,0x60,0x60,0x60,0x60,0x60,0x60,0x62,0x66,0xFE,0x00,0x00,0x00,0x00},
    /* 77 'M' */ {0x00,0x00,0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00},
    /* 78 'N' */ {0x00,0x00,0xC6,0xE6,0xF6,0xFE,0xDE,0xCE,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00},
    /* 79 'O' */ {0x00,0x00,0x38,0x6C,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x00,0x00,0x00,0x00},
    /* 80 'P' */ {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x60,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00},
    /* 81 'Q' */ {0x00,0x00,0x38,0x6C,0xC6,0xC6,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x0C,0x0E,0x00,0x00},
    /* 82 'R' */ {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x6C,0x66,0x66,0x66,0xE6,0x00,0x00,0x00,0x00},
    /* 83 'S' */ {0x00,0x00,0x7C,0xC6,0xC6,0x60,0x38,0x0C,0x06,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* 84 'T' */ {0x00,0x00,0xFF,0xDB,0x99,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    /* 85 'U' */ {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* 86 'V' */ {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x10,0x00,0x00,0x00,0x00},
    /* 87 'W' */ {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xD6,0xD6,0xD6,0xFE,0xEE,0x6C,0x00,0x00,0x00,0x00},
    /* 88 'X' */ {0x00,0x00,0xC6,0xC6,0x6C,0x38,0x38,0x38,0x6C,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00},
    /* 89 'Y' */ {0x00,0x00,0x66,0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    /* 90 'Z' */ {0x00,0x00,0xFE,0xC6,0x86,0x0C,0x18,0x30,0x60,0xC2,0xC6,0xFE,0x00,0x00,0x00,0x00},
    /* 91 '[' */ {0x00,0x00,0x3C,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x30,0x3C,0x00,0x00,0x00,0x00},
    /* 92 '\' */ {0x00,0x00,0x00,0x80,0xC0,0xE0,0x70,0x38,0x1C,0x0E,0x06,0x02,0x00,0x00,0x00,0x00},
    /* 93 ']' */ {0x00,0x00,0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00,0x00,0x00,0x00},
    /* 94 '^' */ {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 95 '_' */ {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00,0x00},
    /* 96 '`' */ {0x00,0x30,0x30,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    /* 97 'a' */ {0x00,0x00,0x00,0x00,0x00,0x78,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00},
    /* 98 'b' */ {0x00,0x00,0xE0,0x60,0x60,0x78,0x6C,0x66,0x66,0x66,0x66,0x7C,0x00,0x00,0x00,0x00},
    /* 99 'c' */ {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xC0,0xC0,0xC0,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* 100 'd' */ {0x00,0x00,0x1C,0x0C,0x0C,0x3C,0x6C,0xCC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00},
    /* 101 'e' */ {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xFE,0xC0,0xC0,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* 102 'f' */ {0x00,0x00,0x1C,0x36,0x32,0x30,0x78,0x30,0x30,0x30,0x30,0x78,0x00,0x00,0x00,0x00},
    /* 103 'g' */ {0x00,0x00,0x00,0x00,0x00,0x76,0xCC,0xCC,0xCC,0xCC,0xCC,0x7C,0x0C,0xCC,0x78,0x00},
    /* 104 'h' */ {0x00,0x00,0xE0,0x60,0x60,0x6C,0x76,0x66,0x66,0x66,0x66,0xE6,0x00,0x00,0x00,0x00},
    /* 105 'i' */ {0x00,0x00,0x18,0x18,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    /* 106 'j' */ {0x00,0x00,0x06,0x06,0x00,0x0E,0x06,0x06,0x06,0x06,0x06,0x06,0x66,0x66,0x3C,0x00},
    /* 107 'k' */ {0x00,0x00,0xE0,0x60,0x60,0x66,0x6C,0x78,0x78,0x6C,0x66,0xE6,0x00,0x00,0x00,0x00},
    /* 108 'l' */ {0x00,0x00,0x38,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    /* 109 'm' */ {0x00,0x00,0x00,0x00,0x00,0xEC,0xFE,0xD6,0xD6,0xD6,0xD6,0xC6,0x00,0x00,0x00,0x00},
    /* 110 'n' */ {0x00,0x00,0x00,0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x66,0x66,0x00,0x00,0x00,0x00},
    /* 111 'o' */ {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* 112 'p' */ {0x00,0x00,0x00,0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00},
    /* 113 'q' */ {0x00,0x00,0x00,0x00,0x00,0x76,0xCC,0xCC,0xCC,0xCC,0xCC,0x7C,0x0C,0x0C,0x1E,0x00},
    /* 114 'r' */ {0x00,0x00,0x00,0x00,0x00,0xDC,0x76,0x66,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00},
    /* 115 's' */ {0x00,0x00,0x00,0x00,0x00,0x7C,0xC6,0x60,0x38,0x0C,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* 116 't' */ {0x00,0x00,0x10,0x30,0x30,0xFC,0x30,0x30,0x30,0x30,0x36,0x1C,0x00,0x00,0x00,0x00},
    /* 117 'u' */ {0x00,0x00,0x00,0x00,0x00,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0x76,0x00,0x00,0x00,0x00},
    /* 118 'v' */ {0x00,0x00,0x00,0x00,0x00,0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00,0x00,0x00,0x00},
    /* 119 'w' */ {0x00,0x00,0x00,0x00,0x00,0xC6,0xC6,0xD6,0xD6,0xD6,0xFE,0x6C,0x00,0x00,0x00,0x00},
    /* 120 'x' */ {0x00,0x00,0x00,0x00,0x00,0xC6,0x6C,0x38,0x38,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00},
    /* 121 'y' */ {0x00,0x00,0x00,0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7E,0x06,0x0C,0xF8,0x00},
    /* 122 'z' */ {0x00,0x00,0x00,0x00,0x00,0xFE,0xCC,0x18,0x30,0x60,0xC6,0xFE,0x00,0x00,0x00,0x00},
    /* 123 '{' */ {0x00,0x00,0x0E,0x18,0x18,0x18,0x70,0x18,0x18,0x18,0x18,0x0E,0x00,0x00,0x00,0x00},
    /* 124 '|' */ {0x00,0x00,0x18,0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x18,0x18,0x00,0x00,0x00,0x00},
    /* 125 '}' */ {0x00,0x00,0x70,0x18,0x18,0x18,0x0E,0x18,0x18,0x18,0x18,0x70,0x00,0x00,0x00,0x00},
    /* 126 '~' */ {0x00,0x00,0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
};

/* ================================================================
 * Global DRM device
 * ================================================================ */
static struct drm_device drm_dev;
static int drm_initialized = 0;

/* ================================================================
 * Internal helpers
 * ================================================================ */

/* Get a framebuffer by ID */
static struct drm_framebuffer *drm_get_fb(int fb_id) {
    for (int i = 0; i < drm_dev.num_fbs; i++) {
        if (drm_dev.framebuffers[i].fb_id == (uint32_t)fb_id &&
            drm_dev.framebuffers[i].buffer) {
            return &drm_dev.framebuffers[i];
        }
    }
    return NULL;
}

/* Set a pixel in a framebuffer */
static void drm_fb_set_pixel(struct drm_framebuffer *fb, int x, int y, uint32_t color) {
    if (!fb || !fb->buffer) return;
    /* FIXED (v4.3.0): NEW-20 DRM-PIXEL — bounds check prevents OOB
     * framebuffer access from malformed coordinates. */
    if (x < 0 || y < 0 || (uint32_t)x >= fb->width || (uint32_t)y >= fb->height) return;

    uint32_t offset = (uint32_t)y * fb->pitch + (uint32_t)x * (fb->bpp / 8);
    uint8_t *dst = (uint8_t *)fb->buffer + offset;

    if (fb->bpp == 32) {
        *(uint32_t *)dst = color;
    } else if (fb->bpp == 24) {
        dst[0] = (uint8_t)(color & 0xFF);
        dst[1] = (uint8_t)((color >> 8) & 0xFF);
        dst[2] = (uint8_t)((color >> 16) & 0xFF);
    } else if (fb->bpp == 16) {
        uint16_t c16 = (uint16_t)(((color >> 19) & 0x1F) << 11) |
                       (uint16_t)(((color >> 10) & 0x3F) << 5) |
                       (uint16_t)((color >> 3) & 0x1F);
        *(uint16_t *)dst = c16;
    }
}

/* ================================================================
 * drm_init: Initialize the DRM subsystem
 * ================================================================ */
void drm_init(void) {
    if (drm_initialized) return;

    memset(&drm_dev, 0, sizeof(drm_dev));

    /* Create a default connector */
    drm_dev.connectors[0].connector_id = 1;
    drm_dev.connectors[0].connector_type = DRM_MODE_CONNECTOR_BuiltIn;
    drm_dev.connectors[0].connected = 0;
    drm_dev.connectors[0].num_modes = 0;
    drm_dev.connectors[0].preferred_mode = NULL;
    drm_dev.num_connectors = 1;

    /* Create a default CRTC */
    drm_dev.crtcs[0].crtc_id = 1;
    drm_dev.crtcs[0].framebuffer = NULL;
    drm_dev.crtcs[0].mode = NULL;
    drm_dev.crtcs[0].x = 0;
    drm_dev.crtcs[0].y = 0;
    drm_dev.crtcs[0].active = 0;
    drm_dev.num_crtcs = 1;

    drm_initialized = 1;
    log_printf(LOG_LEVEL_INFO, "drm: subsystem initialized\n");
}

/* ================================================================
 * drm_init_gop: Initialize DRM with UEFI GOP framebuffer
 * ================================================================ */
void drm_init_gop(void *fb_addr, uint32_t width, uint32_t height,
                  uint32_t pitch, uint32_t bpp) {
    if (!drm_initialized) drm_init();

    drm_dev.gop_fb = fb_addr;
    drm_dev.gop_width = width;
    drm_dev.gop_height = height;
    drm_dev.gop_pitch = pitch;
    drm_dev.gop_bpp = bpp;
    drm_dev.gop_available = 1;

    /* Create a framebuffer pointing to the GOP framebuffer */
    if (drm_dev.num_fbs < DRM_MAX_FBS) {
        struct drm_framebuffer *fb = &drm_dev.framebuffers[drm_dev.num_fbs];
        fb->fb_id = (uint32_t)(drm_dev.num_fbs + 1);
        fb->width = width;
        fb->height = height;
        fb->pitch = pitch;
        fb->bpp = bpp;
        fb->buffer = fb_addr;
        fb->size = (size_t)pitch * height;
        fb->refcount = 1;
        drm_dev.num_fbs++;
    }

    /* Allocate back buffer for double buffering */
    if (drm_dev.gop_available) {
        drm_dev.back_buffer = kmalloc((size_t)pitch * height);
        if (drm_dev.back_buffer) {
            memset(drm_dev.back_buffer, 0, (size_t)pitch * height);
            drm_dev.using_double_buf = 1;
        }
    }

    /* Set up the connector with the GOP display mode */
    drm_dev.connectors[0].connected = DRM_MODE_CONNECTED;
    drm_dev.connectors[0].num_modes = 1;
    drm_dev.connectors[0].modes[0].hdisplay = (uint16_t)width;
    drm_dev.connectors[0].modes[0].vdisplay = (uint16_t)height;
    drm_dev.connectors[0].modes[0].htotal = (uint16_t)(width + 40);
    drm_dev.connectors[0].modes[0].hsync_start = (uint16_t)width;
    drm_dev.connectors[0].modes[0].hsync_end = (uint16_t)(width + 8);
    drm_dev.connectors[0].modes[0].vtotal = (uint16_t)(height + 10);
    drm_dev.connectors[0].modes[0].vsync_start = (uint16_t)height;
    drm_dev.connectors[0].modes[0].vsync_end = (uint16_t)(height + 2);
    drm_dev.connectors[0].modes[0].clock = width * height * 60 / 1000;
    drm_dev.connectors[0].modes[0].flags = 0;
    /* Set mode name */
    {
        int i = 0;
        char *n = drm_dev.connectors[0].modes[0].name;
        n[i++] = '0' + (char)(width / 1000);
        width %= 1000;
        n[i++] = '0' + (char)(width / 100);
        width %= 100;
        n[i++] = '0' + (char)(width / 10);
        n[i++] = '0' + (char)(width % 10);
        n[i++] = 'x';
        n[i++] = '0' + (char)(height / 1000);
        height %= 1000;
        n[i++] = '0' + (char)(height / 100);
        height %= 100;
        n[i++] = '0' + (char)(height / 10);
        n[i++] = '0' + (char)(height % 10);
        /* Copy remaining chars */
        const char *suffix = "@60";
        for (int j = 0; suffix[j] && i < 31; j++) n[i++] = suffix[j];
        n[i] = '\0';
    }
    drm_dev.connectors[0].preferred_mode = &drm_dev.connectors[0].modes[0];

    /* Activate the CRTC with the GOP framebuffer */
    drm_dev.crtcs[0].framebuffer = drm_get_fb(1);
    drm_dev.crtcs[0].mode = &drm_dev.connectors[0].modes[0];
    drm_dev.crtcs[0].active = 1;

    log_printf(LOG_LEVEL_INFO, "drm: GOP framebuffer %ux%u %ubpp initialized\n",
               drm_dev.gop_width, drm_dev.gop_height, drm_dev.gop_bpp);
}

/* ================================================================
 * drm_fb_create: Create a new framebuffer
 * ================================================================ */
int drm_fb_create(uint32_t width, uint32_t height, uint32_t bpp) {
    if (!drm_initialized) return -1;
    if (drm_dev.num_fbs >= DRM_MAX_FBS) return -1;
    if (width == 0 || height == 0 || bpp == 0) return -1;

    uint32_t pitch = width * (bpp / 8);
    size_t size = (size_t)pitch * height;

    void *buf = kmalloc(size);
    if (!buf) return -1;
    memset(buf, 0, size);

    int idx = drm_dev.num_fbs;
    struct drm_framebuffer *fb = &drm_dev.framebuffers[idx];
    fb->fb_id = (uint32_t)(idx + 1);
    fb->width = width;
    fb->height = height;
    fb->pitch = pitch;
    fb->bpp = bpp;
    fb->buffer = buf;
    fb->size = size;
    fb->refcount = 1;
    drm_dev.num_fbs++;

    log_printf(LOG_LEVEL_INFO, "drm: fb%d created (%ux%u %ubpp)\n",
               fb->fb_id, width, height, bpp);

    return (int)fb->fb_id;
}

/* ================================================================
 * drm_fb_destroy: Destroy a framebuffer
 * ================================================================ */
int drm_fb_destroy(int fb_id) {
    struct drm_framebuffer *fb = drm_get_fb(fb_id);
    if (!fb) return -1;

    /* Don't free the GOP framebuffer */
    if (fb->buffer == drm_dev.gop_fb) {
        fb->buffer = NULL;
        fb->refcount = 0;
        return 0;
    }

    if (fb->buffer) {
        kfree(fb->buffer);
        fb->buffer = NULL;
    }
    fb->refcount = 0;

    log_printf(LOG_LEVEL_INFO, "drm: fb%d destroyed\n", fb_id);
    return 0;
}

/* ================================================================
 * drm_fb_fill_rect: Fill a rectangle with a solid color
 * ================================================================ */
void drm_fb_fill_rect(int fb_id, int x, int y, int w, int h, uint32_t color) {
    struct drm_framebuffer *fb = drm_get_fb(fb_id);
    if (!fb) return;

    /* Clamp to framebuffer bounds */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)fb->width) w = (int)fb->width - x;
    if (y + h > (int)fb->height) h = (int)fb->height - y;
    if (w <= 0 || h <= 0) return;

    if (fb->bpp == 32) {
        for (int row = 0; row < h; row++) {
            uint32_t *dst = (uint32_t *)((uint8_t *)fb->buffer +
                             (uint32_t)(y + row) * fb->pitch +
                             (uint32_t)x * 4);
            for (int col = 0; col < w; col++) {
                dst[col] = color;
            }
        }
    } else {
        for (int row = 0; row < h; row++) {
            for (int col = 0; col < w; col++) {
                drm_fb_set_pixel(fb, x + col, y + row, color);
            }
        }
    }
}

/* ================================================================
 * drm_fb_draw_char: Draw a single character using the built-in font
 * ================================================================ */
void drm_fb_draw_char(int fb_id, int x, int y, char c, uint32_t fg, uint32_t bg) {
    struct drm_framebuffer *fb = drm_get_fb(fb_id);
    if (!fb) return;

    /* Bounds check */
    if (x < 0 || y < 0 ||
        x + DRM_FONT_WIDTH > (int)fb->width ||
        y + DRM_FONT_HEIGHT > (int)fb->height) {
        return;
    }

    int glyph_index = (unsigned char)c;
    /* FIXED (v4.3.0): NEW-21 DRM-FONT — bounds check character against
     * font_data array size to prevent OOB access. */
    if (glyph_index < 32 || glyph_index > 126) {
        glyph_index = 32;  /* default to space */
    }
    glyph_index -= 32;  /* font_data starts at ASCII 32 */

    const unsigned char *glyph = font_data[glyph_index];

    for (int row = 0; row < DRM_FONT_HEIGHT; row++) {
        for (int col = 0; col < DRM_FONT_WIDTH; col++) {
            int bit = (glyph[row] >> (7 - col)) & 1;
            drm_fb_set_pixel(fb, x + col, y + row, bit ? fg : bg);
        }
    }
}

/* ================================================================
 * drm_fb_draw_text: Draw a string using the built-in font
 * ================================================================ */
void drm_fb_draw_text(int fb_id, int x, int y, const char *text,
                      uint32_t fg, uint32_t bg) {
    if (!text) return;
    int cx = x;
    for (int i = 0; text[i]; i++) {
        if (text[i] == '\n') {
            cx = x;
            y += DRM_FONT_HEIGHT;
            continue;
        }
        drm_fb_draw_char(fb_id, cx, y, text[i], fg, bg);
        cx += DRM_FONT_WIDTH;
    }
}

/* ================================================================
 * drm_fb_present: Present a framebuffer (double-buffer flip)
 * ================================================================ */
void drm_fb_present(int fb_id) {
    struct drm_framebuffer *fb = drm_get_fb(fb_id);
    if (!fb) return;

    /* If double buffering is enabled, copy to GOP framebuffer */
    if (drm_dev.using_double_buf && drm_dev.gop_available && drm_dev.back_buffer) {
        /* Copy fb to back buffer, then to GOP fb */
        size_t copy_size = fb->size;
        if (copy_size > (size_t)drm_dev.gop_pitch * drm_dev.gop_height) {
            copy_size = (size_t)drm_dev.gop_pitch * drm_dev.gop_height;
        }

        /* Copy fb to back buffer first */
        memcpy(drm_dev.back_buffer, fb->buffer, copy_size);

        /* Then copy back buffer to GOP framebuffer */
        memcpy(drm_dev.gop_fb, drm_dev.back_buffer, copy_size);
    } else if (drm_dev.gop_available && fb->buffer != drm_dev.gop_fb) {
        /* No double buffering: copy directly to GOP fb */
        size_t copy_size = fb->size;
        if (copy_size > (size_t)drm_dev.gop_pitch * drm_dev.gop_height) {
            copy_size = (size_t)drm_dev.gop_pitch * drm_dev.gop_height;
        }
        memcpy(drm_dev.gop_fb, fb->buffer, copy_size);
    }
}

/* ================================================================
 * drm_mode_set: Set the display mode on a CRTC
 * ================================================================ */
int drm_mode_set(int crtc_id, int fb_id, struct drm_mode *mode) {
    if (crtc_id < 1 || crtc_id > drm_dev.num_crtcs) return -1;

    struct drm_crtc *crtc = &drm_dev.crtcs[crtc_id - 1];
    struct drm_framebuffer *fb = drm_get_fb(fb_id);

    if (!fb) return -1;

    crtc->framebuffer = fb;
    if (mode) crtc->mode = mode;
    crtc->active = 1;

    return 0;
}

/* ================================================================
 * drm_connector_detect: Detect connected displays
 * ================================================================ */
int drm_connector_detect(void) {
    if (!drm_initialized) return 0;

    int connected = 0;
    for (int i = 0; i < drm_dev.num_connectors; i++) {
        if (drm_dev.connectors[i].connected == DRM_MODE_CONNECTED) {
            connected++;
        }
    }

    /* If GOP is available, ensure the connector is marked as connected */
    if (drm_dev.gop_available && connected == 0) {
        drm_dev.connectors[0].connected = DRM_MODE_CONNECTED;
        connected = 1;
    }

    return connected;
}

/* ================================================================
 * drm_get_device: Get the DRM device for direct access
 * ================================================================ */
struct drm_device *drm_get_device(void) {
    return &drm_dev;
}

/* ================================================================
 * drm_clear_screen: Clear the entire screen
 * ================================================================ */
void drm_clear_screen(uint32_t color) {
    if (!drm_initialized) return;

    struct drm_framebuffer *fb = drm_get_fb(1);
    if (fb) {
        drm_fb_fill_rect(1, 0, 0, (int)fb->width, (int)fb->height, color);
        drm_fb_present(1);
    }
}

/* ================================================================
 * GUI (v4.2.6) — Color palette (Catppuccin Mocha inspired)
 * ================================================================ */
#define GUI_COLOR_BG              0x00241E1E  /* base (dark background) */
#define GUI_COLOR_SURFACE         0x003E2E2E  /* mantle (window bg) */
#define GUI_COLOR_TITLE_ACTIVE    0x00FAB489  /* blue (active title bar) */
#define GUI_COLOR_TITLE_INACTIVE  0x005A5745  /* surface1 (inactive title) */
#define GUI_COLOR_BORDER          0x00705B58  /* surface2 (border) */
#define GUI_COLOR_TEXT_ACTIVE     0x00241E1E  /* base (text on active) */
#define GUI_COLOR_TEXT_INACTIVE   0x00F4D6CD  /* text (text on inactive) */
#define GUI_COLOR_TEXT_DEFAULT    0x00F4D6CD  /* text (default text) */
#define GUI_COLOR_CURSOR          0x00FFFFFF  /* white cursor */
#define GUI_COLOR_CURSOR_OUTLINE  0x00000000  /* black cursor outline */
#define GUI_COLOR_ACCENT          0x00F5E0DC  /* rosewater (accent) */

/* ================================================================
 * GUI (v4.2.6) — Cursor bitmap (12x19 arrow)
 * ================================================================ */
static const unsigned char cursor_bitmap[19][2] = {
    {0x80, 0x00}, {0xC0, 0x00}, {0xE0, 0x00}, {0xF0, 0x00},
    {0xF8, 0x00}, {0xFC, 0x00}, {0xFE, 0x00}, {0xFF, 0x00},
    {0xFF, 0x80}, {0xF8, 0x00}, {0xDC, 0x00}, {0xCE, 0x00},
    {0x87, 0x00}, {0x03, 0x80}, {0x01, 0xC0}, {0x00, 0xE0},
    {0x00, 0x70}, {0x00, 0x38}, {0x00, 0x1C}
};

/* ================================================================
 * GUI (v4.2.6) — Static compositor instance
 * ================================================================ */
static struct drm_compositor g_compositor;
static int g_compositor_initialized = 0;
static int g_input_initialized = 0;

/* ================================================================
 * GUI (v4.2.6) — Internal helpers
 * ================================================================ */

/* Set a pixel on a raw 32bpp framebuffer */
static void gui_set_pixel(void *fb, int pitch, int x, int y, uint32_t color) {
    if (!fb || x < 0 || y < 0) return;
    uint32_t *dst = (uint32_t *)((uint8_t *)fb + (uint32_t)y * (uint32_t)pitch + (uint32_t)x * 4);
    *dst = color;
}

/* Get the total window width including frame */
static int win_total_width(struct drm_window *w) {
    return w->width + 2 * DRM_WIN_BORDER;
}

/* Get the total window height including frame */
static int win_total_height(struct drm_window *w) {
    return w->height + DRM_WIN_TITLE_HEIGHT + DRM_WIN_BORDER;
}

/* Check if a point is inside a window's frame rect */
static int win_contains_point(struct drm_window *w, int px, int py) {
    int tw = win_total_width(w);
    int th = win_total_height(w);
    return (px >= w->x && px < w->x + tw && py >= w->y && py < w->y + th);
}

/* Check if a point is in the window's title bar */
static int win_title_bar_hit(struct drm_window *w, int px, int py) {
    int tw = win_total_width(w);
    return (px >= w->x && px < w->x + tw &&
            py >= w->y && py < w->y + DRM_WIN_TITLE_HEIGHT);
}

/* Check if a point is in the window's client area */
static int win_client_hit(struct drm_window *w, int px, int py) {
    return (px >= w->x + DRM_WIN_BORDER &&
            px < w->x + DRM_WIN_BORDER + w->width &&
            py >= w->y + DRM_WIN_TITLE_HEIGHT &&
            py < w->y + DRM_WIN_TITLE_HEIGHT + w->height);
}

/* Blit a 32bpp client framebuffer to the screen framebuffer */
static void gui_blit_fb(struct drm_window *w, void *screen_fb, int screen_pitch) {
    if (!w->framebuffer || !screen_fb) return;
    int src_pitch = w->width * 4;
    int dst_x = w->x + DRM_WIN_BORDER;
    int dst_y = w->y + DRM_WIN_TITLE_HEIGHT;

    for (int row = 0; row < w->height; row++) {
        uint32_t *src = (uint32_t *)((uint8_t *)w->framebuffer + (uint32_t)row * (uint32_t)src_pitch);
        uint32_t *dst = (uint32_t *)((uint8_t *)screen_fb +
                         (uint32_t)(dst_y + row) * (uint32_t)screen_pitch +
                         (uint32_t)dst_x * 4);
        for (int col = 0; col < w->width; col++) {
            dst[col] = src[col];
        }
    }
}

/* Draw a single character on the screen framebuffer */
static void gui_draw_char(void *fb, int pitch, int x, int y, char c, uint32_t fg) {
    if (!fb) return;
    int glyph_index = (unsigned char)c;
    if (glyph_index < 32 || glyph_index > 126) glyph_index = 32;
    glyph_index -= 32;

    const unsigned char *glyph = font_data[glyph_index];
    for (int row = 0; row < DRM_FONT_HEIGHT; row++) {
        for (int col = 0; col < DRM_FONT_WIDTH; col++) {
            if ((glyph[row] >> (7 - col)) & 1) {
                gui_set_pixel(fb, pitch, x + col, y + row, fg);
            }
        }
    }
}

/* Remove a window from the compositor's linked list */
static void compositor_remove_window(struct drm_compositor *comp, struct drm_window *w) {
    if (!comp->windows) return;
    if (comp->windows == w) {
        comp->windows = w->next;
        w->next = NULL;
        return;
    }
    struct drm_window *prev = comp->windows;
    while (prev->next && prev->next != w) prev = prev->next;
    if (prev->next == w) {
        prev->next = w->next;
        w->next = NULL;
    }
}

/* Insert a window into the compositor list sorted by z_order ascending */
static void compositor_insert_sorted(struct drm_compositor *comp, struct drm_window *w) {
    w->next = NULL;
    if (!comp->windows || comp->windows->z_order > w->z_order) {
        w->next = comp->windows;
        comp->windows = w;
        return;
    }
    struct drm_window *cur = comp->windows;
    while (cur->next && cur->next->z_order <= w->z_order) cur = cur->next;
    w->next = cur->next;
    cur->next = w;
}

/* FIXED (v4.3.4): GUI-001 — Compositor and window manager stub.
 * The DRM/KMS layer provides framebuffer, double buffering, Alt+Tab,
 * and mouse cursor rendering.  A full compositor/window manager requires:
 *   - Window tree management (create/destroy/resize/move windows)
 *   - Damage tracking and partial redraw
 *   - Input event routing (keyboard focus, mouse click targets)
 *   - Widget toolkit (buttons, text fields, scrollbars)
 *   - Font rendering (TTF/bitmap)
 * These are planned for a future release.  The ~400-line window API
 * in drm.c provides the foundation for this work. */

/* STUB (v4.3.4): GUI-001 — Compositor initialization placeholder.
 * Currently creates a default full-screen terminal window.
 * Future: window manager with multi-window support. */
static int compositor_init(void) {
    if (g_compositor_initialized) return 0;
    
    log_printf(LOG_LEVEL_INFO, "gui: compositor stub initialized\n");
    log_printf(LOG_LEVEL_INFO, "gui: framebuffer=%dx%d, double_buffer=%s\n",
               drm_dev.gop_width, drm_dev.gop_height,
               drm_dev.back_buffer ? "yes" : "no");
    log_printf(LOG_LEVEL_INFO, "gui: window manager API available (%d functions)\n", 12);
    /* FIXED (v4.3.4): GUI-001 — Future: create default window */
    
    g_compositor_initialized = 1;
    return 0;
}

/* ================================================================
 * GUI (v4.2.6) — Compositor
 * ================================================================ */

void drm_compositor_init(void) {
    if (g_compositor_initialized) return;

    struct drm_device *dev = drm_get_device();
    if (!dev || !dev->gop_available) {
        log_printf(LOG_LEVEL_WARN, "drm: compositor requires GOP framebuffer\n");
        return;
    }

    memset(&g_compositor, 0, sizeof(g_compositor));
    g_compositor.screen_width = (int)dev->gop_width;
    g_compositor.screen_height = (int)dev->gop_height;
    g_compositor.screen_pitch = (int)dev->gop_pitch;

    size_t fb_size = (size_t)dev->gop_pitch * dev->gop_height;
    g_compositor.screen_fb = kmalloc(fb_size);
    if (!g_compositor.screen_fb) {
        log_printf(LOG_LEVEL_ERROR, "drm: failed to allocate compositor back buffer\n");
        return;
    }
    memset(g_compositor.screen_fb, 0, fb_size);

    g_compositor.cursor_x = g_compositor.screen_width / 2;
    g_compositor.cursor_y = g_compositor.screen_height / 2;
    g_compositor.cursor_visible = 1;
    g_compositor.next_z_order = 1;
    g_compositor.initialized = 1;
    g_compositor_initialized = 1;

    log_printf(LOG_LEVEL_INFO, "drm: compositor initialized (%dx%d)\n",
               g_compositor.screen_width, g_compositor.screen_height);
}

void drm_compositor_render(void) {
    if (!g_compositor.initialized || !g_compositor.screen_fb) return;

    /* Clear screen to background color */
    drm_fill_rect(g_compositor.screen_fb, g_compositor.screen_pitch,
                  0, 0, g_compositor.screen_width, g_compositor.screen_height,
                  GUI_COLOR_BG);

    /* Render each window from bottom to top */
    struct drm_window *w = g_compositor.windows;
    while (w) {
        if (w->visible) {
            drm_draw_window_frame(w);
            gui_blit_fb(w, g_compositor.screen_fb, g_compositor.screen_pitch);
        }
        w = w->next;
    }

    /* Draw cursor on top */
    if (g_compositor.cursor_visible) {
        int cx = g_compositor.cursor_x;
        int cy = g_compositor.cursor_y;
        for (int row = 0; row < 19; row++) {
            for (int col = 0; col < 12; col++) {
                int byte_idx = col / 8;
                int bit_idx = 7 - (col % 8);
                if (cursor_bitmap[row][byte_idx] & (1 << bit_idx)) {
                    /* Draw outline first */
                    for (int dy = -1; dy <= 1; dy++) {
                        for (int dx = -1; dx <= 1; dx++) {
                            if (dx == 0 && dy == 0) continue;
                            gui_set_pixel(g_compositor.screen_fb, g_compositor.screen_pitch,
                                          cx + col + dx, cy + row + dy, GUI_COLOR_CURSOR_OUTLINE);
                        }
                    }
                    /* Draw cursor pixel */
                    gui_set_pixel(g_compositor.screen_fb, g_compositor.screen_pitch,
                                  cx + col, cy + row, GUI_COLOR_CURSOR);
                }
            }
        }
    }
}

void drm_compositor_swap(void) {
    if (!g_compositor.initialized || !g_compositor.screen_fb) return;

    struct drm_device *dev = drm_get_device();
    if (!dev || !dev->gop_available || !dev->gop_fb) return;

    size_t copy_size = (size_t)dev->gop_pitch * dev->gop_height;
    memcpy(dev->gop_fb, g_compositor.screen_fb, copy_size);
}

/* ================================================================
 * GUI (v4.2.6) — Window Management
 * ================================================================ */

struct drm_window *drm_window_create(int x, int y, int w, int h, const char *title) {
    if (!g_compositor.initialized) return NULL;
    if (w <= 0 || h <= 0) return NULL;

    struct drm_window *win = (struct drm_window *)kmalloc(sizeof(struct drm_window));
    if (!win) return NULL;
    memset(win, 0, sizeof(struct drm_window));

    win->x = x;
    win->y = y;
    win->width = w;
    win->height = h;
    win->visible = 1;
    win->dirty = 1;

    /* Set title */
    if (title) {
        int i;
        for (i = 0; i < 63 && title[i]; i++) win->title[i] = title[i];
        win->title[i] = '\0';
    }

    /* Allocate client framebuffer (32bpp) */
    size_t fb_size = (size_t)w * h * 4;
    win->framebuffer = kmalloc(fb_size);
    if (!win->framebuffer) {
        kfree(win);
        return NULL;
    }
    memset(win->framebuffer, 0, fb_size);

    /* Fill with default background */
    drm_fill_rect(win->framebuffer, w * 4, 0, 0, w, h, GUI_COLOR_SURFACE);

    /* Assign z_order and insert into compositor list */
    win->z_order = g_compositor.next_z_order++;
    compositor_insert_sorted(&g_compositor, win);

    /* If this is the first window, focus it */
    if (!g_compositor.active_window) {
        g_compositor.active_window = win;
    }

    log_printf(LOG_LEVEL_INFO, "drm: window \"%s\" created (%dx%d) at (%d,%d)\n",
               win->title, w, h, x, y);
    return win;
}

void drm_window_destroy(struct drm_window *window) {
    if (!window) return;

    /* Remove from compositor list */
    compositor_remove_window(&g_compositor, window);

    /* Update active window if needed */
    if (g_compositor.active_window == window) {
        g_compositor.active_window = g_compositor.windows;
    }

    /* Free framebuffer */
    if (window->framebuffer) {
        kfree(window->framebuffer);
        window->framebuffer = NULL;
    }

    log_printf(LOG_LEVEL_INFO, "drm: window \"%s\" destroyed\n", window->title);
    kfree(window);
}

void drm_window_move(struct drm_window *window, int x, int y) {
    if (!window) return;
    window->x = x;
    window->y = y;
    window->dirty = 1;
}

void drm_window_resize(struct drm_window *window, int w, int h) {
    if (!window || w <= 0 || h <= 0) return;

    void *new_fb = kmalloc((size_t)w * h * 4);
    if (!new_fb) return;
    memset(new_fb, 0, (size_t)w * h * 4);

    /* Copy old content (clamped) */
    int copy_w = (w < window->width) ? w : window->width;
    int copy_h = (h < window->height) ? h : window->height;
    int old_pitch = window->width * 4;
    int new_pitch = w * 4;
    for (int row = 0; row < copy_h; row++) {
        uint32_t *src = (uint32_t *)((uint8_t *)window->framebuffer + (uint32_t)row * (uint32_t)old_pitch);
        uint32_t *dst = (uint32_t *)((uint8_t *)new_fb + (uint32_t)row * (uint32_t)new_pitch);
        for (int col = 0; col < copy_w; col++) {
            dst[col] = src[col];
        }
    }

    /* Fill new area with background */
    if (w > window->width) {
        drm_fill_rect(new_fb, new_pitch, window->width, 0, w - window->width, h, GUI_COLOR_SURFACE);
    }
    if (h > window->height) {
        drm_fill_rect(new_fb, new_pitch, 0, window->height, w, h - window->height, GUI_COLOR_SURFACE);
    }

    kfree(window->framebuffer);
    window->framebuffer = new_fb;
    window->width = w;
    window->height = h;
    window->dirty = 1;

    log_printf(LOG_LEVEL_INFO, "drm: window \"%s\" resized to %dx%d\n", window->title, w, h);
}

void drm_window_raise(struct drm_window *window) {
    if (!window) return;

    /* Remove from current position and reinsert at top */
    compositor_remove_window(&g_compositor, window);
    window->z_order = g_compositor.next_z_order++;
    compositor_insert_sorted(&g_compositor, window);
    window->dirty = 1;
}

void drm_window_set_title(struct drm_window *window, const char *title) {
    if (!window || !title) return;
    int i;
    for (i = 0; i < 63 && title[i]; i++) window->title[i] = title[i];
    window->title[i] = '\0';
    window->dirty = 1;
}

void *drm_window_get_fb(struct drm_window *window) {
    return window ? window->framebuffer : NULL;
}

void drm_window_mark_dirty(struct drm_window *window) {
    if (window) window->dirty = 1;
}

/* ================================================================
 * GUI (v4.2.6) — Input Event System
 * ================================================================ */

void drm_input_init(void) {
    if (g_input_initialized) return;
    g_input_initialized = 1;
    log_printf(LOG_LEVEL_INFO, "drm: input subsystem initialized\n");
}

void drm_input_handle_key(int key_code, int pressed) {
    if (!g_compositor.initialized) return;
    if (!pressed) return;  /* Only handle key press, not release */

    /* Alt+Tab: cycle through windows */
    /* Tab scancode = 0x0F, Alt modifier */
    (void)key_code;
    /* Actual Alt+Tab handling is done in keyboard.c which calls drm_input_cycle_focus */
}

void drm_input_handle_mouse_move(int dx, int dy) {
    if (!g_compositor.initialized) return;
    g_compositor.cursor_x += dx;
    g_compositor.cursor_y += dy;
    if (g_compositor.cursor_x < 0) g_compositor.cursor_x = 0;
    if (g_compositor.cursor_y < 0) g_compositor.cursor_y = 0;
    if (g_compositor.cursor_x >= g_compositor.screen_width)
        g_compositor.cursor_x = g_compositor.screen_width - 1;
    if (g_compositor.cursor_y >= g_compositor.screen_height)
        g_compositor.cursor_y = g_compositor.screen_height - 1;
}

void drm_input_handle_mouse_button(int button, int pressed) {
    if (!g_compositor.initialized) return;
    if (!pressed) return;

    struct drm_window *w = drm_find_window_at(g_compositor.cursor_x, g_compositor.cursor_y);
    if (w) {
        drm_window_raise(w);
        drm_input_focus_window(w);
    }
}

struct drm_window *drm_find_window_at(int x, int y) {
    if (!g_compositor.initialized) return NULL;

    /* Search from top to bottom (highest z_order first) */
    struct drm_window *w = g_compositor.windows;
    struct drm_window *found = NULL;
    while (w) {
        if (w->visible && win_contains_point(w, x, y)) {
            found = w;  /* Keep going — last match is topmost */
        }
        w = w->next;
    }
    return found;
}

void drm_input_focus_window(struct drm_window *window) {
    if (!g_compositor.initialized) return;
    if (g_compositor.active_window == window) return;

    struct drm_window *old = g_compositor.active_window;
    g_compositor.active_window = window;

    if (old) old->dirty = 1;
    if (window) window->dirty = 1;

    log_printf(LOG_LEVEL_INFO, "drm: focus changed to \"%s\"\n",
               window ? window->title : "(none)");
}

void drm_input_dispatch_event(struct drm_input_event *event) {
    if (!event || !g_compositor.initialized) return;

    struct drm_window *target = g_compositor.active_window;
    if (!target) return;

    /* For now, just log the event */
    switch (event->type) {
        case DRM_EV_KEY:
            log_printf(LOG_LEVEL_DEBUG, "drm: key event code=%d mod=%d -> \"%s\"\n",
                       event->key_code, event->key_modifiers, target->title);
            break;
        case DRM_EV_MOUSE_MOVE:
            break;
        case DRM_EV_MOUSE_BUTTON:
            break;
        case DRM_EV_MOUSE_SCROLL:
            break;
    }
}

/* Cycle focus to the next visible window (for Alt+Tab) */
void drm_input_cycle_focus(void) {
    if (!g_compositor.initialized || !g_compositor.windows) return;

    struct drm_window *cur = g_compositor.active_window;
    struct drm_window *next = NULL;

    if (cur && cur->next) {
        next = cur->next;
    } else {
        /* Wrap around to the first window */
        next = g_compositor.windows;
    }

    if (next && next != cur) {
        drm_window_raise(next);
        drm_input_focus_window(next);
        /* Render and present immediately so the change is visible */
        drm_compositor_render();
        drm_compositor_swap();
    }
}

/* ================================================================
 * GUI (v4.2.6) — Drawing Primitives
 * ================================================================ */

void drm_draw_rect(int x, int y, int w, int h, uint32_t color) {
    if (!g_compositor.initialized || !g_compositor.screen_fb) return;
    drm_fill_rect(g_compositor.screen_fb, g_compositor.screen_pitch, x, y, w, h, color);
}

void drm_draw_line(int x1, int y1, int x2, int y2, uint32_t color) {
    if (!g_compositor.initialized || !g_compositor.screen_fb) return;

    int dx = x2 - x1;
    int dy = y2 - y1;
    int abs_dx = (dx < 0) ? -dx : dx;
    int abs_dy = (dy < 0) ? -dy : dy;
    int sx = (dx > 0) ? 1 : (dx < 0) ? -1 : 0;
    int sy = (dy > 0) ? 1 : (dy < 0) ? -1 : 0;

    if (abs_dx >= abs_dy) {
        int err = abs_dx / 2;
        int y = y1;
        for (int x = x1; ; x += sx) {
            gui_set_pixel(g_compositor.screen_fb, g_compositor.screen_pitch, x, y, color);
            if (x == x2) break;
            err -= abs_dy;
            if (err < 0) { y += sy; err += abs_dx; }
        }
    } else {
        int err = abs_dy / 2;
        int x = x1;
        for (int y = y1; ; y += sy) {
            gui_set_pixel(g_compositor.screen_fb, g_compositor.screen_pitch, x, y, color);
            if (y == y2) break;
            err -= abs_dx;
            if (err < 0) { x += sx; err += abs_dy; }
        }
    }
}

void drm_draw_text(int x, int y, const char *text, uint32_t color) {
    if (!g_compositor.initialized || !g_compositor.screen_fb || !text) return;
    int cx = x;
    for (int i = 0; text[i]; i++) {
        if (text[i] == '\n') {
            cx = x;
            y += DRM_FONT_HEIGHT;
            continue;
        }
        gui_draw_char(g_compositor.screen_fb, g_compositor.screen_pitch, cx, y, text[i], color);
        cx += DRM_FONT_WIDTH;
    }
}

void drm_draw_window_frame(struct drm_window *window) {
    if (!window || !g_compositor.initialized || !g_compositor.screen_fb) return;

    int is_active = (window == g_compositor.active_window);
    uint32_t title_color = is_active ? GUI_COLOR_TITLE_ACTIVE : GUI_COLOR_TITLE_INACTIVE;
    uint32_t text_color = is_active ? GUI_COLOR_TEXT_ACTIVE : GUI_COLOR_TEXT_INACTIVE;
    uint32_t border_color = GUI_COLOR_BORDER;

    int total_w = win_total_width(window);
    int total_h = win_total_height(window);

    /* Draw border (outer rectangle) */
    drm_fill_rect(g_compositor.screen_fb, g_compositor.screen_pitch,
                  window->x, window->y, total_w, total_h, border_color);

    /* Draw title bar */
    drm_fill_rect(g_compositor.screen_fb, g_compositor.screen_pitch,
                  window->x + DRM_WIN_BORDER,
                  window->y + DRM_WIN_BORDER,
                  total_w - 2 * DRM_WIN_BORDER,
                  DRM_WIN_TITLE_HEIGHT - DRM_WIN_BORDER,
                  title_color);

    /* Draw client area background */
    drm_fill_rect(g_compositor.screen_fb, g_compositor.screen_pitch,
                  window->x + DRM_WIN_BORDER,
                  window->y + DRM_WIN_TITLE_HEIGHT,
                  total_w - 2 * DRM_WIN_BORDER,
                  total_h - DRM_WIN_TITLE_HEIGHT - DRM_WIN_BORDER,
                  GUI_COLOR_BG);

    /* Draw title text (centered vertically in title bar) */
    int title_y = window->y + (DRM_WIN_TITLE_HEIGHT - DRM_FONT_HEIGHT) / 2;
    int title_x = window->x + DRM_WIN_BORDER + 8;
    drm_draw_text(title_x, title_y, window->title, text_color);
}

void drm_fill_rect(void *fb, int pitch, int x, int y, int w, int h, uint32_t color) {
    if (!fb || w <= 0 || h <= 0) return;

    for (int row = 0; row < h; row++) {
        uint32_t *dst = (uint32_t *)((uint8_t *)fb + (uint32_t)(y + row) * (uint32_t)pitch + (uint32_t)x * 4);
        for (int col = 0; col < w; col++) {
            dst[col] = color;
        }
    }
}

struct drm_compositor *drm_get_compositor(void) {
    return g_compositor.initialized ? &g_compositor : NULL;
}