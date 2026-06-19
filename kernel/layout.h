/*
 * layout.h - Layout and component rendering utilities
 *
 * Provides reusable UI primitives for VGA text mode (80x25).
 * All functions use design tokens from theme.h.
 *
 * Components:
 *   - Centered text output
 *   - Horizontal separators / dividers
 *   - Box drawing (ASCII border)
 *   - Progress bar (with ETA)
 *   - Status labels
 *   - Simple table layout
 *   - Confirmation dialog
 *   - Notification banner
 *   - Lock screen / time display
 */

#ifndef LAYOUT_H
#define LAYOUT_H

#include "console.h"
#include "include/theme.h"
#include <stddef.h>
#include <stdint.h>

/* ================================================================
 * Text Layout
 * ================================================================ */

/* Write string centered horizontally.  Strips ANSI codes for width calc. */
static inline void console_write_centered(const char *s) {
    int slen = 0;
    for (const char *p = s; *p; ++p) {
        if (*p == '\x1b') { while (*p && *p != 'm') p++; continue; }
        slen++;
    }
    int pad = (COLS - slen) / 2;
    if (pad < 0) pad = 0;
    for (int i = 0; i < pad; ++i) console_putc(' ');
    console_write(s);
}

/* Write ANSI-colored string centered */
static inline void console_write_centered_ansi(const char *s) {
    int slen = 0;
    for (const char *p = s; *p; ++p) {
        if (*p == '\x1b') { while (*p && *p != 'm') p++; continue; }
        slen++;
    }
    int pad = (COLS - slen) / 2;
    if (pad < 0) pad = 0;
    for (int i = 0; i < pad; ++i) console_putc(' ');
    console_write_ansi(s);
}

/* Write left-padded text */
static inline void console_write_padded(const char *s, int left_pad) {
    for (int i = 0; i < left_pad; ++i) console_putc(' ');
    console_write(s);
}

/* ================================================================
 * Separators / Dividers
 * ================================================================ */

/* Draw a full-width horizontal separator line */
static inline void console_draw_hr(char ch) {
    console_write_ansi(CLR_DIVIDER);
    for (int i = 0; i < COLS; ++i) console_putc(ch);
    console_write_ansi(SGR_RESET);
    console_putc('\n');
}

/* Draw a horizontal line with centered text (e.g. "--- Title ---") */
static inline void console_draw_hr_text(const char *text, char ch) {
    int tlen = 0;
    for (const char *p = text; *p; ++p) tlen++;
    int left = (COLS - tlen - 2) / 2;
    if (left < 0) left = 0;
    console_write_ansi(CLR_DIVIDER);
    for (int i = 0; i < left; ++i) console_putc(ch);
    console_putc(' ');
    console_write(text);
    console_putc(' ');
    int right = COLS - left - tlen - 2;
    for (int i = 0; i < right; ++i) console_putc(ch);
    console_write_ansi(SGR_RESET);
    console_putc('\n');
}

/* ================================================================
 * Box Drawing (ASCII borders, 80-column width)
 * ================================================================ */

/* Draw a box with a title.
 * Usage:
 *   console_draw_box_top("My Title");
 *   console_write("content line 1\n");
 *   console_write("content line 2\n");
 *   console_draw_box_bottom();
 */
static inline void console_draw_box_top(const char *title) {
    console_write_ansi(CLR_BORDER);
    console_putc(BOX_TL);
    int tlen = 0;
    if (title) for (const char *p = title; *p; ++p) tlen++;
    int remain = COLS - 2; /* minus corners */
    if (title && tlen > 0) {
        console_putc(' ');
        console_write_ansi(CLR_PRIMARY);
        console_write(title);
        console_write_ansi(CLR_BORDER);
        remain -= (tlen + 2);
    }
    for (int i = 0; i < remain; ++i) console_putc(BOX_HZ);
    console_putc(BOX_TR);
    console_write_ansi(SGR_RESET);
    console_putc('\n');
}

static inline void console_draw_box_bottom(void) {
    console_write_ansi(CLR_BORDER);
    console_putc(BOX_BL);
    for (int i = 0; i < COLS - 2; ++i) console_putc(BOX_HZ);
    console_putc(BOX_BR);
    console_write_ansi(SGR_RESET);
    console_putc('\n');
}

