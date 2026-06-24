/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, ASD Project Contributors
 *
 * Serial I/O, logging, readline, and input routing for init.
 */

#include "init_internal.h"
#include "../kernel/drv/ps2kbd.h"

/* ------------------------------------------------------------------ */
/* Output                                                               */
/* ------------------------------------------------------------------ */

void serial_port_putc(char c) {
    for (uint32_t i = 0; i < 1000000; i++) {
        if ((io_in8(COM1_PORT + 5) & 0x20) != 0) break;
        __asm__ volatile("pause");
    }
    io_out8(COM1_PORT, (uint8_t)c);
}

void serial_port_puts(const char *s) {
    if (!s) return;
    while (*s) serial_port_putc(*s++);
}

void serial_putc(char c) {
    serial_port_putc(c);
    fb_console_putc(c);
}

void serial_puts(const char *s) {
    if (!s) return;
    while (*s) serial_putc(*s++);
}

void put_u64(uint64_t v) {
    char buf[32];
    int i = 31;
    buf[i] = '\0';
    if (v == 0) { buf[--i] = '0'; }
    while (v) { buf[--i] = (char)('0' + (v % 10)); v /= 10; }
    serial_puts(&buf[i]);
}

void put_repeat(char c, int n) {
    while (n-- > 0) serial_putc(c);
}

static size_t u64_len(uint64_t v) {
    if (v == 0) return 1;
    size_t n = 0;
    while (v) { n++; v /= 10; }
    return n;
}

void box_right_pad(size_t used, size_t inner_w) {
    while (used < inner_w) { serial_putc(' '); used++; }
    serial_puts("|\n");
}

/* ------------------------------------------------------------------ */
/* Logging                                                              */
/* ------------------------------------------------------------------ */

void log_msg(const char *msg) {
    serial_port_puts("[asdinit] ");
    serial_port_puts(msg);
    serial_port_puts("\n");
}

void log_svc(const char *svc, const char *msg) {
    serial_port_puts("[asdinit] ");
    serial_port_puts(svc);
    serial_port_puts(": ");
    serial_port_puts(msg);
    serial_port_puts("\n");
}

void boot_log(const char *status, const char *msg) {
    serial_port_puts("[");
    serial_port_puts(status);
    serial_port_puts("] ");
    serial_port_puts(msg);
    serial_port_puts("\n");
}

void print_shell_banner(void) {
    serial_puts("\nASD/amd64 (asdtty0)\n");
    serial_puts("login: root (auto)\n\n");
}

/* ------------------------------------------------------------------ */
/* Input                                                                */
/* ------------------------------------------------------------------ */

static int  g_input_unget_valid;
static char g_input_unget_ch;

void input_unget(char c) {
    g_input_unget_valid = 1;
    g_input_unget_ch = c;
}

int serial_getc_nonblock(char *out) {
    uint8_t lsr = io_in8(COM1_PORT + 5);
    if ((lsr & 0x01) == 0)
        return 0;
    /* Discard break/framing/parity garbage instead of feeding the TUI. */
    if (lsr & 0x1E) {
        (void)io_in8(COM1_PORT);
        return 0;
    }
    uint8_t b = io_in8(COM1_PORT);
    if (b == 0)
        return 0;
    *out = (char)b;
    return 1;
}

int kbd_getc_nonblock(char *out) {
    return ps2kbd_getc(out);
}

/* Keyboard before serial so installer fields work when COM1 has noise. */
int input_getc_nonblock(char *out) {
    if (g_input_unget_valid) {
        g_input_unget_valid = 0;
        *out = g_input_unget_ch;
        return 1;
    }
    if (kbd_getc_nonblock(out))
        return 1;
    return serial_getc_nonblock(out);
}

void flush_input(void) {
    char dummy;
    g_input_unget_valid = 0;
    /* Never spin forever — some UARTs always report data ready (hangs
     * the installer after Enter on the hostname screen). */
    for (int n = 0; n < 4096; n++) {
        int got = 0;
        if (kbd_getc_nonblock(&dummy))    got = 1;
        if (serial_getc_nonblock(&dummy)) got = 1;
        if (!got) break;
    }
}

