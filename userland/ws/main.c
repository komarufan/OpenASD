#include <asd/syscall.h>
#include <asd/stdio.h>
#include <asd/string.h>
#include "../libgui/gui.h"
#include "../../kernel/console/font_bsd_vgarom_8x16.h"

#define MAX_WINDOWS 16
#define RESIZE_MARGIN 6
#define MIN_WIN_W 120
#define MIN_WIN_H 80
#define RESIZE_LEFT   0x1
#define RESIZE_RIGHT  0x2
#define RESIZE_BOTTOM 0x4

struct window {
    int32_t id;
    int32_t x, y, w, h;
    int32_t buf_w, buf_h;
    uint32_t bg_color;
    uint32_t *buffer;
    int client_port;
    char client_port_name[32];
    char title[32];
    int active;
    int no_decor;   /* WS_WIN_NODECOR: no title bar / border / close button */
    int minimized;
    int maximized;
    int32_t restore_x, restore_y, restore_w, restore_h;
};

struct window g_windows[MAX_WINDOWS];
int g_next_win_id = 1;

uint32_t *g_backbuffer = NULL;
uint32_t g_screen_w = 0;
uint32_t g_screen_h = 0;
uint32_t g_stride = 0;
uint32_t g_format = 0;

/* Desktop wallpaper: raw 1280x800 XRGB8888 pixels loaded at startup from
 * /boot/wallpaper.bin into a BSS buffer (NOT embedded in the binary — that
 * bloated ws to 4 MB, which choked the installer's FFS copy and the Mach-O
 * loader).  Falls back to a solid colour if the file is missing/short. */
#define WP_W 1280u
#define WP_H 800u
static uint32_t g_wallpaper[WP_W * WP_H];   /* 4 MB, zero-fill BSS (demand-paged) */
static int      g_have_wallpaper = 0;

static void load_wallpaper(void) {
    int fd = asd_open("/boot/wallpaper.bin", O_RDONLY);
    if (fd < 0) return;
    uint8_t *p = (uint8_t *)g_wallpaper;
    size_t want = sizeof(g_wallpaper), total = 0;
    while (total < want) {
        long r = asd_read(fd, p + total, want - total);
        if (r <= 0) break;
        total += (size_t)r;
    }
    asd_close(fd);
    if (total == want) g_have_wallpaper = 1;
}

/* Simple White/Black arrow cursor */
const char g_cursor_map[12][13] = {
    "B           ",
    "BB          ",
    "BWB         ",
    "BWWB        ",
    "BWWWB       ",
    "BWWWWB      ",
    "BWWWWWB     ",
    "BWWWWWWB    ",
    "BWWWWWWWB   ",
    "BWWWWWWWWB  ",
    "BWWWB       ",
    "BB BWB      "
};

static void draw_rect(uint32_t *buf, int32_t bw, int32_t bh, int32_t rx, int32_t ry, int32_t rw, int32_t rh, uint32_t color) {
    for (int32_t y = 0; y < rh; y++) {
        int32_t wy = ry + y;
        if (wy < 0 || wy >= bh) continue;
        for (int32_t x = 0; x < rw; x++) {
            int32_t wx = rx + x;
            if (wx < 0 || wx >= bw) continue;
            buf[wy * bw + wx] = color;
        }
    }
}

static void draw_text(uint32_t *buf, int32_t bw, int32_t bh, int32_t rx, int32_t ry, uint32_t color, const char *text) {
    int32_t cur_x = rx;
    while (*text) {
        unsigned char c = (unsigned char)*text++;
        const uint8_t *glyph = bsd_vgarom_glyph_8x16(c);
        for (int y = 0; y < 16; y++) {
            uint8_t row = glyph[y];
            int32_t wy = ry + y;
            if (wy < 0 || wy >= bh) continue;
            for (int x = 0; x < 8; x++) {
                int32_t wx = cur_x + x;
                if (wx < 0 || wx >= bw) continue;
                if (row & (1 << (7 - x))) {
                    buf[wy * bw + wx] = color;
                }
            }
        }
        cur_x += 8;
    }
}

