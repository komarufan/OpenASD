/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, ASD Project Contributors
 *
*/

#include "init_internal.h"
#include "../kernel/console/fbcon.h"

/* ------------------------------------------------------------------ */
/* Colour palette (RGB24)                                               */
/* ------------------------------------------------------------------ */

#define TUI_BG_BLUE     0x0000AA   /* screen background                */
#define TUI_FG_WHITE    0xEEEEEE   /* normal text on blue              */
#define TUI_FG_YELLOW   0xFFFF55   /* status bar / highlights          */
#define TUI_DLG_BG      0xAAAAAA   /* dialog box background            */
#define TUI_DLG_FG      0x000000   /* dialog box text                  */
#define TUI_BTN_SEL_BG  0x0000AA   /* selected button background       */
#define TUI_BTN_SEL_FG  0xFFFFFF   /* selected button text             */
#define TUI_BTN_NRM_BG  0x000000   /* normal button background         */
#define TUI_BTN_NRM_FG  0xAAAAAA   /* normal button text               */
#define TUI_SHADOW      0x000000   /* drop shadow                      */
#define TUI_BAR_BG      0x0000AA   /* progress bar background          */
#define TUI_BAR_FG      0xFFFF55   /* progress bar fill                */
#define TUI_ERR_BG      0xAA0000   /* error dialog background          */
#define TUI_ERR_FG      0xFFFFFF   /* error dialog text                */

/* ------------------------------------------------------------------ */
/* Low-level TUI primitives                                             */
/* ------------------------------------------------------------------ */

static void tui_screen_bg(void) {
    fb_console_set_colors(TUI_FG_WHITE, TUI_BG_BLUE);
    fb_console_clear();

    fb_console_set_colors(TUI_FG_YELLOW, TUI_BG_BLUE);
    uint32_t cols, rows;
    fb_console_get_size(&cols, &rows);
    fb_console_set_cursor((cols - 24) / 2, 1);
    fb_console_puts("  OpenASD Installer v1.0  ");
    fb_console_tick();
}

/* Draw the bottom status bar */
static void tui_status_bar(const char *left, const char *right) {
    uint32_t cols, rows;
    fb_console_get_size(&cols, &rows);
    fb_console_set_colors(TUI_FG_YELLOW, TUI_BG_BLUE);
    fb_console_set_cursor(0, rows - 1);
    fb_console_puts(" ");
    fb_console_puts(left);
    /* pad to right side */
    size_t used = 1 + strlen(left);
    size_t rlen = strlen(right);
    while (used + rlen + 1 < (size_t)cols) { fb_console_putc(' '); used++; }
    fb_console_puts(right);
    /* DO NOT write to the very last cell (cols-1, rows-1) to avoid scrolling */
    if (used + rlen < (size_t)cols - 1) {
        fb_console_putc(' ');
    }
}

/* Draw a centred dialog box; returns (x, y) of top-left corner via out_x/y */
static void tui_dialog(uint32_t w, uint32_t h, const char *title,
                        uint32_t *out_x, uint32_t *out_y) {
    uint32_t cols, rows;
    fb_console_get_size(&cols, &rows);
    uint32_t x = (cols > w) ? (cols - w) / 2 : 0;
    /* FIX Bug 1: clamp y to >= 3 so dialog never overlaps the screen
     * header banner drawn at row 1 by tui_screen_bg(). */
    uint32_t y;
    if (rows > h + 4)
        y = (rows - h) / 2;
    else
        y = 3;
    if (y < 3) y = 3;

    /* Drop shadow */
    fb_console_set_colors(TUI_SHADOW, TUI_SHADOW);
    for (uint32_t i = 1; i <= h; i++) {
        fb_console_set_cursor(x + w, y + i);
        fb_console_putc(' ');
        fb_console_putc(' ');
    }
    fb_console_set_cursor(x + 2, y + h);
    for (uint32_t i = 0; i < w; i++) fb_console_putc(' ');

    /* Box fill */
    fb_console_set_colors(TUI_DLG_FG, TUI_DLG_BG);
    for (uint32_t i = 0; i < h; i++) {
        fb_console_set_cursor(x, y + i);
        for (uint32_t j = 0; j < w; j++) fb_console_putc(' ');
    }

    /* Box border + title */
    fb_console_draw_box(x, y, w, h, title);

    if (out_x) *out_x = x;
    if (out_y) *out_y = y;
}