/* Draw a boxed content line */
static inline void console_draw_box_line(const char *text) {
    console_write_ansi(CLR_BORDER);
    console_putc(BOX_VT);
    console_write_ansi(SGR_RESET);
    if (text) {
        console_putc(' ');
        console_write(text);
    }
    /* pad to COLS-2 */
    int tlen = 0;
    if (text) for (const char *p = text; *p; ++p) tlen++;
    int pad = COLS - 3 - tlen;
    for (int i = 0; i < pad; ++i) console_putc(' ');
    console_write_ansi(CLR_BORDER);
    console_putc(BOX_VT);
    console_write_ansi(SGR_RESET);
    console_putc('\n');
}

/* Draw a full box with content lines (NULL-terminated array) */
static inline void console_draw_box(const char *title, const char *lines[]) {
    console_draw_box_top(title);
    if (lines) {
        for (int i = 0; lines[i]; ++i)
            console_draw_box_line(lines[i]);
    }
    console_draw_box_bottom();
}

/* Draw a double-line box top (more prominent) */
static inline void console_draw_box_top_double(const char *title) {
    console_write_ansi(CLR_BORDER);
    console_putc(BOX_TL);
    console_putc(SEP_DOUBLE);
    int tlen = 0;
    if (title) for (const char *p = title; *p; ++p) tlen++;
    int remain = COLS - 4; /* minus 2 corners + 2 double chars */
    if (title && tlen > 0) {
        console_putc(' ');
        console_write_ansi(CLR_PRIMARY_BOLD);
        console_write(title);
        console_write_ansi(CLR_BORDER);
        remain -= (tlen + 2);
    }
    for (int i = 0; i < remain; ++i) console_putc(SEP_DOUBLE);
    console_putc(SEP_DOUBLE);
    console_putc(BOX_TR);
    console_write_ansi(SGR_RESET);
    console_putc('\n');
}

/* Draw a double-line box bottom */
static inline void console_draw_box_bottom_double(void) {
    console_write_ansi(CLR_BORDER);
    console_putc(BOX_BL);
    console_putc(SEP_DOUBLE);
    for (int i = 0; i < COLS - 4; ++i) console_putc(SEP_DOUBLE);
    console_putc(SEP_DOUBLE);
    console_putc(BOX_BR);
    console_write_ansi(SGR_RESET);
    console_putc('\n');
}

/* ================================================================
 * Status Labels
 * ================================================================ */

static inline void console_status_ok(const char *msg) {
    console_write_ansi(BOOT_OK_FG);
    console_write(STATUS_OK_STR);
    console_write_ansi(SGR_RESET);
    console_putc(' ');
    console_write(msg);
    console_putc('\n');
}

static inline void console_status_info(const char *msg) {
    console_write_ansi(BOOT_INFO_FG);
    console_write(STATUS_INFO_STR);
    console_write_ansi(SGR_RESET);
    console_putc(' ');
    console_write(msg);
    console_putc('\n');
}

static inline void console_status_warn(const char *msg) {
    console_write_ansi(BOOT_WARN_FG);
    console_write(STATUS_WARN_STR);
    console_write_ansi(SGR_RESET);
    console_putc(' ');
    console_write(msg);
    console_putc('\n');
}

static inline void console_status_error(const char *msg) {
    console_write_ansi(BOOT_ERROR_FG);
    console_write(STATUS_FAIL_STR);
    console_write_ansi(SGR_RESET);
    console_putc(' ');
    console_write(msg);
    console_putc('\n');
}

/* ================================================================
 * Progress Bar
 * ================================================================ */

/* Draw a progress bar: [####      ] 45%
 * @pct: 0-100 percentage
 * @width: bar width in characters (typically 40-60)
 */