static void input_skip_optional_lf_after_cr(void) {
    char next;
    if (input_getc_nonblock(&next) && next != '\n')
        input_unget(next);
}

static int input_wait_key(void) {
    char ch;
    for (;;) {
        while (!input_getc_nonblock(&ch)) {
            fb_console_tick();
            __asm__ volatile("pause");
        }
        return (unsigned char)ch;
    }
}

char read_char(void) {
    for (;;) {
        char ch = (char)input_wait_key();
        if (ch >= 0x20 && ch <= 0x7e) return ch;
    }
}

int read_menu_key(void) {
    for (;;) {
        char ch = (char)input_wait_key();
        if ((uint8_t)ch == 0x80) return 1; /* up   */
        if ((uint8_t)ch == 0x81) return 2; /* down */
        if (ch == '\r' || ch == '\n') return 3; /* enter */
        if (ch == 'q' || ch == 'Q' || ch == 0x1B) return 4; /* quit/esc */
    }
}

/* ------------------------------------------------------------------ */
/* Readline with history, cursor movement, and tab completion           */
/* ------------------------------------------------------------------ */

#define HIST_CAP 32

static char     g_hist[HIST_CAP][256];
static uint32_t g_hist_count;
static uint32_t g_hist_head;

static void hist_add(const char *line) {
    if (!line || !line[0]) return;
    uint32_t idx = (g_hist_head + g_hist_count) % HIST_CAP;
    if (g_hist_count == HIST_CAP) {
        g_hist_head = (g_hist_head + 1) % HIST_CAP;
        idx = (g_hist_head + g_hist_count - 1) % HIST_CAP;
    } else {
        g_hist_count++;
    }
    strncpy(g_hist[idx], line, sizeof(g_hist[idx]));
    g_hist[idx][sizeof(g_hist[idx]) - 1] = '\0';
}

static const char *hist_get(uint32_t rel) {
    if (rel >= g_hist_count) return NULL;
    return g_hist[(g_hist_head + rel) % HIST_CAP];
}

/* Move screen cursor left by n (no character erasure) */
static void cursor_move_left(uint32_t n) {
    while (n--) serial_puts("\x1b[D");
}

/* Move screen cursor right by n (no character write — cursor only) */
static void cursor_move_right(uint32_t n) {
    while (n--) serial_puts("\x1b[C");
}

/* Erase n chars leftward from current cursor (classic terminal BS SPC BS) */
static void line_erase(uint32_t n) {
    while (n--) serial_puts("\b \b");
}

/* Replace current line with src.  Cursor must be at position *curp. */
static void line_replace_cur(char *buf, uint32_t *lenp, uint32_t *curp,
                              uint32_t cap, const char *src) {
    /* Move cursor to end of current content */
    cursor_move_right(*lenp - *curp);
    /* Erase all current content */
    line_erase(*lenp);
    /* Write new content */
    uint32_t i = 0;
    while (src && src[i] && i < cap - 1) {
        buf[i] = src[i];
        serial_putc(src[i]);
        i++;
    }
    buf[i] = '\0';
    *lenp = i;
    *curp = i;
}

/* ------------------------------------------------------------------ */
/* Tab completion                                                        */
/* ------------------------------------------------------------------ */

