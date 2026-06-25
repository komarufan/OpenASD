#include "fbcon.h"
#include "font_bsd_vgarom_8x16.h"
#include <stdint.h>
#include <stddef.h>

static uint32_t *g_fb;
static uint32_t g_w, g_h, g_stride;
static uint8_t g_fmt;
static uint32_t g_cols, g_rows;
static uint32_t g_cx, g_cy;
static int g_ready;
enum { CELL_W = 8, CELL_H = 16 };
enum { MAX_TEXT_CELLS = 32768 };
static char g_cells[MAX_TEXT_CELLS];
static uint32_t g_cell_cap;
static uint8_t g_cursor_on;
static uint64_t g_cursor_last_tsc;
static uint64_t g_cursor_period_tsc = 1125000000ULL;
static uint32_t g_fg = 0xC0C0C0, g_bg = 0x000000;

/* ANSI state machine */
enum {
    ANSI_STATE_NORMAL,
    ANSI_STATE_ESC,
    ANSI_STATE_CSI,
    ANSI_STATE_DEC  /* ESC[? — DEC private, ignore until letter */
};
static int g_ansi_state = ANSI_STATE_NORMAL;
#define ANSI_MAX_PARAMS 8
static int g_ansi_params[ANSI_MAX_PARAMS];
static int g_ansi_num_params;

static char up(char c) {
    if (c >= 'a' && c <= 'z') return (char)(c - 'a' + 'A');
    return c;
}

static const uint8_t *glyph5x7(char ch) {
    static const uint8_t g_space[7] = {0,0,0,0,0,0,0};
    static const uint8_t g_qmark[7] = {14,17,16,12,4,0,4};
    static const uint8_t g_dot[7] = {0,0,0,0,0,12,12};
    static const uint8_t g_comma[7] = {0,0,0,0,0,12,12};
    static const uint8_t g_colon[7] = {0,12,12,0,12,12,0};
    static const uint8_t g_minus[7] = {0,0,0,31,0,0,0};
    static const uint8_t g_under[7] = {0,0,0,0,0,0,31};
    static const uint8_t g_slash[7] = {16,8,4,2,1,0,0};
    static const uint8_t g_bslash[7] = {1,2,4,8,16,0,0};
    static const uint8_t g_lbr[7] = {14,2,2,2,2,2,14};
    static const uint8_t g_rbr[7] = {14,8,8,8,8,8,14};
    static const uint8_t g_lpar[7] = {8,4,2,2,2,4,8};
    static const uint8_t g_rpar[7] = {2,4,8,8,8,4,2};
    static const uint8_t g_hash[7] = {10,10,31,10,31,10,10};
    static const uint8_t g_at[7] = {14,17,23,21,23,1,14};
    static const uint8_t g_eq[7] = {0,31,0,31,0,0,0};
    static const uint8_t g_plus[7] = {0,4,4,31,4,4,0};
    static const uint8_t g_bang[7] = {4,4,4,4,4,0,4};
    static const uint8_t g_star[7] = {0,21,14,31,14,21,0};
    static const uint8_t g_quote[7] = {10,10,0,0,0,0,0};
    static const uint8_t g_dquote[7] = {17,17,0,0,0,0,0};

    static const uint8_t g_0[7] = {14,17,19,21,25,17,14};
    static const uint8_t g_1[7] = {4,12,4,4,4,4,14};
    static const uint8_t g_2[7] = {14,17,16,8,4,2,31};
    static const uint8_t g_3[7] = {14,17,16,12,16,17,14};
    static const uint8_t g_4[7] = {8,12,10,9,31,8,8};
    static const uint8_t g_5[7] = {31,1,15,16,16,17,14};
    static const uint8_t g_6[7] = {14,1,1,15,17,17,14};
    static const uint8_t g_7[7] = {31,16,8,4,2,2,2};
    static const uint8_t g_8[7] = {14,17,17,14,17,17,14};
    static const uint8_t g_9[7] = {14,17,17,30,16,16,14};

    static const uint8_t g_A[7] = {14,17,17,31,17,17,17};
    static const uint8_t g_B[7] = {15,17,17,15,17,17,15};
    static const uint8_t g_C[7] = {14,17,1,1,1,17,14};
    static const uint8_t g_D[7] = {7,9,17,17,17,9,7};
    static const uint8_t g_E[7] = {31,1,1,15,1,1,31};
    static const uint8_t g_F[7] = {31,1,1,15,1,1,1};
    static const uint8_t g_G[7] = {14,17,1,29,17,17,14};
    static const uint8_t g_H[7] = {17,17,17,31,17,17,17};
    static const uint8_t g_I[7] = {14,4,4,4,4,4,14};
    static const uint8_t g_J[7] = {28,8,8,8,8,9,6};
    static const uint8_t g_K[7] = {17,9,5,3,5,9,17};
    static const uint8_t g_L[7] = {1,1,1,1,1,1,31};
    static const uint8_t g_M[7] = {17,27,21,21,17,17,17};
    static const uint8_t g_N[7] = {17,19,21,25,17,17,17};
    static const uint8_t g_O[7] = {14,17,17,17,17,17,14};
    static const uint8_t g_P[7] = {15,17,17,15,1,1,1};
    static const uint8_t g_Q[7] = {14,17,17,17,21,9,22};
    static const uint8_t g_R[7] = {15,17,17,15,5,9,17};
    static const uint8_t g_S[7] = {14,17,1,14,16,17,14};
    static const uint8_t g_T[7] = {31,4,4,4,4,4,4};
    static const uint8_t g_U[7] = {17,17,17,17,17,17,14};
    static const uint8_t g_V[7] = {17,17,17,17,17,10,4};
    static const uint8_t g_W[7] = {17,17,17,21,21,21,10};
    static const uint8_t g_X[7] = {17,17,10,4,10,17,17};
    static const uint8_t g_Y[7] = {17,17,10,4,4,4,4};
    static const uint8_t g_Z[7] = {31,16,8,4,2,1,31};

    char c = up(ch);
    if (c >= '0' && c <= '9') {
        static const uint8_t *digits[10] = {g_0,g_1,g_2,g_3,g_4,g_5,g_6,g_7,g_8,g_9};
        return digits[c - '0'];
    }
    if (c >= 'A' && c <= 'Z') {
        static const uint8_t *letters[26] = {
            g_A,g_B,g_C,g_D,g_E,g_F,g_G,g_H,g_I,g_J,g_K,g_L,g_M,
            g_N,g_O,g_P,g_Q,g_R,g_S,g_T,g_U,g_V,g_W,g_X,g_Y,g_Z
        };
        return letters[c - 'A'];
    }
    switch (c) {
    case ' ': return g_space;
    case '.': return g_dot;
    case ',': return g_comma;
    case ':': return g_colon;
    case '-': return g_minus;
    case '_': return g_under;
    case '/': return g_slash;
    case '\\': return g_bslash;
    case '[': return g_lbr;
    case ']': return g_rbr;
    case '(': return g_lpar;
    case ')': return g_rpar;
    case '#': return g_hash;
    case '@': return g_at;
    case '=': return g_eq;
    case '+': return g_plus;
    case '!': return g_bang;
    case '*': return g_star;
    case '\'': return g_quote;
    case '"': return g_dquote;
    default: return g_qmark;
    }
}

