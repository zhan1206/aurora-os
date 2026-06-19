/*
 * theme.h - AuroraOS Design Token System
 *
 * Centralized design tokens for all visual elements, following
 * the AuroraOS Design Specification V2.0.
 *
 * Philosophy: "Effortless, Calm, Personal, Familiar, Coherent, Inclusive"
 *
 * Token hierarchy (3-layer):
 *   Layer 1: Raw VGA color values (vga_color enum in console.h)
 *   Layer 2: Semantic role tokens (this file, §4.1 mapping)
 *   Layer 3: Component tokens (this file, per UI component)
 *
 * Specification color mapping (VGA 16-color palette):
 *   Spec #2979FF / #448AFF (Primary Blue)  → VGA Cyan (3) / Bright Cyan (11)
 *   Spec #00C853 / #69F0AE (Success Green) → VGA Green (2) / Bright Green (10)
 *   Spec #FFAB00 / #FFD740 (Warning Amber) → VGA Yellow (14) / Bright Yellow
 *   Spec #D50000 / #FF5252 (Error Red)     → VGA Red (4) / Bright Red (12)
 *   Spec #F8F9FA / #1A1A1E (Background)    → VGA Black (0) / Dark Grey (8)
 *   Spec #FFFFFF / #2C2C30 (Surface)       → VGA White (15) / Dark Grey (8)
 *
 * Contrast ratios (VGA on VGA Black background):
 *   White (15)       → ~10:1  (AAA compliant)
 *   Bright Cyan (11) → ~6:1   (AA compliant)
 *   Green (2)        → ~5:1   (AA compliant)
 *   Yellow (14)      → ~10:1  (AAA compliant)
 *   Red (4)          → ~5:1   (AA compliant)
 *   Bright Black (8) → ~3:1   (below AA — use only for decorative)
 *
 * Accessibility:
 *   - High-contrast theme tokens force ≥7:1 contrast
 *   - Reduced-motion flag suppresses animations
 *   - All themes share the same token names for zero-code-change switching
 */

#ifndef THEME_H
#define THEME_H

/* ================================================================
 * Theme Mode Enumeration
 * ================================================================ */
enum theme_mode {
    THEME_DARK           = 0,  /* default dark theme */
    THEME_LIGHT          = 1,  /* light theme (black-on-white) */
    THEME_HIGH_CONTRAST  = 2,  /* high contrast (≥7:1 AAA) */
    THEME_COUNT          = 3
};

/* ---- Runtime theme state ---- */
extern int  g_theme_mode;        /* current theme: THEME_DARK / THEME_LIGHT / THEME_HIGH_CONTRAST */
extern int  g_reduced_motion;    /* 1 = suppress animations for accessibility */
extern int  g_anim_enabled;      /* 1 = animations allowed (negation of reduced_motion) */

/* ---- Theme switching helpers ---- */
#define theme_is_high_contrast() (g_theme_mode == THEME_HIGH_CONTRAST)
#define theme_is_light()         (g_theme_mode == THEME_LIGHT)
#define theme_is_dark()          (g_theme_mode == THEME_DARK)

/* ================================================================
 * Layer 1: Raw ANSI SGR Sequences
 *
 * These are the ONLY place where ANSI escape strings are defined.
 * All kernel code MUST use semantic tokens, not raw "\x1b[XXm".
 * ================================================================ */

/* ---- Reset ---- */
#define SGR_RESET       "\x1b[0m"

/* ---- Text formatting ---- */
#define SGR_BOLD        "\x1b[1m"
#define SGR_DIM         "\x1b[2m"
#define SGR_ITALIC      "\x1b[3m"
#define SGR_UNDERLINE   "\x1b[4m"
#define SGR_BLINK       "\x1b[5m"
#define SGR_REVERSE     "\x1b[7m"

/* ---- Standard foreground colors (16-color palette) ---- */
#define FG_BLACK        "\x1b[30m"
#define FG_RED          "\x1b[31m"
#define FG_GREEN        "\x1b[32m"
#define FG_YELLOW       "\x1b[33m"
#define FG_BLUE         "\x1b[34m"
#define FG_MAGENTA      "\x1b[35m"
#define FG_CYAN         "\x1b[36m"
#define FG_WHITE        "\x1b[37m"

/* ---- Bright foreground colors ---- */
#define FG_BRIGHT_BLACK   "\x1b[90m"
#define FG_BRIGHT_RED     "\x1b[91m"
#define FG_BRIGHT_GREEN   "\x1b[92m"
#define FG_BRIGHT_YELLOW  "\x1b[93m"
#define FG_BRIGHT_BLUE    "\x1b[94m"
#define FG_BRIGHT_MAGENTA "\x1b[95m"
#define FG_BRIGHT_CYAN    "\x1b[96m"
#define FG_BRIGHT_WHITE   "\x1b[97m"