static int resize_window(struct window *w, int32_t new_w, int32_t new_h) {
    if (!w || new_w <= 0 || new_h <= 0) return -1;

    if (new_w <= w->buf_w && new_h <= w->buf_h) {
        if (new_w > w->w) {
            for (int32_t y = 0; y < w->h && y < new_h; y++) {
                for (int32_t x = w->w; x < new_w; x++) {
                    w->buffer[y * w->buf_w + x] = w->bg_color;
                }
            }
        }
        if (new_h > w->h) {
            for (int32_t y = w->h; y < new_h; y++) {
                for (int32_t x = 0; x < new_w; x++) {
                    w->buffer[y * w->buf_w + x] = w->bg_color;
                }
            }
        }
        w->w = new_w;
        w->h = new_h;
        return 0;
    }

    int32_t alloc_w = new_w > w->buf_w ? new_w : w->buf_w;
    int32_t alloc_h = new_h > w->buf_h ? new_h : w->buf_h;
    size_t buf_sz = alloc_w * alloc_h * 4;
    uint32_t *new_buf = (uint32_t *)asd_mmap(NULL, buf_sz,
                                             PROT_READ | PROT_WRITE,
                                             MAP_PRIVATE | MAP_ANONYMOUS,
                                             -1, 0);
    if (new_buf == MAP_FAILED || new_buf == NULL) return -1;

    for (int32_t i = 0; i < alloc_w * alloc_h; i++) {
        new_buf[i] = w->bg_color;
    }

    if (w->buffer) {
        int32_t copy_w = w->w < new_w ? w->w : new_w;
        int32_t copy_h = w->h < new_h ? w->h : new_h;
        for (int32_t y = 0; y < copy_h; y++) {
            for (int32_t x = 0; x < copy_w; x++) {
                new_buf[y * alloc_w + x] = w->buffer[y * w->buf_w + x];
            }
        }
    }

    w->buffer = new_buf;
    w->buf_w = alloc_w;
    w->buf_h = alloc_h;
    w->w = new_w;
    w->h = new_h;
    return 0;
}

static int hit_resize_edges(struct window *w, int32_t sx, int32_t sy) {
    if (!w || w->no_decor || w->minimized || w->maximized) return 0;
    if (sx < w->x || sx >= w->x + w->w || sy < w->y || sy >= w->y + w->h) return 0;

    int edges = 0;
    if (sx < w->x + RESIZE_MARGIN) edges |= RESIZE_LEFT;
    if (sx >= w->x + w->w - RESIZE_MARGIN) edges |= RESIZE_RIGHT;
    if (sy >= w->y + w->h - RESIZE_MARGIN) edges |= RESIZE_BOTTOM;
    return edges;
}