static void tab_complete(char *buf, uint32_t *lenp, uint32_t *curp,
                         uint32_t cap) {
    uint32_t cur = *curp;
    uint32_t len = *lenp;

    /* Find start of current word (last space before cursor) */
    uint32_t word_start = 0;
    for (uint32_t i = 0; i < cur; i++)
        if (buf[i] == ' ') word_start = i + 1;

    uint32_t prefix_len = cur - word_start;
    if (prefix_len >= VFS_PATH_MAX) return;

    char prefix[VFS_PATH_MAX];
    for (uint32_t i = 0; i < prefix_len; i++) prefix[i] = buf[word_start + i];
    prefix[prefix_len] = '\0';

    /* Find last '/' to split directory from file-prefix */
    int slash = -1;
    for (int i = (int)prefix_len - 1; i >= 0; i--)
        if (prefix[i] == '/') { slash = i; break; }

    int is_command = (word_start == 0 && slash < 0);

    char dir[VFS_PATH_MAX];
    char fpfx[VFS_PATH_MAX];

    if (slash < 0) {
        strncpy(dir, is_command ? "/bin" : g_cwd, VFS_PATH_MAX);
        dir[VFS_PATH_MAX - 1] = '\0';
        strncpy(fpfx, prefix, VFS_PATH_MAX);
        fpfx[VFS_PATH_MAX - 1] = '\0';
    } else {
        if (slash == 0) {
            dir[0] = '/'; dir[1] = '\0';
        } else {
            int n = slash < VFS_PATH_MAX - 1 ? slash : VFS_PATH_MAX - 2;
            for (int i = 0; i < n; i++) dir[i] = prefix[i];
            dir[n] = '\0';
        }
        int fp_start = slash + 1;
        int fp_len   = (int)prefix_len - fp_start;
        if (fp_len < 0) fp_len = 0;
        for (int i = 0; i < fp_len && i < VFS_PATH_MAX - 1; i++)
            fpfx[i] = prefix[fp_start + i];
        fpfx[fp_len] = '\0';
    }

    /* Enumerate directory */
    static vfs_dirent_t ents[128];
    uint32_t n = 0;
    if (vfs_readdir(dir, ents, 128, &n) != 0) return;

    /* Collect matches */
    char matches[32][VFS_PATH_MAX];
    int nmatch = 0;
    int fpfx_len = (int)strlen(fpfx);

    for (uint32_t i = 0; i < n && nmatch < 32; i++) {
        int elen = (int)strlen(ents[i].name);
        if (elen < fpfx_len) continue;
        int ok = 1;
        for (int j = 0; j < fpfx_len; j++)
            if (ents[i].name[j] != fpfx[j]) { ok = 0; break; }
        if (!ok) continue;
        strncpy(matches[nmatch], ents[i].name, VFS_PATH_MAX - 2);
        matches[nmatch][VFS_PATH_MAX - 2] = '\0';
        if (ents[i].kind == VFS_NODE_DIR) {
            int ml = (int)strlen(matches[nmatch]);
            matches[nmatch][ml]   = '/';
            matches[nmatch][ml+1] = '\0';
        }
        nmatch++;
    }

    if (nmatch == 0) return;

    if (nmatch == 1) {
        /* Insert the remainder of the single match at cursor */
        const char *rest     = matches[0] + fpfx_len;
        uint32_t    rest_len = (uint32_t)strlen(rest);
        if (rest_len == 0) return;
        if (len + rest_len >= cap) return;

        /* Shift buf[cur..len] right by rest_len */
        for (int k = (int)len; k >= (int)cur; k--)
            buf[k + rest_len] = buf[k];
        for (uint32_t k = 0; k < rest_len; k++)
            buf[cur + k] = rest[k];
        len += rest_len;
        buf[len] = '\0';

        /* Redraw from current screen position (= old cur) to end */
        for (uint32_t k = cur; k < len; k++) serial_putc(buf[k]);
        cur += rest_len;

        /* Move cursor back to new cur position */
        cursor_move_left(len - cur);
        *lenp = len;
        *curp = cur;
    } else {
        /* Show all matches then reprint the partial line */
        serial_puts("\n");
        for (int i = 0; i < nmatch; i++) {
            serial_puts(matches[i]);
            serial_putc(' ');
            serial_putc(' ');
        }
        serial_puts("\n");
        /* Reprint current line content; cursor ends at cur position */
        for (uint32_t k = 0; k < len; k++) serial_putc(buf[k]);
        cursor_move_left(len - cur);
    }
}