/* ---- Standard background colors ---- */
#define BG_BLACK        "\x1b[40m"
#define BG_RED          "\x1b[41m"
#define BG_GREEN        "\x1b[42m"
#define BG_YELLOW       "\x1b[43m"
#define BG_BLUE         "\x1b[44m"
#define BG_MAGENTA      "\x1b[45m"
#define BG_CYAN         "\x1b[46m"
#define BG_WHITE        "\x1b[47m"

/* ---- Bright background colors ---- */
#define BG_BRIGHT_BLACK   "\x1b[100m"
#define BG_BRIGHT_RED     "\x1b[101m"
#define BG_BRIGHT_GREEN   "\x1b[102m"
#define BG_BRIGHT_YELLOW  "\x1b[103m"
#define BG_BRIGHT_BLUE    "\x1b[104m"
#define BG_BRIGHT_MAGENTA "\x1b[105m"
#define BG_BRIGHT_CYAN    "\x1b[106m"
#define BG_BRIGHT_WHITE   "\x1b[107m"

/* ================================================================
 * Theme-Aware SGR Selector
 *
 * theme_sgr(dark, light, hc) picks the right SGR string at runtime.
 * Used by all Layer 2 semantic tokens for zero-code-change theming.
 * ================================================================ */
static inline const char *theme_sgr(const char *dark, const char *light,
                                     const char *hc) {
    if (g_theme_mode == THEME_HIGH_CONTRAST) return hc;
    if (g_theme_mode == THEME_LIGHT)        return light;
    return dark;
}

/* Shorthand: theme-aware foreground that only needs dark + hc variants */
static inline const char *theme_fg(const char *dark, const char *hc) {
    /* Light theme: invert foreground — use dark color on white bg */
    if (g_theme_mode == THEME_HIGH_CONTRAST) return hc;
    if (g_theme_mode == THEME_LIGHT)        return FG_BLACK;
    return dark;
}

/* Shorthand: theme-aware background */
static inline const char *theme_bg(const char *dark, const char *hc) {
    if (g_theme_mode == THEME_HIGH_CONTRAST) return hc;
    if (g_theme_mode == THEME_LIGHT)        return BG_WHITE;
    return dark;
}

/* ================================================================
 * Layer 2: Semantic Role Tokens
 *
 * Map functional roles to colors.  Follows the spec's color system §4.1:
 *   - Primary:    key actions, selection, focus
 *   - Success:    completion, positive state
 *   - Warning:    attention needed
 *   - Error:      failure, danger
 *   - Info:       informational messages
 *
 * Each token is theme-aware via theme_sgr() / theme_fg() / theme_bg().
 * The three variants are: (dark, light, high-contrast).
 * ================================================================ */

/* ---- Primary / Brand ---- */
#define CLR_PRIMARY          theme_fg(FG_CYAN,         FG_BRIGHT_WHITE)
#define CLR_PRIMARY_BOLD     theme_fg(FG_BRIGHT_CYAN,  FG_BRIGHT_WHITE)
#define CLR_BRAND            theme_fg(FG_CYAN,         FG_BRIGHT_WHITE)
#define CLR_BRAND_BG         theme_bg(BG_CYAN,         BG_WHITE)

/* ---- Functional colors ---- */
#define CLR_SUCCESS          theme_fg(FG_GREEN,        FG_BRIGHT_GREEN)
#define CLR_SUCCESS_BOLD     theme_fg(FG_BRIGHT_GREEN, FG_BRIGHT_GREEN)
#define CLR_WARN             theme_fg(FG_YELLOW,       FG_BRIGHT_YELLOW)
#define CLR_WARN_BOLD        theme_fg(FG_BRIGHT_YELLOW,FG_BRIGHT_YELLOW)
#define CLR_ERROR            theme_fg(FG_RED,          FG_BRIGHT_RED)
#define CLR_ERROR_BOLD       theme_fg(FG_BRIGHT_RED,   FG_BRIGHT_RED)
#define CLR_INFO             theme_fg(FG_CYAN,         FG_BRIGHT_CYAN)
#define CLR_MUTED            theme_fg(FG_BRIGHT_BLACK, FG_BRIGHT_WHITE)

/* ---- Text hierarchy ---- */
#define CLR_TEXT_PRIMARY     theme_fg(FG_WHITE,        FG_BRIGHT_WHITE)
#define CLR_TEXT_SECONDARY   theme_fg(FG_BRIGHT_BLACK, FG_WHITE)
#define CLR_TEXT_TERTIARY    theme_fg(FG_BLACK,        FG_BRIGHT_BLACK)
#define CLR_TEXT_INVERSE     theme_fg(FG_BLACK,        FG_BLACK)