static void tui_field_putc(uint32_t col, uint32_t row, char c) {
    fb_console_set_colors(TUI_BTN_SEL_FG, TUI_BTN_SEL_BG);
    fb_console_set_cursor(col, row);
    fb_console_putc(c);
}

static void tui_field_clear(uint32_t col, uint32_t row, uint32_t width) {
    for (uint32_t j = 0; j < width; j++)
        tui_field_putc(col + j, row, ' ');
}

static void tui_put_u64_at(uint32_t col, uint32_t row, uint64_t v) {
    char buf[24];
    int n = 23;
    buf[n] = '\0';
    if (v == 0) { buf[--n] = '0'; }
    while (v) { buf[--n] = (char)('0' + (v % 10)); v /= 10; }
    fb_console_set_colors(TUI_DLG_FG, TUI_DLG_BG);
    fb_console_set_cursor(col, row);
    fb_console_puts(&buf[n]);
}

/* Read one line into a fixed TUI field (fb only — never touches global serial console). */
static void tui_readline_field(uint32_t col, uint32_t row, char *buf, size_t cap,
                               uint32_t width, int masked) {
    if (!buf || cap < 2 || width == 0) return;
    if (width >= cap) width = cap - 1;
    flush_input();

    uint32_t i = 0;
    buf[0] = '\0';
    tui_field_clear(col, row, width);

    for (;;) {
        char ch = 0;
        while (!input_getc_nonblock(&ch)) {
            fb_console_tick();
            __asm__ volatile("pause");
        }

        if (ch == '\r' || ch == '\n') {
            if (ch == '\r') {
                char next;
                if (input_getc_nonblock(&next) && next != '\n')
                    input_unget(next);
            }
            buf[i] = '\0';
            return;
        }
        if ((ch == '\b' || ch == 127) && i > 0) {
            i--;
            buf[i] = '\0';
            tui_field_putc(col + i, row, ' ');
            fb_console_set_cursor(col + i, row);
            continue;
        }
        if (i >= width)
            continue;
        if (ch < 0x20 || ch > 0x7e)
            continue;

        buf[i++] = ch;
        buf[i] = '\0';
        tui_field_putc(col + i - 1, row, masked ? '*' : ch);
    }
}

/* Draw a button at (x,y); selected = highlighted */
static void tui_button(uint32_t x, uint32_t y, const char *label, int sel) {
    if (sel) fb_console_set_colors(TUI_BTN_SEL_FG, TUI_BTN_SEL_BG);
    else     fb_console_set_colors(TUI_BTN_NRM_FG, TUI_BTN_NRM_BG);
    fb_console_set_cursor(x, y);
    fb_console_putc('[');
    fb_console_puts(label);
    fb_console_putc(']');
}

/* Put text inside a dialog at relative (dx, dy) from dialog origin */
static void tui_text(uint32_t bx, uint32_t by, uint32_t dx, uint32_t dy,
                     const char *s) {
    fb_console_set_colors(TUI_DLG_FG, TUI_DLG_BG);
    fb_console_set_cursor(bx + dx, by + dy);
    fb_console_puts(s);
}