static inline uint32_t mk_rgb(uint8_t r, uint8_t g, uint8_t b) {
    if (g_fmt == ASD_FB_BGRX32) return ((uint32_t)b << 16) | ((uint32_t)g << 8) | (uint32_t)r;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static void plot(uint32_t x, uint32_t y, uint32_t c) {
    if (x >= g_w || y >= g_h) return;
    g_fb[(uint64_t)y * g_stride + x] = c;
}

static inline uint64_t rdtsc_now(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static void clear_row_pixels(uint32_t py0, uint32_t py1, uint32_t color) {
    if (py1 > g_h) py1 = g_h;
    for (uint32_t y = py0; y < py1; y++) {
        uint32_t *row = g_fb + (uint64_t)y * g_stride;
        uint32_t x = 0;
        for (; x + 4 <= g_w; x += 4) {
            row[x]     = color;
            row[x + 1] = color;
            row[x + 2] = color;
            row[x + 3] = color;
        }
        for (; x < g_w; x++)
            row[x] = color;
    }
}

static void scroll_if_needed(void) {
    /* Only scroll if we are actually beyond the last row */
    if (g_cy < g_rows) return;
    uint32_t line_h = CELL_H;
    uint32_t bg = g_bg;
    for (uint32_t y = 0; y + line_h < g_h; y++) {
        uint32_t *dst = g_fb + (uint64_t)y * g_stride;
        uint32_t *src = g_fb + (uint64_t)(y + line_h) * g_stride;
        for (uint32_t x = 0; x < g_w; x++) dst[x] = src[x];
    }
    clear_row_pixels(g_h - line_h, g_h, bg);
    g_cy = g_rows - 1;

    if (g_cell_cap && g_rows > 0) {
        uint32_t cols = g_cols;
        uint32_t rows = g_rows;
        if (cols * rows > g_cell_cap) rows = g_cell_cap / (cols ? cols : 1);
        if (rows > 1 && cols > 0) {
            for (uint32_t y = 1; y < rows; y++) {
                for (uint32_t x = 0; x < cols; x++) {
                    g_cells[(y - 1) * cols + x] = g_cells[y * cols + x];
                }
            }
            for (uint32_t x = 0; x < cols; x++) g_cells[(rows - 1) * cols + x] = ' ';
        }
    }
}

static void draw_char(uint32_t cx, uint32_t cy, char ch, int invert) {
    const uint8_t *glyph = bsd_vgarom_glyph_8x16((unsigned char)ch);
    uint32_t fg = g_fg;
    uint32_t bg = g_bg;
    if (invert) {
        uint32_t t = fg;
        fg = bg;
        bg = t;
    }
    uint32_t px = cx * CELL_W;
    uint32_t py = cy * CELL_H;
    for (uint32_t y = 0; y < CELL_H; y++) {
        uint8_t bits = glyph[y];
        for (uint32_t x = 0; x < CELL_W; x++) {
            uint32_t color = (bits & (uint8_t)(0x80u >> x)) ? fg : bg;
            plot(px + x, py + y, color);
        }
    }
}

static void cursor_erase(void) {
    if (!g_ready || !g_cursor_on) return;
    if (g_cx >= g_cols || g_cy >= g_rows) { g_cursor_on = 0; return; }
    uint32_t idx = g_cy * g_cols + g_cx;
    if (idx >= g_cell_cap) { g_cursor_on = 0; return; }
    draw_char(g_cx, g_cy, g_cells[idx], 0);
    g_cursor_on = 0;
}

static void cursor_draw(void) {
    if (!g_ready) return;
    if (g_cx >= g_cols || g_cy >= g_rows) return;
    uint32_t idx = g_cy * g_cols + g_cx;
    if (idx >= g_cell_cap) return;
    draw_char(g_cx, g_cy, g_cells[idx], 1);
    g_cursor_on = 1;
}

void fb_console_init(const asd_bib_t *bib) {
    g_ready = 0;
    if (!bib) return;
    if (!bib->fb_phys || bib->fb_width == 0 || bib->fb_height == 0 || bib->fb_stride == 0) return;
    if (bib->fb_format != ASD_FB_RGBX32 && bib->fb_format != ASD_FB_BGRX32) return;
    g_fb = (uint32_t *)(uintptr_t)bib->fb_phys;
    g_w = bib->fb_width;
    g_h = bib->fb_height;
    g_stride = bib->fb_stride;
    g_fmt = bib->fb_format;
    g_cols = g_w / CELL_W;
    g_rows = g_h / CELL_H;
    g_cx = 0;
    g_cy = 0;
    g_cell_cap = g_cols * g_rows;
    if (g_cell_cap > MAX_TEXT_CELLS) g_cell_cap = MAX_TEXT_CELLS;
    for (uint32_t i = 0; i < g_cell_cap; i++) g_cells[i] = ' ';
    g_cursor_on = 0;
    g_cursor_last_tsc = rdtsc_now();
    clear_row_pixels(0, g_h, mk_rgb(0, 0, 0));
    g_ready = 1;
    cursor_draw();
}

void fb_console_clear(void) {
    if (!g_ready) return;
    cursor_erase();
    clear_row_pixels(0, g_h, g_bg);
    g_cx = 0;
    g_cy = 0;
    for (uint32_t i = 0; i < g_cell_cap; i++) g_cells[i] = ' ';
    g_ansi_state = ANSI_STATE_NORMAL;
    g_ansi_num_params = 0;
    g_ansi_params[0] = -1;
    g_cursor_last_tsc = rdtsc_now();
    cursor_draw();
}

static void handle_ansi_csi(char c) {
    if (c >= '0' && c <= '9') {
        if (g_ansi_num_params < ANSI_MAX_PARAMS) {
            if (g_ansi_params[g_ansi_num_params] == -1) g_ansi_params[g_ansi_num_params] = 0;
            g_ansi_params[g_ansi_num_params] = g_ansi_params[g_ansi_num_params] * 10 + (c - '0');
        }
        return;
    }
    if (c == ';') {
        if (g_ansi_num_params < ANSI_MAX_PARAMS) g_ansi_num_params++;
        if (g_ansi_num_params < ANSI_MAX_PARAMS) g_ansi_params[g_ansi_num_params] = -1;
        return;
    }

    /* Final character of the sequence */
    if (g_ansi_num_params < ANSI_MAX_PARAMS && g_ansi_params[g_ansi_num_params] != -1)
        g_ansi_num_params++;

    int p1 = (g_ansi_num_params > 0 && g_ansi_params[0] != -1) ? g_ansi_params[0] : 1;
    int p2 = (g_ansi_num_params > 1 && g_ansi_params[1] != -1) ? g_ansi_params[1] : 1;

    switch (c) {
    case 'H': /* Cursor Position */
    case 'f':
        {
            uint32_t row = (p1 > 0) ? (uint32_t)p1 - 1 : 0;
            uint32_t col = (p2 > 0) ? (uint32_t)p2 - 1 : 0;
            if (row >= g_rows) row = g_rows - 1;
            if (col >= g_cols) col = g_cols - 1;
            g_cx = col;
            g_cy = row;
        }
        break;
    case 'A': /* Cursor Up */
        if (g_cy >= (uint32_t)p1) g_cy -= (uint32_t)p1; else g_cy = 0;
        break;
    case 'B': /* Cursor Down */
        g_cy += (uint32_t)p1; if (g_cy >= g_rows) g_cy = g_rows - 1;
        break;
    case 'C': /* Cursor Forward */
        g_cx += (uint32_t)p1; if (g_cx >= g_cols) g_cx = g_cols - 1;
        break;
    case 'D': /* Cursor Backward */
        if (g_cx >= (uint32_t)p1) g_cx -= (uint32_t)p1; else g_cx = 0;
        break;
    case 'J': /* Erase in Display */
        if (p1 == 2) fb_console_clear();
        break;
    case 'K': /* Erase in Line — default param 0 (erase to EOL) */
        {
            int kp = (g_ansi_num_params > 0 && g_ansi_params[0] != -1) ? g_ansi_params[0] : 0;
            uint32_t start = (kp == 1) ? 0 : g_cx;
            uint32_t end = (kp == 0) ? g_cols : (g_cx + 1);
            if (kp == 2) { start = 0; end = g_cols; }
            for (uint32_t x = start; x < end; x++) {
                draw_char(x, g_cy, ' ', 0);
                if (g_cy * g_cols + x < g_cell_cap) g_cells[g_cy * g_cols + x] = ' ';
            }
        }
        break;
    case 'm': /* Select Graphic Rendition (Colors) */
        for (int i = 0; i < g_ansi_num_params; i++) {
            int m = g_ansi_params[i];
            if (m == 0) { g_fg = 0xEEEEEE; g_bg = 0x000000; }
            else if (m >= 30 && m <= 37) {
                static const uint32_t pal[] = {0x000000, 0xAA0000, 0x00AA00, 0xAA5500, 0x0000AA, 0xAA00AA, 0x00AAAA, 0xAAAAAA};
                g_fg = pal[m - 30];
            }
            else if (m >= 40 && m <= 47) {
                static const uint32_t pal[] = {0x000000, 0xAA0000, 0x00AA00, 0xAA5500, 0x0000AA, 0xAA00AA, 0x00AAAA, 0xAAAAAA};
                g_bg = pal[m - 40];
            }
            else if (m >= 90 && m <= 97) {
                static const uint32_t bpal[] = {0x555555, 0xFF5555, 0x55FF55, 0xFFFF55, 0x5555FF, 0xFF69B4, 0x55FFFF, 0xFFFFFF};
                g_fg = bpal[m - 90];
            }
            else if (m >= 100 && m <= 107) {
                static const uint32_t bpal[] = {0x555555, 0xFF5555, 0x55FF55, 0xFFFF55, 0x5555FF, 0xFF69B4, 0x55FFFF, 0xFFFFFF};
                g_bg = bpal[m - 100];
            }
        }
        break;
    }
    g_ansi_state = ANSI_STATE_NORMAL;
}

void fb_console_putc(char c) {
    if (!g_ready) return;
    cursor_erase();

    if (g_ansi_state == ANSI_STATE_ESC) {
        if (c == '[') {
            g_ansi_state = ANSI_STATE_CSI;
            g_ansi_num_params = 0;
            g_ansi_params[0] = -1;
        } else {
            g_ansi_state = ANSI_STATE_NORMAL;
        }
        goto done;
    } else if (g_ansi_state == ANSI_STATE_CSI) {
        if (c == '?') {
            /* DEC private mode (ESC[?25h etc) — ignore whole sequence */
            g_ansi_state = ANSI_STATE_DEC;
            goto done;
        }
        handle_ansi_csi(c);
        goto done;
    } else if (g_ansi_state == ANSI_STATE_DEC) {
        /* Consume until we hit the final letter */
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
            g_ansi_state = ANSI_STATE_NORMAL;
        goto done;
    }

    if (c == 0x1B) {
        g_ansi_state = ANSI_STATE_ESC;
        goto done;
    }

    if (c == '\r') {
        g_cx = 0;
    } else if (c == '\n') {
        g_cx = 0;
        g_cy++;
        scroll_if_needed();
    } else if (c == '\b') {
        if (g_cx > 0) g_cx--;
        draw_char(g_cx, g_cy, ' ', 0);
        if (g_cx < g_cols && g_cy < g_rows) {
            uint32_t idx = g_cy * g_cols + g_cx;
            if (idx < g_cell_cap) g_cells[idx] = ' ';
        }
    } else {
        draw_char(g_cx, g_cy, c, 0);
        if (g_cx < g_cols && g_cy < g_rows) {
            uint32_t idx = g_cy * g_cols + g_cx;
            if (idx < g_cell_cap) g_cells[idx] = c;
        }
        g_cx++;
        if (g_cx >= g_cols) {
            g_cx = 0;
            if (g_cy < g_rows - 1) {
                g_cy++;
            } else {
                scroll_if_needed();
            }
        }
    }

done:
    g_cursor_last_tsc = rdtsc_now();
    cursor_draw();
}

void fb_console_puts(const char *s) {
    if (!s) return;
    while (*s) fb_console_putc(*s++);
}

void fb_console_info(uint32_t *w, uint32_t *h, uint32_t *stride, uint32_t *fmt) {
    if (w) *w = g_w;
    if (h) *h = g_h;
    if (stride) *stride = g_stride;
    if (fmt) *fmt = g_fmt;
}

void fb_console_blit(const uint32_t *buf, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    if (!g_fb || x >= g_w || y >= g_h) return;
    if (x + w > g_w) w = g_w - x;
    if (y + h > g_h) h = g_h - y;

    for (uint32_t row = 0; row < h; row++) {
        uint32_t *dst = g_fb + (uint64_t)(y + row) * g_stride + x;
        const uint32_t *src = buf + (uint64_t)row * w;
        for (uint32_t col = 0; col < w; col++) {
            dst[col] = src[col];
        }
    }
}

void fb_console_tick(void) {
    if (!g_ready) return;
    uint64_t now = rdtsc_now();
    if (now - g_cursor_last_tsc < g_cursor_period_tsc) return;
    g_cursor_last_tsc = now;
    if (g_cursor_on) cursor_erase();
    else cursor_draw();
}

void fb_console_set_colors(uint32_t fg, uint32_t bg) {
    g_fg = fg;
    g_bg = bg;
}

void fb_console_get_size(uint32_t *cols, uint32_t *rows) {
    if (cols) *cols = g_cols;
    if (rows) *rows = g_rows;
}

void fb_console_set_cursor(uint32_t col, uint32_t row) {
    if (!g_ready) return;
    cursor_erase();
    if (g_cols > 0 && col >= g_cols) col = g_cols - 1;
    if (g_rows > 0 && row >= g_rows) row = g_rows - 1;
    g_cx = col;
    g_cy = row;
    cursor_draw();
}

void fb_console_draw_box(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const char *title) {
    /* Draw corners and edges using ASCII for now, can be improved with CP437 */
    for (uint32_t i = 0; i < w; i++) {
        draw_char(x + i, y, '-', 0);
        draw_char(x + i, y + h - 1, '-', 0);
    }
    for (uint32_t i = 0; i < h; i++) {
        draw_char(x, y + i, '|', 0);
        draw_char(x + w - 1, y + i, '|', 0);
    }
    draw_char(x, y, '+', 0);
    draw_char(x + w - 1, y, '+', 0);
    draw_char(x, y + h - 1, '+', 0);
    draw_char(x + w - 1, y + h - 1, '+', 0);

    if (title) {
        uint32_t len = 0;
        while (title[len]) len++;
        if (len < w - 2) {
            uint32_t tx = x + (w - len) / 2;
            for (uint32_t i = 0; i < len; i++) draw_char(tx + i, y, title[i], 0);
        }
    }
}