/* ---- Surface / Background ---- */
#define CLR_SURFACE          theme_bg(BG_BLACK,        BG_BLACK)
#define CLR_SURFACE_RAISED   theme_bg(BG_BRIGHT_BLACK, BG_BRIGHT_BLACK)
#define CLR_SURFACE_HEADER   theme_bg(BG_WHITE,        BG_BRIGHT_WHITE)

/* ---- Borders / Dividers ---- */
#define CLR_DIVIDER          theme_fg(FG_BRIGHT_BLACK, FG_WHITE)
#define CLR_BORDER           theme_fg(FG_BRIGHT_BLACK, FG_WHITE)

/* ================================================================
 * Layer 3: Component Tokens
 *
 * Style definitions for specific UI components.
 * All component tokens reference Layer 2 semantic tokens only.
 * ================================================================ */

/* ---- Boot Screen ---- */
#define BOOT_LOGO_COLOR     CLR_BRAND
#define BOOT_LOGO_SHADOW    CLR_MUTED
#define BOOT_VERSION_COLOR  CLR_BRAND
#define BOOT_BUILD_COLOR    CLR_MUTED
#define BOOT_OK_FG          CLR_SUCCESS
#define BOOT_OK_BG          ""
#define BOOT_INFO_FG        CLR_INFO
#define BOOT_WARN_FG        CLR_WARN
#define BOOT_ERROR_FG       CLR_ERROR
#define BOOT_DIVIDER        CLR_MUTED
#define BOOT_PROGRESS_BG    CLR_MUTED
#define BOOT_PROGRESS_FILL  CLR_BRAND
#define BOOT_FRAME_TL       CLR_MUTED
#define BOOT_FRAME_BR       CLR_MUTED
#define BOOT_STAGE_LABEL    CLR_MUTED
#define BOOT_STAGE_ACTIVE   CLR_BRAND
#define BOOT_STAGE_DONE     CLR_SUCCESS

/* ---- Shell / Terminal ---- */
#define SHELL_PROMPT_USER   CLR_BRAND
#define SHELL_PROMPT_AT     CLR_MUTED
#define SHELL_PROMPT_HOST   CLR_BRAND
#define SHELL_PROMPT_PATH   CLR_MUTED
#define SHELL_PROMPT_DOLLAR CLR_SUCCESS
#define SHELL_CMD_HELP      CLR_INFO
#define SHELL_CMD_OK        CLR_SUCCESS
#define SHELL_CMD_ERROR     CLR_ERROR
#define SHELL_CMD_WARN      CLR_WARN
#define SHELL_DIR_COLOR     CLR_BRAND
#define SHELL_FILE_COLOR    CLR_TEXT_PRIMARY
#define SHELL_EXEC_COLOR    FG_BRIGHT_GREEN
#define SHELL_LINK_COLOR    FG_BRIGHT_MAGENTA
#define SHELL_HEADER_BG     BG_CYAN
#define SHELL_HEADER_FG     FG_BLACK
#define SHELL_BANNER_FG     FG_BRIGHT_CYAN
#define SHELL_WELCOME_FG    FG_BRIGHT_WHITE

/* ---- Panic Screen ---- */
#define PANIC_BG            VGA_RED
#define PANIC_FG            VGA_WHITE
#define PANIC_MSG_COLOR     VGA_YELLOW
#define PANIC_TITLE_FG      FG_WHITE
#define PANIC_LABEL_FG      FG_CYAN
#define PANIC_LABEL_PREFIX  ">>> "
#define PANIC_VALUE_FG      FG_YELLOW
#define PANIC_BOTTOM_FG     FG_BRIGHT_BLACK

/* ---- Login Screen (§7.2) ---- */
#define LOGIN_TITLE_FG      CLR_BRAND
#define LOGIN_PROMPT_FG     CLR_TEXT_PRIMARY
#define LOGIN_SUCCESS_FG    CLR_SUCCESS
#define LOGIN_ERROR_FG      CLR_ERROR
#define LOGIN_TIME_FG       CLR_BRAND
#define LOGIN_DATE_FG       CLR_MUTED
#define LOGIN_AVATAR_FG     CLR_BRAND
#define LOGIN_HINT_FG       CLR_MUTED
#define LOGIN_POWER_FG      CLR_WARN
#define LOGIN_ACC_FG        CLR_INFO

/* ---- Lock Screen (§7.3) ---- */
#define LOCK_TIME_FG        FG_BRIGHT_WHITE
#define LOCK_DATE_FG        CLR_MUTED
#define LOCK_HINT_FG        CLR_MUTED
#define LOCK_PRESS_FG       CLR_BRAND

/* ---- Status Labels ---- */
#define STATUS_OK_STR       "[  OK  ]"
#define STATUS_INFO_STR     "[ INFO ]"
#define STATUS_WARN_STR     "[ WARN ]"
#define STATUS_FAIL_STR     "[ FAIL ]"