char *readline_serial(char *buf, uint32_t cap) {
    if (!buf || cap < 2) return NULL;
    uint32_t len = 0;   /* buffer length */
    uint32_t cur = 0;   /* logical cursor position [0..len] */
    int hist_cursor = -1;

    for (;;) {
        char ch = (char)input_wait_key();
        uint8_t uch = (uint8_t)ch;

        /* ── History navigation ── */
        if (uch == 0x80) { /* up arrow — history back */
            if (g_hist_count == 0) continue;
            if (hist_cursor < 0) hist_cursor = (int)g_hist_count - 1;
            else if (hist_cursor > 0) hist_cursor--;
            const char *h = hist_get((uint32_t)hist_cursor);
            if (h) line_replace_cur(buf, &len, &cur, cap, h);
            continue;
        }
        if (uch == 0x81) { /* down arrow — history forward */
            if (g_hist_count == 0 || hist_cursor < 0) continue;
            hist_cursor++;
            if (hist_cursor >= (int)g_hist_count) {
                hist_cursor = -1;
                line_replace_cur(buf, &len, &cur, cap, "");
            } else {
                const char *h = hist_get((uint32_t)hist_cursor);
                if (h) line_replace_cur(buf, &len, &cur, cap, h);
            }
            continue;
        }

        /* ── Cursor movement ── */
        if (uch == 0x82) { /* left arrow */
            if (cur > 0) { cur--; cursor_move_left(1); }
            continue;
        }
        if (uch == 0x83) { /* right arrow */
            if (cur < len) { cur++; cursor_move_right(1); }
            continue;
        }

        /* ── Tab completion ── */
        if (ch == '\t') {
            tab_complete(buf, &len, &cur, cap);
            continue;
        }

        /* ── Enter ── */
        if (ch == '\r' || ch == '\n') {
            if (ch == '\r')
                input_skip_optional_lf_after_cr();
            serial_puts("\n");
            buf[len] = '\0';
            hist_add(buf);
            return buf;
        }

        /* ── Backspace / Delete ── */
        if ((ch == '\b' || ch == 0x7f) && cur > 0) {
            cur--;
            /* Shift buffer left, removing buf[cur] */
            for (uint32_t k = cur; k < len - 1; k++) buf[k] = buf[k + 1];
            len--;
            buf[len] = '\0';
            /* Move cursor back one without erasing (then we redraw) */
            cursor_move_left(1);
            /* Reprint buf[cur..len-1] and one blank for the removed char */
            for (uint32_t k = cur; k < len; k++) serial_putc(buf[k]);
            serial_putc(' ');
            /* Cursor is now len-cur+1 positions past cur; return to cur */
            cursor_move_left(len - cur + 1);
            continue;
        }

        /* ── Printable character — insert at cursor ── */
        if (len < cap - 1 && ch >= 0x20 && ch <= 0x7e) {
            /* Shift buf[cur..len] right by 1 */
            for (uint32_t k = len; k > cur; k--) buf[k] = buf[k - 1];
            buf[cur] = ch;
            len++;
            buf[len] = '\0';
            /* Print from old cursor position to end of buffer */
            for (uint32_t k = cur; k < len; k++) serial_putc(buf[k]);
            cur++;
            /* Move cursor back to new cur position */
            cursor_move_left(len - cur);
        }
    }
}

/* Echoes '*' per character; backspace erases. */
char *readline_serial_noecho(char *buf, uint32_t cap) {
    if (!buf || cap < 2) return NULL;
    uint32_t i = 0;
    for (;;) {
        char c = (char)input_wait_key();
        if (c == '\r' || c == '\n') {
            if (c == '\r')
                input_skip_optional_lf_after_cr();
            serial_putc('\n');
            break;
        }
        if ((c == '\b' || c == 127) && i > 0) {
            i--;
            serial_putc('\b'); serial_putc(' '); serial_putc('\b');
            continue;
        }
        if (c < 0x20) continue;
        if (i < cap - 1) { buf[i++] = c; serial_putc('*'); }
    }
    buf[i] = '\0';
    return buf;
}