/* Draw a horizontal progress bar at (x,y) of width w, filled pct% */
static void tui_progress_bar(uint32_t x, uint32_t y, uint32_t w, uint64_t pct) {
    if (pct > 100) pct = 100;
    uint32_t fill = (uint32_t)(pct * w / 100);
    fb_console_set_cursor(x, y);
    for (uint32_t i = 0; i < w; i++) {
        if (i < fill) {
            fb_console_set_colors(TUI_BAR_FG, TUI_BAR_BG);
            fb_console_putc('#');
        } else {
            fb_console_set_colors(TUI_DLG_FG, TUI_DLG_BG);
            fb_console_putc('-');
        }
    }
    fb_console_set_colors(TUI_DLG_FG, TUI_DLG_BG);
    fb_console_set_cursor(x + w + 1, y);
    fb_console_putc(' ');
    tui_put_u64_at(x + w + 2, y, pct);
    fb_console_putc('%');
}

/* ------------------------------------------------------------------ */
/* Screen 1: Welcome                                                    */
/* ------------------------------------------------------------------ */

/* Returns 1 = Install, 0 = Live Shell, -1 = Quit */
int tui_screen_welcome(void) {
    int sel = 0;   /* 0=Install 1=Shell */
    for (;;) {
        tui_screen_bg();

        uint32_t bx, by;
        tui_dialog(58, 14, " Welcome to OpenASD ", &bx, &by);

        tui_text(bx, by, 3, 2,
            "Welcome to the OpenASD installation wizard.");
        tui_text(bx, by, 3, 3,
            "This program will guide you through installing");
        tui_text(bx, by, 3, 4,
            "OpenASD onto your system.");
        tui_text(bx, by, 3, 6,
            "OpenASD is a research microkernel OS.  All data");
        tui_text(bx, by, 3, 7,
            "on the selected disk WILL BE ERASED.");
        tui_text(bx, by, 3, 9,
            "Choose an option below:");

        tui_button(bx + 8,  by + 11, " Install OpenASD ", sel == 0);
        tui_button(bx + 32, by + 11, " Live Shell ",      sel == 1);

        tui_status_bar("[Enter] Select  [Arrows] Navigate  [Q] Quit", "");

        int key = read_menu_key();
        if (key == 1 || key == 2) sel ^= 1;
        else if (key == 3) return (sel == 0) ? 1 : 0;
        else if (key == 4) return -1;
    }
}

/* ------------------------------------------------------------------ */
/* Screen 2: Disk selection                                             */
/* ------------------------------------------------------------------ */

/* Returns 1 on selection, 0 on cancel; sets *out */
int tui_screen_disk(block_dev_t **out) {
    block_dev_t *items[BLOCK_MAX_DEVS];
    int count = 0;
    for (int i = 0; i < block_count() && count < BLOCK_MAX_DEVS; i++) {
        block_dev_t *d = block_get(i);
        if (d && d->read && d->write) items[count++] = d;
    }
    if (count == 0) return -1;

    int sel = 0;
    for (;;) {
        tui_screen_bg();

        uint32_t bx, by;
        tui_dialog(60, 6 + count + 3, " Select Installation Disk ", &bx, &by);

        tui_text(bx, by, 3, 2, "WARNING: ALL DATA ON THE SELECTED DISK WILL BE LOST.");
        tui_text(bx, by, 3, 3, "Select the target disk:");

        fb_console_set_colors(TUI_DLG_FG, TUI_DLG_BG);
        for (int i = 0; i < count; i++) {
            uint32_t row = by + 5 + (uint32_t)i;
            if (i == sel) {
                fb_console_set_colors(TUI_BTN_SEL_FG, TUI_BTN_SEL_BG);
                fb_console_set_cursor(bx + 3, row);
                fb_console_puts(">> ");
            } else {
                fb_console_set_colors(TUI_DLG_FG, TUI_DLG_BG);
                fb_console_set_cursor(bx + 3, row);
                fb_console_puts("   ");
            }
            fb_console_set_colors(TUI_DLG_FG, TUI_DLG_BG);
            uint32_t col = bx + 6;
            fb_console_set_cursor(col, row);
            fb_console_puts(items[i]->name);
            col += (uint32_t)strlen(items[i]->name);
            fb_console_set_cursor(col, row);
            fb_console_puts("  (");
            col += 3;
            uint64_t mib = (items[i]->sector_count * 512ULL) / (1024ULL * 1024ULL);
            tui_put_u64_at(col, row, mib);
            fb_console_puts(" MiB)");
        }

        tui_button(bx + 10, by + 5 + count + 1, " OK ",     1);
        tui_button(bx + 22, by + 5 + count + 1, " Cancel ", 0);

        tui_status_bar("[Up/Down] Select disk  [Enter] Confirm  [Q] Cancel", "");

        int key = read_menu_key();
        if (key == 1) { if (sel > 0) sel--; }
        else if (key == 2) { if (sel < count - 1) sel++; }
        else if (key == 3) {
            if (sel >= 0 && sel < count) {
                *out = items[sel];
                return 1;
            }
        }
        else if (key == 4) return 0;
    }
}