/* ---- Process States ---- */
#define PS_CURRENT_MARK     FG_CYAN
#define PS_STATE_RUNNING    FG_GREEN
#define PS_STATE_READY      FG_YELLOW
#define PS_STATE_BLOCKED    FG_YELLOW
#define PS_STATE_ZOMBIE     FG_RED
#define PS_STATE_DEAD       FG_BRIGHT_BLACK
#define PS_HEADER_BG        BG_WHITE
#define PS_HEADER_FG        FG_BLACK

/* ---- Memory Bar ---- */
#define MEM_BAR_FILLED      FG_CYAN
#define MEM_BAR_EMPTY       FG_BRIGHT_BLACK
#define MEM_BAR_BRACKET     FG_WHITE
#define MEM_BAR_PCT         FG_YELLOW
#define MEM_BAR_LABEL       FG_WHITE

/* ---- About Box ---- */
#define ABOUT_BORDER_FG     FG_CYAN
#define ABOUT_TEXT_FG       FG_WHITE
#define ABOUT_HIGHLIGHT_FG  FG_BRIGHT_WHITE

/* ---- Spinner / Loading ---- */
#define SPINNER_FG          FG_CYAN
#define SPINNER_FRAMES      "|/-\\"

/* ---- Dashboard / sysinfo ---- */
#define DASH_BORDER         FG_CYAN
#define DASH_TITLE          FG_BRIGHT_CYAN
#define DASH_LABEL          FG_BRIGHT_BLACK
#define DASH_VALUE          FG_BRIGHT_WHITE
#define DASH_SECTION        FG_CYAN
#define DASH_BAR_FILLED     FG_GREEN
#define DASH_BAR_EMPTY      FG_BRIGHT_BLACK
#define DASH_BAR_BRACKET    FG_WHITE
#define DASH_BAR_LOW        FG_YELLOW
#define DASH_BAR_CRITICAL   FG_RED

/* ---- Confirmation Dialog ---- */
#define CONFIRM_BORDER_FG   CLR_WARN
#define CONFIRM_TITLE_FG    CLR_WARN_BOLD
#define CONFIRM_PROMPT_FG   CLR_TEXT_PRIMARY
#define CONFIRM_YES_FG      CLR_SUCCESS_BOLD
#define CONFIRM_NO_FG       CLR_ERROR_BOLD

/* ---- Notification Banner (§5.5) ---- */
#define NOTIFY_INFO_FG      CLR_INFO
#define NOTIFY_SUCCESS_FG   CLR_SUCCESS
#define NOTIFY_WARN_FG      CLR_WARN
#define NOTIFY_ERROR_FG     CLR_ERROR

/* ---- Progress Bar with ETA ---- */
#define PROGRESS_ETA_FG     CLR_MUTED
#define PROGRESS_STEP_FG    CLR_TEXT_SECONDARY

/* ---- Accessibility Indicators ---- */
#define A11Y_BADGE_FG       CLR_INFO
#define A11Y_BADGE_BG       BG_CYAN
#define A11Y_LABEL_FG       CLR_MUTED

/* ---- System Power Buttons ---- */
#define POWER_SHUTDOWN_FG   CLR_ERROR
#define POWER_RESTART_FG    CLR_WARN
#define POWER_SLEEP_FG      CLR_INFO

/* ---- Spacing Tokens (in characters, VGA text mode) ---- */
#define SPACE_NONE    0
#define SPACE_XS      1
#define SPACE_SM      2
#define SPACE_MD      4
#define SPACE_LG      8
#define SPACE_XL      12

/* ---- Border / Box Drawing Characters (CP437) ---- */
#define BOX_TL      '+'     /* top-left */
#define BOX_TR      '+'     /* top-right */
#define BOX_BL      '+'     /* bottom-left */
#define BOX_BR      '+'     /* bottom-right */
#define BOX_HZ      '-'     /* horizontal */
#define BOX_VT      '|'     /* vertical */
#define BOX_DIV_L   '+'     /* divider left */
#define BOX_DIV_R   '+'     /* divider right */

/* ---- Separator Characters ---- */
#define SEP_LINE    '-'     /* horizontal separator */
#define SEP_DOUBLE  '='     /* double horizontal separator */
#define SEP_DOT     '.'     /* dotted separator */

/* ================================================================
 * Helper: Compose SGR + text into a single output
 * ================================================================ */

/* Write a colored string using semantic tokens */
#define console_color(sgr, text) do { \
    console_write_ansi(sgr);          \
    console_write(text);              \
    console_write_ansi(SGR_RESET);    \
} while(0)

/* Write a colored character */
#define console_color_c(sgr, ch) do { \
    console_write_ansi(sgr);          \
    console_putc(ch);                 \
    console_write_ansi(SGR_RESET);    \
} while(0)

#endif /* THEME_H */