static inline void console_draw_progress_bar(int pct, int bar_width) {
    if (bar_width <= 0) bar_width = 40;
    int filled = pct * bar_width / 100;
    if (filled > bar_width) filled = bar_width;

    console_write_ansi(MEM_BAR_BRACKET);
    console_putc('[');
    console_write_ansi(MEM_BAR_FILLED);
    for (int i = 0; i < filled; ++i) console_putc('#');
    console_write_ansi(MEM_BAR_EMPTY);
    for (int i = filled; i < bar_width; ++i) console_putc(' ');
    console_write_ansi(MEM_BAR_BRACKET);
    console_putc(']');
    console_putc(' ');
    console_write_ansi(MEM_BAR_PCT);
    /* Print percentage inline */
    char pbuf[8];
    int n = 0;
    int x = pct;
    if (x == 0) pbuf[n++] = '0';
    while (x > 0 && n < 6) { pbuf[n++] = '0' + (x % 10); x /= 10; }
    for (int i = n-1; i >= 0; i--) console_putc(pbuf[i]);
    console_write_ansi(SGR_RESET);
    console_putc('%');
}

/* Draw a styled progress bar with custom colors and block chars */
static inline void console_draw_progress_bar_styled(int pct, int bar_width,
                                                     const char *fill_fg,
                                                     const char *empty_fg,
                                                     const char *bracket_fg,
                                                     const char *pct_fg) {
    if (bar_width <= 0) bar_width = 40;
    int filled = pct * bar_width / 100;
    if (filled > bar_width) filled = bar_width;

    console_write_ansi(bracket_fg);
    console_putc('[');
    console_write_ansi(fill_fg);
    /* Use solid block chars for modern look */
    for (int i = 0; i < filled; ++i) console_putc('#');
    console_write_ansi(empty_fg);
    /* Use thin dots for empty space */
    for (int i = filled; i < bar_width; ++i) console_putc('.');
    console_write_ansi(bracket_fg);
    console_putc(']');
    console_putc(' ');
    console_write_ansi(pct_fg);
    char pbuf[8];
    int n = 0, x = pct;
    if (x == 0) pbuf[n++] = '0';
    while (x > 0 && n < 6) { pbuf[n++] = '0' + (x % 10); x /= 10; }
    for (int i = n-1; i >= 0; i--) console_putc(pbuf[i]);
    console_write_ansi(SGR_RESET);
    console_putc('%');
}

/* ================================================================
 * Spinner (animated loading indicator)
 * ================================================================ */

/* Draw a spinner frame at the current cursor position.
 * Call with frame index 0..3 cyclically. */
static inline void console_draw_spinner(int frame) {
    /* Save cursor */
    int sr, sc;
    console_get_cursor(&sr, &sc);
    console_write_ansi(SPINNER_FG);
    console_putc(SPINNER_FRAMES[frame & 3]);
    console_write_ansi(SGR_RESET);
    /* Move cursor back so next write overwrites the spinner */
    console_set_cursor(sr, sc);
}

/* ================================================================
 * Dashboard / System Info Panel
 * ================================================================ */

/* Draw a key-value line in dashboard style */
static inline void console_draw_dash_row(const char *label, const char *value) {
    console_write_ansi(DASH_LABEL);
    console_write_padded(label, 2);
    /* Pad label to fixed width */
    int llen = 0;
    for (const char *p = label; *p; ++p) llen++;
    for (int i = llen; i < 20; ++i) console_putc('.');
    console_putc(' ');
    console_write_ansi(DASH_VALUE);
    console_write(value);
    console_write_ansi(SGR_RESET);
    console_putc('\n');
}

/* Draw a section header in dashboard */
static inline void console_draw_dash_section(const char *title) {
    console_write_ansi(DASH_SECTION);
    console_putc(' ');
    console_write(title);
    console_write_ansi(SGR_RESET);
    console_putc('\n');
}

/* ================================================================
 * Banner / Header
 * ================================================================ */

/* Draw a full-width banner bar with centered text */
static inline void console_draw_banner(const char *text, const char *fg, const char *bg) {
    int tlen = 0;
    for (const char *p = text; *p; ++p) tlen++;
    int pad = (COLS - tlen) / 2;
    if (pad < 0) pad = 0;

    /* Save current row before writing */
    int row, col;
    console_get_cursor(&row, &col);

    console_write_ansi(bg);
    console_write_ansi(fg);
    /* Fill the entire row with spaces (background color) */
    for (int i = 0; i < COLS; ++i) console_putc(' ');
    /* Writing 80 spaces wraps to next line; go back to original row */
    console_set_cursor(row, pad);
    console_write(text);
    /* Move to next line */
    console_set_cursor(row + 1, 0);
    console_write_ansi(SGR_RESET);
}