/* ------------------------------------------------------------------ */
/* Screen 3: Hostname                                                   */
/* ------------------------------------------------------------------ */

void tui_screen_hostname(char *hostname_buf, size_t buf_sz) {
    serial_port_puts("[installer] tui: hostname\n");
    tui_screen_bg();

    uint32_t bx, by;
    tui_dialog(54, 10, " Hostname Configuration ", &bx, &by);

    tui_text(bx, by, 3, 2, "Enter a hostname for this machine.");
    tui_text(bx, by, 3, 3, "Use letters, digits and hyphens only.");
    tui_text(bx, by, 3, 4, "Press Enter to keep default 'asd'.");
    /* FIX Bug 2: label and field on same row (dy=5).
     * Cursor is set AFTER tui_status_bar() to prevent status bar
     * from moving the cursor away from the input field. */
    tui_text(bx, by, 3, 5, "Hostname: ");

    fb_console_set_colors(TUI_BTN_SEL_FG, TUI_BTN_SEL_BG);
    fb_console_set_cursor(bx + 13, by + 5);
    /* inline input field */
    for (uint32_t i = 0; i < 30; i++) fb_console_putc(' ');

    /* Draw status bar FIRST, then restore cursor to field */
    tui_status_bar("[Enter] Confirm  [Backspace] Erase", "");
    fb_console_set_colors(TUI_BTN_SEL_FG, TUI_BTN_SEL_BG);
    fb_console_set_cursor(bx + 13, by + 5);
    fb_console_tick(); /* Ensure cursor is drawn at correct position */

    char buf[64];
    tui_readline_field(bx + 13, by + 5, buf, sizeof(buf), 30, 0);
    serial_port_puts("[installer] tui: hostname done -> root password\n");
    if (buf[0]) {
        int ok = 1;
        for (int i = 0; buf[i]; i++) {
            char c = buf[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-')) { ok = 0; break; }
        }
        if (ok) {
            strncpy(hostname_buf, buf, buf_sz);
            hostname_buf[buf_sz - 1] = '\0';
        } else {
            tui_text(bx, by, 3, 8, "Invalid hostname — using 'asd'.");
            tui_text(bx, by, 3, 9, "Press Enter to continue.");
            char dummy[4];
            tui_readline_field(bx + 3, by + 10, dummy, sizeof(dummy), 1, 0);
        }
    }
}

void tui_screen_configure_accounts(void) {
    serial_port_puts("[installer] tui: configure accounts\n");
    flush_input();
    tui_screen_bg();

    uint32_t bx, by;
    tui_dialog(58, 12, " System Accounts ", &bx, &by);

    tui_text(bx, by, 3, 2, "Base system is installed on disk.");
    tui_text(bx, by, 3, 3, "Now set the root password and optional user.");
    tui_text(bx, by, 3, 4, "(just enter root password).");
    tui_text(bx, by, 3, 6, "Press Enter to continue.");

    tui_status_bar("[Enter] Continue", "");
    char dummy[4];
    tui_readline_field(bx + 3, by + 7, dummy, sizeof(dummy), 1, 0);
}

/* ------------------------------------------------------------------ */
/* Screen 4: Root password                                              */
/* ------------------------------------------------------------------ */

void tui_screen_root_pw(char *pw_buf, size_t buf_sz) {
    serial_port_puts("[installer] tui: root password\n");
    flush_input();
    tui_screen_bg();

    uint32_t bx, by;
    tui_dialog(54, 10, " Root Password ", &bx, &by);

    tui_text(bx, by, 3, 2, "Set a password for the root account.");
    tui_text(bx, by, 3, 3, "Press Enter for no password (not recommended).");
    /* FIX Bug 2: cursor set AFTER tui_status_bar */
    tui_text(bx, by, 3, 5, "Password: ");

    fb_console_set_colors(TUI_BTN_SEL_FG, TUI_BTN_SEL_BG);
    fb_console_set_cursor(bx + 13, by + 5);
    for (uint32_t i = 0; i < 30; i++) fb_console_putc(' ');

    tui_status_bar("[Enter] Confirm  (input is masked)", "");
    fb_console_set_colors(TUI_BTN_SEL_FG, TUI_BTN_SEL_BG);
    fb_console_set_cursor(bx + 13, by + 5);
    fb_console_tick();
    tui_readline_field(bx + 13, by + 5, pw_buf, buf_sz, 30, 1);
}

/* ------------------------------------------------------------------ */
/* Screen 5: New user                                                   */
/* ------------------------------------------------------------------ */

/* Returns 1 if user was configured, 0 if skipped */
int tui_screen_user(char *name_buf, size_t name_sz,
                    char *pw_buf,   size_t pw_sz,
                    int  *wheel_out) {
    tui_screen_bg();

    uint32_t bx, by;
    tui_dialog(56, 14, " Create User Account ", &bx, &by);

    tui_text(bx, by, 3, 2, "Create a regular user account (optional).");
    tui_text(bx, by, 3, 3, "Press Enter to skip.");
    /* FIX Bug 2: cursor set AFTER tui_status_bar */
    tui_text(bx, by, 3, 5, "Username  : ");

    fb_console_set_colors(TUI_BTN_SEL_FG, TUI_BTN_SEL_BG);
    fb_console_set_cursor(bx + 15, by + 5);
    for (uint32_t i = 0; i < 28; i++) fb_console_putc(' ');

    tui_status_bar("[Enter] Confirm  [Backspace] Erase", "");
    fb_console_set_colors(TUI_BTN_SEL_FG, TUI_BTN_SEL_BG);
    fb_console_set_cursor(bx + 15, by + 5);
    fb_console_tick();
    tui_readline_field(bx + 15, by + 5, name_buf, name_sz, 28, 0);
    if (!name_buf[0]) return 0;

    /* Validate username */
    for (int i = 0; name_buf[i]; i++) {
        char c = name_buf[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            tui_text(bx, by, 3, 12, "Invalid username — skipping user creation.");
            char dummy[4];
            tui_readline_field(bx + 3, by + 12, dummy, sizeof(dummy), 1, 0);
            name_buf[0] = '\0';
            return 0;
        }
    }

    tui_text(bx, by, 3, 7, "Password  : ");
    fb_console_set_colors(TUI_BTN_SEL_FG, TUI_BTN_SEL_BG);
    fb_console_set_cursor(bx + 15, by + 7);
    for (uint32_t i = 0; i < 28; i++) fb_console_putc(' ');
    tui_status_bar("[Enter] Confirm  (input is masked)", "");
    fb_console_set_colors(TUI_BTN_SEL_FG, TUI_BTN_SEL_BG);
    fb_console_set_cursor(bx + 15, by + 7);
    fb_console_tick();
    tui_readline_field(bx + 15, by + 7, pw_buf, pw_sz, 28, 1);

    tui_text(bx, by, 3, 9, "Add to wheel (admin) group? [y/N]: ");
    tui_status_bar("[y] Yes  [n] No  [Enter] No — one key, no Enter needed", "");
    fb_console_set_colors(TUI_BTN_SEL_FG, TUI_BTN_SEL_BG);
    fb_console_set_cursor(bx + 38, by + 9);
    fb_console_putc('N');
    fb_console_tick();
    flush_input();
    *wheel_out = 0;
    for (;;) {
        char ch = 0;
        while (!input_getc_nonblock(&ch)) {
            fb_console_tick();
            __asm__ volatile("pause");
        }
        if (ch == 'y' || ch == 'Y') {
            fb_console_set_cursor(bx + 38, by + 9);
            fb_console_putc('Y');
            *wheel_out = 1;
            flush_input();
            break;
        }
        if (ch == 'n' || ch == 'N' || ch == 0x1B ||
            ch == '\r' || ch == '\n') {
            flush_input();
            break;
        }
        /* Ignore key-repeat / stray bytes so we never spin on garbage. */
    }

    return 1;
}

/* ------------------------------------------------------------------ */
/* Screen 6: Confirmation                                               */
/* ------------------------------------------------------------------ */

/* Returns 1 = proceed, 0 = go back */
int tui_screen_confirm(const char *disk_name, const char *hostname) {
    int sel = 0;   /* 0=Install 1=Cancel */
    for (;;) {
        tui_screen_bg();

        uint32_t bx, by;
        tui_dialog(58, 14, " Confirm Installation ", &bx, &by);

        tui_text(bx, by, 3, 2, "Review your installation settings:");
        tui_text(bx, by, 3, 4, "Target disk : ");
        fb_console_set_colors(TUI_FG_YELLOW, TUI_DLG_BG);
        fb_console_set_cursor(bx + 17, by + 4);
        fb_console_puts(disk_name);

        fb_console_set_colors(TUI_DLG_FG, TUI_DLG_BG);
        tui_text(bx, by, 3, 5, "Hostname    : ");
        fb_console_set_colors(TUI_FG_YELLOW, TUI_DLG_BG);
        fb_console_set_cursor(bx + 17, by + 5);
        fb_console_puts(hostname);

        fb_console_set_colors(TUI_DLG_FG, TUI_DLG_BG);
        tui_text(bx, by, 3, 6, "Accounts    : after install (next step)");

        fb_console_set_colors(TUI_ERR_FG, TUI_ERR_BG);
        fb_console_set_cursor(bx + 3, by + 8);
        fb_console_puts("WARNING: ALL DATA ON THE TARGET DISK WILL BE");
        fb_console_set_cursor(bx + 3, by + 9);
        fb_console_puts("PERMANENTLY ERASED.  This cannot be undone.");

        tui_button(bx + 8,  by + 11, " Install Now ", sel == 0);
        tui_button(bx + 30, by + 11, "   Cancel    ", sel == 1);

        tui_status_bar("[Enter] Confirm  [Arrows] Navigate  [Q] Cancel", "");

        int key = read_menu_key();
        if (key == 1 || key == 2) sel ^= 1;
        else if (key == 3) return (sel == 0) ? 1 : 0;
        else if (key == 4) return 0;
    }
}

/* ------------------------------------------------------------------ */
/* Screen 7: Progress                                                   */
/* ------------------------------------------------------------------ */

/*
 * Called by installer.c to update the progress display.
 * step:  0=GPT  1=copy  2=layout  3=accounts
 * done/total: sectors copied (step 2 only; ignored for 0,1)
 */
void tui_screen_progress(int step, uint64_t done, uint64_t total) {
    /* FIX Bug 1: cache bx/by as static — only call tui_dialog when step
     * changes so the dialog title is never redrawn (duplicated) on
     * every progress tick. */
    static int      last_step = -1;
    static int      last_pct  = -1;
    static uint64_t last_done = 0;
    static uint32_t s_bx = 0, s_by = 0;
    int current_pct = (total > 0) ? (int)((done * 100ULL) / total) : 0;
    if (step != last_step) {
        last_done = 0;
        /* Clear screen before each major step to avoid text duplication */
        fb_console_set_colors(TUI_FG_WHITE, TUI_BG_BLUE);
        fb_console_clear();
        tui_screen_bg();
        tui_dialog(58, 14, " Installing OpenASD ", &s_bx, &s_by);
        last_step = step;
        last_pct = -1; /* force redraw bar */
    } else if (step == 1 && current_pct == last_pct && done == last_done) {
        return; /* skip redraw if nothing changed */
    }
    last_done = done;
    uint32_t bx = s_bx, by = s_by;
    last_pct = current_pct;

    /* Step labels */
    static const char *labels[] = {
        "[1/4] Writing GPT partition table ...",
        "[2/4] Copying boot files to ESP ...",
        "[3/4] Preparing directory layout ...",
        "[4/4] Configuring user accounts ..."
    };

    int done_all = (step >= 4);
    for (int i = 0; i < 4; i++) {
        int finished = done_all || i < step;
        int current  = !done_all && i == step;
        fb_console_set_colors(finished ? TUI_FG_YELLOW : TUI_DLG_FG, TUI_DLG_BG);
        fb_console_set_cursor(bx + 3, by + 3 + (uint32_t)i * 2);
        fb_console_puts(finished ? "[DONE] " :
                        current  ? "[    ] " : "[    ] ");
        fb_console_set_colors(TUI_DLG_FG, TUI_DLG_BG);
        fb_console_puts(labels[i]);
    }

    if (step == 1 && total > 0) {
        uint64_t pct = (done * 100ULL) / total;
        tui_progress_bar(bx + 3, by + 10, 48, pct);
    }

    tui_status_bar("Installing — please wait ...", "");
}

/* ------------------------------------------------------------------ */
/* Screen 8: Success / Error                                            */
/* ------------------------------------------------------------------ */

void tui_screen_done(int success) {
    tui_screen_bg();

    uint32_t bx, by;
    if (success) {
        tui_dialog(54, 10, " Installation Complete ", &bx, &by);
        tui_text(bx, by, 3, 2, "OpenASD has been successfully installed.");
        tui_text(bx, by, 3, 3, "User accounts are saved on the target disk.");
        tui_text(bx, by, 3, 4, "Remove installation media, then reboot.");
        tui_text(bx, by, 3, 6, "Press Enter to reboot, or Q to drop to shell.");
    } else {
        fb_console_set_colors(TUI_ERR_FG, TUI_ERR_BG);
        tui_dialog(54, 10, " Installation Failed ", &bx, &by);
        fb_console_set_colors(TUI_ERR_FG, TUI_ERR_BG);
        tui_text(bx, by, 3, 2, "Installation encountered an error.");
        tui_text(bx, by, 3, 3, "Check serial output for details.");
        tui_text(bx, by, 3, 5, "Press Enter to reboot, or Q to drop to shell.");
    }
    tui_status_bar("[Enter] Reboot  [Q] Shell", "");
}

/* ------------------------------------------------------------------ */
/* Main TUI entry point — called from installer_run()                   */
/* ------------------------------------------------------------------ */

/*
 * Returns:
 *   1  = user confirmed install (disk/hostname/user already set in globals)
 *   0  = user chose live shell
 *  -1  = user quit
 */
int installer_run_tui(void) {
    /* Flush any stale bytes left by UEFI/serial before reading user input. */
    flush_input();
    /* Screen 1: Welcome */
    int choice = tui_screen_welcome();
    return choice;   /* 1=install, 0=shell, -1=quit */
}