int main(void) {
    /* Get Screen Info */
    if (asd_fb_info(&g_screen_w, &g_screen_h, &g_stride, &g_format) != 0) {
        printf("ws: failed to query framebuffer info\n");
        return 1;
    }

    /* Allocate Backbuffer */
    size_t fbsize = g_screen_w * g_screen_h * 4;
    g_backbuffer = (uint32_t *)asd_mmap(NULL, fbsize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g_backbuffer == MAP_FAILED) {
        printf("ws: failed to allocate backbuffer\n");
        return 1;
    }

    /* Create WS Port */
    int ws_port = asd_port_create(WS_PORT_NAME, 0);
    if (ws_port < 0) {
        printf("ws: failed to create ws_port\n");
        return 1;
    }

    memset(g_windows, 0, sizeof(g_windows));

    load_wallpaper();   /* /boot/wallpaper.bin -> g_wallpaper (solid fallback) */

    int32_t cursor_x = g_screen_w / 2;
    int32_t cursor_y = g_screen_h / 2;
    uint32_t prev_btn = 0;
    int drag_win_idx = -1;
    int32_t drag_offset_x = 0;
    int32_t drag_offset_y = 0;
    int resize_win_idx = -1;
    int resize_edges = 0;
    int32_t resize_start_x = 0;
    int32_t resize_start_y = 0;
    int32_t resize_orig_x = 0;
    int32_t resize_orig_y = 0;
    int32_t resize_orig_w = 0;
    int32_t resize_orig_h = 0;

    printf("ws: window server started successfully (%dx%d)\n", g_screen_w, g_screen_h);

    while (1) {
        /* 1. Handle Client Messages */
        ws_msg_t msg;
        while (asd_port_recv_nonblock(ws_port, &msg, sizeof(msg)) > 0) {
            if (msg.type == WS_MSG_CREATE_WINDOW) {
                int win_idx = -1;
                for (int i = 0; i < MAX_WINDOWS; i++) {
                    if (g_windows[i].id == 0) {
                        win_idx = i;
                        break;
                    }
                }
                if (win_idx != -1) {
                    struct window *w = &g_windows[win_idx];
                    w->id = g_next_win_id++;
                    w->x = msg.create.x;
                    w->y = msg.create.y;
                    w->w = msg.create.w;
                    w->h = msg.create.h;
                    w->buf_w = w->w;
                    w->buf_h = w->h;
                    w->bg_color = msg.create.bg_color;
                    w->no_decor = (msg.create.flags & 0x1) ? 1 : 0;  /* WS_WIN_NODECOR */
                    w->minimized = 0;
                    w->maximized = 0;
                    w->restore_x = w->x;
                    w->restore_y = w->y;
                    w->restore_w = w->w;
                    w->restore_h = w->h;
                    
                    /* Client buffer with 20px title bar space */
                    size_t buf_sz = w->w * w->h * 4;
                    w->buffer = (uint32_t *)asd_mmap(NULL, buf_sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

                    if (w->buffer == MAP_FAILED || w->buffer == NULL) {
                        /* Out of memory — refuse the window instead of leaving a
                         * dangling -1 buffer that the compositor would deref. */
                        printf("ws: mmap failed for window buffer (%dx%d)\n", w->w, w->h);
                        w->buffer = NULL;
                        w->id = 0;          /* free the slot */
                        continue;
                    }

                    /* Clear client area to bg_color */
                    for (int32_t i = 0; i < w->w * w->h; i++) {
                        w->buffer[i] = w->bg_color;
                    }

                    strncpy(w->client_port_name, msg.create.port_name, sizeof(w->client_port_name));
                    strncpy(w->title, msg.create.port_name, sizeof(w->title));
                    w->title[sizeof(w->title) - 1] = '\0';
                    w->client_port = asd_port_open(w->client_port_name);
                    
                    /* Set active */
                    for (int i = 0; i < MAX_WINDOWS; i++) g_windows[i].active = 0;
                    w->active = 1;

                    /* Send reply */
                    ws_msg_t reply;
                    memset(&reply, 0, sizeof(reply));
                    reply.type = WS_MSG_CREATE_WINDOW;
                    reply.win_id = w->id;
                    asd_port_send(w->client_port, &reply, sizeof(reply));
                }
            } else {
                /* Find target window */
                struct window *w = NULL;
                for (int i = 0; i < MAX_WINDOWS; i++) {
                    if (g_windows[i].id == msg.win_id) {
                        w = &g_windows[i];
                        break;
                    }
                }
                if (w) {
                    if (msg.type == WS_MSG_DESTROY_WINDOW) {
                        asd_port_close(w->client_port);
                        w->id = 0;
                    } else if (msg.type == WS_MSG_SET_TITLE) {
                        strncpy(w->title, msg.title.title, sizeof(w->title));
                        w->title[sizeof(w->title) - 1] = '\0';
                    } else if (msg.type == WS_MSG_DRAW_RECT) {
                        if (!w->minimized)
                            draw_rect(w->buffer, w->buf_w, w->buf_h, msg.rect.x, msg.rect.y, msg.rect.w, msg.rect.h, msg.rect.color);
                    } else if (msg.type == WS_MSG_DRAW_TEXT) {
                        if (!w->minimized)
                            draw_text(w->buffer, w->buf_w, w->buf_h, msg.text.x, msg.text.y, msg.text.color, msg.text.text);
                    }
                }
            }
        }

        /* 2. Poll Keyboard and forward to active window */
        char k = (char)asd_kbd_poll();
        if (k != 0) {
            for (int i = 0; i < MAX_WINDOWS; i++) {
                if (g_windows[i].id != 0 && g_windows[i].active) {
                    ws_msg_t evt;
                    memset(&evt, 0, sizeof(evt));
                    evt.type = WS_EVT_KEYPRESS;
                    evt.win_id = g_windows[i].id;
                    evt.key.ch = k;
                    asd_port_send(g_windows[i].client_port, &evt, sizeof(evt));
                    break;
                }
            }
        }

        /* 3. Poll Mouse — kernel exposes a free-running accumulated position;
         * move the cursor by its DELTA (1 unit = 1 pixel) and clamp to screen. */
        int32_t mx, my;
        uint32_t mbtn;
        asd_get_mouse(&mx, &my, &mbtn);

        static int32_t prev_mx = 0, prev_my = 0;
        static int     m_init  = 0;
        if (!m_init) { prev_mx = mx; prev_my = my; m_init = 1; }
        cursor_x += (mx - prev_mx);
        cursor_y += (my - prev_my);
        prev_mx = mx; prev_my = my;
        if (cursor_x < 0) cursor_x = 0;
        if (cursor_y < 0) cursor_y = 0;
        if (cursor_x >= (int32_t)g_screen_w) cursor_x = g_screen_w - 1;
        if (cursor_y >= (int32_t)g_screen_h) cursor_y = g_screen_h - 1;

        if (mbtn & 1) {
            if (prev_btn == 0) {
                /* Click Down: Hit testing from top (last index) to bottom */
                int hit_idx = -1;
                for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
                    struct window *w = &g_windows[i];
                    if (w->id != 0) {
                        /* Hit test.  Decorated windows include a 20px title bar
                         * above the client area; no-decor windows do not. */
                        int32_t top = w->no_decor ? w->y : (w->y - 20);
                        int32_t bottom = w->minimized ? w->y : (w->y + w->h);
                        if (cursor_x >= w->x && cursor_x < w->x + w->w &&
                            cursor_y >= top && cursor_y < bottom) {
                            hit_idx = i;
                            break;
                        }
                    }
                }
                if (hit_idx != -1) {
                    /* Focus window */
                    for (int i = 0; i < MAX_WINDOWS; i++) g_windows[i].active = 0;
                    g_windows[hit_idx].active = 1;
                    
                    struct window *w = &g_windows[hit_idx];
                    
                    /* Clicked in Title Bar? */
                    if (cursor_y < w->y) {
                        int32_t rel_x = cursor_x - w->x;
                        /* Window controls: [_] [O] [X] */
                        if (rel_x >= w->w - 20) {
                            asd_port_close(w->client_port);
                            w->id = 0;
                        } else if (rel_x >= w->w - 40) {
                            if (w->maximized) {
                                w->x = w->restore_x;
                                w->y = w->restore_y;
                                resize_window(w, w->restore_w, w->restore_h);
                                w->maximized = 0;
                            } else {
                                w->restore_x = w->x;
                                w->restore_y = w->y;
                                w->restore_w = w->w;
                                w->restore_h = w->h;
                                w->x = 10;
                                w->y = 30;
                                resize_window(w, (int32_t)g_screen_w - 20, (int32_t)g_screen_h - 50);
                                w->maximized = 1;
                                w->minimized = 0;
                            }
                        } else if (rel_x >= w->w - 60) {
                            w->minimized = !w->minimized;
                        } else {
                            /* Drag window start */
                            drag_win_idx = hit_idx;
                            drag_offset_x = cursor_x - w->x;
                            drag_offset_y = cursor_y - w->y;
                        }
                    } else {
                        int edges = hit_resize_edges(w, cursor_x, cursor_y);
                        if (edges) {
                            resize_win_idx = hit_idx;
                            resize_edges = edges;
                            resize_start_x = cursor_x;
                            resize_start_y = cursor_y;
                            resize_orig_x = w->x;
                            resize_orig_y = w->y;
                            resize_orig_w = w->w;
                            resize_orig_h = w->h;
                        } else {
                            /* Clicked in client area — forward click */
                            ws_msg_t evt;
                            memset(&evt, 0, sizeof(evt));
                            evt.type = WS_EVT_MOUSE_CLICK;
                            evt.win_id = w->id;
                            evt.mouse.x = cursor_x - w->x;
                            evt.mouse.y = cursor_y - w->y;
                            evt.mouse.btn = mbtn;
                            asd_port_send(w->client_port, &evt, sizeof(evt));
                        }
                    }
                }
            } else if (resize_win_idx != -1) {
                /* Resizing by client edges/bottom corners */
                struct window *w = &g_windows[resize_win_idx];
                int32_t dx = cursor_x - resize_start_x;
                int32_t dy = cursor_y - resize_start_y;
                int32_t new_x = resize_orig_x;
                int32_t new_y = resize_orig_y;
                int32_t new_w = resize_orig_w;
                int32_t new_h = resize_orig_h;

                if (resize_edges & RESIZE_LEFT) {
                    new_x = resize_orig_x + dx;
                    new_w = resize_orig_w - dx;
                    if (new_w < MIN_WIN_W) {
                        new_x -= (MIN_WIN_W - new_w);
                        new_w = MIN_WIN_W;
                    }
                }
                if (resize_edges & RESIZE_RIGHT) {
                    new_w = resize_orig_w + dx;
                    if (new_w < MIN_WIN_W) new_w = MIN_WIN_W;
                }
                if (resize_edges & RESIZE_BOTTOM) {
                    new_h = resize_orig_h + dy;
                    if (new_h < MIN_WIN_H) new_h = MIN_WIN_H;
                }

                w->x = new_x;
                w->y = new_y;
                resize_window(w, new_w, new_h);
            } else if (drag_win_idx != -1) {
                /* Dragging */
                g_windows[drag_win_idx].x = cursor_x - drag_offset_x;
                g_windows[drag_win_idx].y = cursor_y - drag_offset_y;
            }
        } else {
            drag_win_idx = -1;
            resize_win_idx = -1;
            resize_edges = 0;
        }
        prev_btn = mbtn;

        /* 4. Render & Compose Desktop */
        /* Background: wallpaper (1:1 when the screen matches, nearest-neighbour
         * scale otherwise) or a solid colour if no wallpaper was loaded. */
        if (!g_have_wallpaper) {
            draw_rect(g_backbuffer, g_screen_w, g_screen_h, 0, 0,
                      g_screen_w, g_screen_h, 0x000F172A);
        } else if (g_screen_w == WP_W && g_screen_h == WP_H) {
            uint32_t n = WP_W * WP_H;
            for (uint32_t i = 0; i < n; i++) g_backbuffer[i] = g_wallpaper[i];
        } else {
            for (uint32_t sy = 0; sy < g_screen_h; sy++) {
                uint32_t wy = sy * WP_H / g_screen_h;
                const uint32_t *wrow = &g_wallpaper[wy * WP_W];
                uint32_t *brow = &g_backbuffer[sy * g_screen_w];
                for (uint32_t sx = 0; sx < g_screen_w; sx++)
                    brow[sx] = wrow[sx * WP_W / g_screen_w];
            }
        }

        /* Render Windows */
        for (int i = 0; i < MAX_WINDOWS; i++) {
            struct window *w = &g_windows[i];
            if (w->id != 0 && w->buffer) {
                if (w->minimized) continue;
                if (!w->no_decor) {
                    /* Title Bar (20px tall) */
                    uint32_t title_color = w->active ? 0x001E293B : 0x00334155;
                    draw_rect(g_backbuffer, g_screen_w, g_screen_h, w->x, w->y - 20, w->w, 20, title_color);

                    /* Border around window */
                    draw_rect(g_backbuffer, g_screen_w, g_screen_h, w->x - 1, w->y - 21, w->w + 2, w->h + 22, w->active ? 0x003B82F6 : 0x00475569);

                    /* Title Text */
                    draw_text(g_backbuffer, g_screen_w, g_screen_h, w->x + 6, w->y - 18, 0x00F8FAFC, w->title);

                    /* Minimize Button '_' */
                    draw_rect(g_backbuffer, g_screen_w, g_screen_h, w->x + w->w - 60, w->y - 20, 20, 20, 0x0064748B);
                    draw_text(g_backbuffer, g_screen_w, g_screen_h, w->x + w->w - 54, w->y - 18, 0x00FFFFFF, "_");

                    /* Maximize Button 'O' */
                    draw_rect(g_backbuffer, g_screen_w, g_screen_h, w->x + w->w - 40, w->y - 20, 20, 20, 0x0022C55E);
                    draw_text(g_backbuffer, g_screen_w, g_screen_h, w->x + w->w - 34, w->y - 18, 0x00FFFFFF, "O");

                    /* Close Button 'X' */
                    draw_rect(g_backbuffer, g_screen_w, g_screen_h, w->x + w->w - 20, w->y - 20, 20, 20, 0x00EF4444);
                    draw_text(g_backbuffer, g_screen_w, g_screen_h, w->x + w->w - 14, w->y - 18, 0x00FFFFFF, "X");

                    /* Resize grip */
                    if (!w->minimized && !w->maximized) {
                        draw_rect(g_backbuffer, g_screen_w, g_screen_h, w->x + w->w - 12, w->y + w->h - 2, 10, 2, 0x0094A3B8);
                        draw_rect(g_backbuffer, g_screen_w, g_screen_h, w->x + w->w - 2, w->y + w->h - 12, 2, 10, 0x0094A3B8);
                    }
                }

                /* Window Client Content */
                if (!w->minimized) {
                    for (int32_t wy = 0; wy < w->h; wy++) {
                        int32_t sy = w->y + wy;
                        if (sy < 0 || sy >= (int32_t)g_screen_h) continue;
                        for (int32_t wx = 0; wx < w->w; wx++) {
                            int32_t sx = w->x + wx;
                            if (sx < 0 || sx >= (int32_t)g_screen_w) continue;
                            g_backbuffer[sy * g_screen_w + sx] = w->buffer[wy * w->buf_w + wx];
                        }
                    }
                }
            }
        }

        /* Render Mouse Cursor */
        for (int y = 0; y < 12; y++) {
            for (int x = 0; x < 12; x++) {
                char pix = g_cursor_map[y][x];
                int32_t sx = cursor_x + x;
                int32_t sy = cursor_y + y;
                if (sx >= 0 && sx < (int32_t)g_screen_w && sy >= 0 && sy < (int32_t)g_screen_h) {
                    if (pix == 'W') {
                        g_backbuffer[sy * g_screen_w + sx] = 0x00FFFFFF;
                    } else if (pix == 'B') {
                        g_backbuffer[sy * g_screen_w + sx] = 0x00000000;
                    }
                }
            }
        }

        /* Blit composed frame */
        asd_fb_blit(g_backbuffer, 0, 0, g_screen_w, g_screen_h);
        asd_yield();
    }

    return 0;
}