/* ================================================================
 * Simple Table
 * ================================================================ */

/* Draw a table header row.  Each column is a fixed-width field.
 * @cols: array of column titles
 * @widths: array of column widths
 * @ncols: number of columns
 */
static inline void console_draw_table_header(const char *cols[], int widths[], int ncols) {
    console_write_ansi(PS_HEADER_BG);
    console_write_ansi(PS_HEADER_FG);
    for (int i = 0; i < ncols; ++i) {
        console_write(cols[i]);
        int pad = widths[i] - 0;
        if (cols[i]) {
            int clen = 0; for (const char *p = cols[i]; *p; ++p) clen++;
            pad -= clen;
        }
        for (int j = 0; j < pad; ++j) console_putc(' ');
    }
    console_write_ansi(SGR_RESET);
    console_putc('\n');
}

/* Draw a table row with colored values */
static inline void console_draw_table_row(const char *vals[], const char *colors[],
                                           int widths[], int ncols) {
    for (int i = 0; i < ncols; ++i) {
        if (colors && colors[i]) console_write_ansi(colors[i]);
        if (vals[i]) console_write(vals[i]);
        if (colors && colors[i]) console_write_ansi(SGR_RESET);
        if (vals[i]) {
            int vlen = 0; for (const char *p = vals[i]; *p; ++p) vlen++;
            int pad = widths[i] - vlen;
            for (int j = 0; j < pad; ++j) console_putc(' ');
        }
    }
    console_putc('\n');
}

/* ================================================================
 * Vertical Padding
 * ================================================================ */

/* Insert N blank lines */
static inline void console_vpad(int n) {
    for (int i = 0; i < n; ++i) console_putc('\n');
}

/* Center content vertically: insert enough blank lines so that
 * @content_lines lines appear centered in the 25-row screen. */
static inline void console_vcenter(int content_lines) {
    int pad = (ROWS - content_lines) / 2;
    if (pad < 0) pad = 0;
    for (int i = 0; i < pad; ++i) console_putc('\n');
}

/* ================================================================
 * Progress Bar with ETA (§8 Performance)
 * ================================================================ */

/* Draw a progress bar with ETA: [####......] 45% ETA: 3s
 * @pct: 0-100 percentage
 * @bar_width: bar width in characters
 * @eta_seconds: estimated seconds remaining (-1 = unknown)
 */
static inline void console_draw_progress_eta(int pct, int bar_width, int eta_seconds) {
    if (bar_width <= 0) bar_width = 40;
    int filled = pct * bar_width / 100;
    if (filled > bar_width) filled = bar_width;

    console_write_ansi(DASH_BAR_BRACKET);
    console_putc('[');
    console_write_ansi(DASH_BAR_FILLED);
    for (int i = 0; i < filled; ++i) console_putc('#');
    console_write_ansi(DASH_BAR_EMPTY);
    for (int i = filled; i < bar_width; ++i) console_putc('.');
    console_write_ansi(DASH_BAR_BRACKET);
    console_putc(']');
    console_putc(' ');

    /* Percentage */
    console_write_ansi(MEM_BAR_PCT);
    char pbuf[8];
    int n = 0, x = pct;
    if (x == 0) pbuf[n++] = '0';
    while (x > 0 && n < 6) { pbuf[n++] = '0' + (x % 10); x /= 10; }
    for (int i = n-1; i >= 0; i--) console_putc(pbuf[i]);
    console_putc('%');

    /* ETA */
    console_write_ansi(PROGRESS_ETA_FG);
    if (eta_seconds >= 0) {
        console_write("  ETA: ");
        char ebuf[8];
        if (eta_seconds == 0) {
            console_write("<1s");
        } else {
            int en = 0, es = eta_seconds;
            if (es == 0) ebuf[en++] = '0';
            while (es > 0 && en < 6) { ebuf[en++] = '0' + (es % 10); es /= 10; }
            for (int i = en-1; i >= 0; i--) console_putc(ebuf[i]);
            console_putc('s');
        }
    }
    console_write_ansi(SGR_RESET);
}

/* ================================================================
 * Confirmation Dialog (§5.6 - User confirmation)
 *
 * Draws a modal-style confirmation box.
 * Returns 1 if user would confirm (caller handles actual input).
 * The box is drawn; caller must handle the yes/no input loop.
 * ================================================================ */

/* Draw the confirmation dialog frame without waiting for input */
static inline void console_draw_confirm_dialog(const char *title, const char *message) {
    console_vpad(2);
    console_write_ansi(CONFIRM_BORDER_FG);
    console_putc(BOX_TL);
    for (int i = 0; i < COLS - 2; ++i) console_putc(BOX_HZ);
    console_putc(BOX_TR);
    console_putc('\n');

    /* Title line */
    console_putc(BOX_VT);
    console_write_ansi(SGR_RESET);
    console_putc(' ');
    console_write_ansi(CONFIRM_TITLE_FG);
    console_write(title);
    /* Pad */
    int tlen = 0;
    for (const char *p = title; *p; ++p) tlen++;
    int remain = COLS - 4 - tlen;
    for (int i = 0; i < remain; ++i) console_putc(' ');
    console_write_ansi(CONFIRM_BORDER_FG);
    console_putc(BOX_VT);
    console_write_ansi(SGR_RESET);
    console_putc('\n');

    /* Separator */
    console_write_ansi(CONFIRM_BORDER_FG);
    console_putc(BOX_VT);
    for (int i = 0; i < COLS - 2; ++i) console_putc(SEP_DOT);
    console_putc(BOX_VT);
    console_write_ansi(SGR_RESET);
    console_putc('\n');

    /* Message */
    console_write_ansi(CONFIRM_BORDER_FG);
    console_putc(BOX_VT);
    console_write_ansi(SGR_RESET);
    console_putc(' ');
    console_write_ansi(CONFIRM_PROMPT_FG);
    console_write(message);
    int mlen = 0;
    for (const char *p = message; *p; ++p) mlen++;
    remain = COLS - 4 - mlen;
    for (int i = 0; i < remain; ++i) console_putc(' ');
    console_write_ansi(CONFIRM_BORDER_FG);
    console_putc(BOX_VT);
    console_write_ansi(SGR_RESET);
    console_putc('\n');

    /* Prompt line */
    console_write_ansi(CONFIRM_BORDER_FG);
    console_putc(BOX_VT);
    console_write_ansi(SGR_RESET);
    console_putc(' ');
    console_write_ansi(CONFIRM_PROMPT_FG);
    console_write("[");
    console_write_ansi(CONFIRM_YES_FG);
    console_write("Y");
    console_write_ansi(CONFIRM_PROMPT_FG);
    console_write("]es / [");
    console_write_ansi(CONFIRM_NO_FG);
    console_write("N");
    console_write_ansi(CONFIRM_PROMPT_FG);
    console_write("]o");
    remain = COLS - 4 - 15; /* "[Y]es / [N]o" = 15 chars */
    for (int i = 0; i < remain; ++i) console_putc(' ');
    console_write_ansi(CONFIRM_BORDER_FG);
    console_putc(BOX_VT);
    console_write_ansi(SGR_RESET);
    console_putc('\n');

    /* Bottom */
    console_write_ansi(CONFIRM_BORDER_FG);
    console_putc(BOX_BL);
    for (int i = 0; i < COLS - 2; ++i) console_putc(BOX_HZ);
    console_putc(BOX_BR);
    console_write_ansi(SGR_RESET);
    console_putc('\n');
}

/* ================================================================
 * Notification Banner (§5.5)
 *
 * Draws a colored notification banner at the current cursor position.
 * @type: "info", "success", "warn", "error"
 * @message: notification text
 * ================================================================ */
static inline void console_draw_notification(const char *type, const char *message) {
    const char *fg = NOTIFY_INFO_FG;
    const char *icon = "[i]";

    if (type[0] == 's' || type[0] == 'S') { fg = NOTIFY_SUCCESS_FG; icon = "[*]"; }
    else if (type[0] == 'w' || type[0] == 'W') { fg = NOTIFY_WARN_FG; icon = "[!]"; }
    else if (type[0] == 'e' || type[0] == 'E') { fg = NOTIFY_ERROR_FG; icon = "[x]"; }

    console_write_ansi(fg);
    console_putc(' ');
    console_putc(BOX_TL);
    for (int i = 0; i < COLS - 4; ++i) console_putc(BOX_HZ);
    console_putc(BOX_TR);
    console_putc('\n');

    console_putc(' ');
    console_putc(BOX_VT);
    console_putc(' ');
    console_write(icon);
    console_putc(' ');
    console_write(message);
    /* Pad to fill */
    int mlen = 0;
    for (const char *p = icon; *p; ++p) mlen++;
    for (const char *p = message; *p; ++p) mlen++;
    int remain = COLS - 7 - mlen;
    for (int i = 0; i < remain; ++i) console_putc(' ');
    console_putc(BOX_VT);
    console_putc('\n');

    console_putc(' ');
    console_putc(BOX_BL);
    for (int i = 0; i < COLS - 4; ++i) console_putc(BOX_HZ);
    console_putc(BOX_BR);
    console_write_ansi(SGR_RESET);
    console_putc('\n');
}

/* ================================================================
 * Lock Screen / Time Display (§7.3)
 *
 * Draws a large clock-style time display centered on screen.
 * @time_str: formatted time string (e.g., "14:30")
 * @date_str: formatted date string (e.g., "2026-06-19 Friday")
 * ================================================================ */
static inline void console_draw_lock_screen(const char *time_str, const char *date_str) {
    console_clear();
    console_vcenter(8);

    /* Top decorative line */
    console_write_ansi(LOCK_HINT_FG);
    console_draw_hr(SEP_DOT);
    console_write_ansi(SGR_RESET);
    console_vpad(1);

    /* Time — large and centered */
    console_write_ansi(LOCK_TIME_FG);
    console_write_centered(time_str);
    console_write_ansi(SGR_RESET);
    console_putc('\n');

    /* Date */
    console_write_ansi(LOCK_DATE_FG);
    console_write_centered(date_str);
    console_write_ansi(SGR_RESET);
    console_putc('\n');

    console_vpad(2);

    /* Hint */
    console_write_ansi(LOCK_HINT_FG);
    console_write_centered("Press any key to unlock...");
    console_write_ansi(SGR_RESET);
    console_putc('\n');

    console_vpad(1);

    /* Bottom decorative line */
    console_write_ansi(LOCK_HINT_FG);
    console_draw_hr(SEP_DOT);
    console_write_ansi(SGR_RESET);
}

/* ================================================================
 * Accessibility Badge
 *
 * Draws a small badge indicating accessibility mode is active.
 * ================================================================ */
static inline void console_draw_a11y_badge(void) {
    if (theme_is_high_contrast() || g_reduced_motion) {
        console_write_ansi(A11Y_BADGE_FG);
        console_write(" [");
        if (theme_is_high_contrast()) console_write("HC");
        if (theme_is_high_contrast() && g_reduced_motion) console_write("+");
        if (g_reduced_motion) console_write("RM");
        console_write("]");
        console_write_ansi(SGR_RESET);
    }
}

/* ================================================================
 * Enhanced Error Message with Suggestion
 *
 * Prints an error message followed by an actionable suggestion.
 * ================================================================ */
static inline void console_error_with_hint(const char *error_msg, const char *hint) {
    console_write_ansi(SHELL_CMD_ERROR);
    console_write("Error: ");
    console_write(error_msg);
    console_write_ansi(SGR_RESET);
    console_putc('\n');
    if (hint && hint[0]) {
        console_write_ansi(CLR_MUTED);
        console_write("  Hint: ");
        console_write(hint);
        console_write_ansi(SGR_RESET);
        console_putc('\n');
    }
}

#endif /* LAYOUT_H */
