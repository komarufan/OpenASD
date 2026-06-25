#include <asd/syscall.h>
#include <asd/stdio.h>
#include <asd/string.h>
#include "../libgui/gui.h"
#include "../../kernel/console/font_bsd_vgarom_8x16.h"

#define MAX_WINDOWS 16

struct window {
    int32_t id;
    int32_t x, y, w, h;
    uint32_t bg_color;
    uint32_t *buffer;
    int client_port;
    char client_port_name[32];
    int active;
};

struct window g_windows[MAX_WINDOWS];
int g_next_win_id = 1;

uint32_t *g_backbuffer = NULL;
uint32_t g_screen_w = 0;
uint32_t g_screen_h = 0;
uint32_t g_stride = 0;
uint32_t g_format = 0;

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

    int32_t cursor_x = g_screen_w / 2;
    int32_t cursor_y = g_screen_h / 2;
    uint32_t prev_btn = 0;
    int drag_win_idx = -1;
    int32_t drag_offset_x = 0;
    int32_t drag_offset_y = 0;

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
                    w->bg_color = msg.create.bg_color;
                    
                    /* Client buffer with 20px title bar space */
                    size_t buf_sz = w->w * w->h * 4;
                    w->buffer = (uint32_t *)asd_mmap(NULL, buf_sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                    
                    /* Clear client area to bg_color */
                    for (int32_t i = 0; i < w->w * w->h; i++) {
                        w->buffer[i] = w->bg_color;
                    }

                    strncpy(w->client_port_name, msg.text.text, sizeof(w->client_port_name));
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
                    } else if (msg.type == WS_MSG_DRAW_RECT) {
                        draw_rect(w->buffer, w->w, w->h, msg.rect.x, msg.rect.y, msg.rect.w, msg.rect.h, msg.rect.color);
                    } else if (msg.type == WS_MSG_DRAW_TEXT) {
                        draw_text(w->buffer, w->w, w->h, msg.text.x, msg.text.y, msg.text.color, msg.text.text);
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

        /* 3. Poll Mouse */
        int32_t mx, my;
        uint32_t mbtn;
        asd_get_mouse(&mx, &my, &mbtn);
        
        cursor_x = (mx * (int32_t)g_screen_w) / 4096;
        cursor_y = (my * (int32_t)g_screen_h) / 4096;
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
                        /* Check hit including title bar (+20px on top) */
                        if (cursor_x >= w->x && cursor_x < w->x + w->w &&
                            cursor_y >= w->y - 20 && cursor_y < w->y + w->h) {
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
                        /* Close Button? (Top right 20x20 area) */
                        if (cursor_x >= w->x + w->w - 20) {
                            asd_port_close(w->client_port);
                            w->id = 0;
                        } else {
                            /* Drag window start */
                            drag_win_idx = hit_idx;
                            drag_offset_x = cursor_x - w->x;
                            drag_offset_y = cursor_y - w->y;
                        }
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
            } else if (drag_win_idx != -1) {
                /* Dragging */
                g_windows[drag_win_idx].x = cursor_x - drag_offset_x;
                g_windows[drag_win_idx].y = cursor_y - drag_offset_y;
            }
        } else {
            drag_win_idx = -1;
        }
        prev_btn = mbtn;

        /* 4. Render & Compose Desktop */
        /* Background: Solid premium dark slate blue */
        draw_rect(g_backbuffer, g_screen_w, g_screen_h, 0, 0, g_screen_w, g_screen_h, 0x000F172A);

        /* Render Windows */
        for (int i = 0; i < MAX_WINDOWS; i++) {
            struct window *w = &g_windows[i];
            if (w->id != 0) {
                /* Title Bar (20px tall) */
                uint32_t title_color = w->active ? 0x001E293B : 0x00334155;
                draw_rect(g_backbuffer, g_screen_w, g_screen_h, w->x, w->y - 20, w->w, 20, title_color);
                
                /* Border around window */
                draw_rect(g_backbuffer, g_screen_w, g_screen_h, w->x - 1, w->y - 21, w->w + 2, w->h + 22, w->active ? 0x003B82F6 : 0x00475569);

                /* Title Text */
                draw_text(g_backbuffer, g_screen_w, g_screen_h, w->x + 6, w->y - 18, 0x00F8FAFC, w->client_port_name);

                /* Close Button 'X' */
                draw_rect(g_backbuffer, g_screen_w, g_screen_h, w->x + w->w - 20, w->y - 20, 20, 20, 0x00EF4444);
                draw_text(g_backbuffer, g_screen_w, g_screen_h, w->x + w->w - 14, w->y - 18, 0x00FFFFFF, "X");

                /* Window Client Content */
                for (int32_t wy = 0; wy < w->h; wy++) {
                    int32_t sy = w->y + wy;
                    if (sy < 0 || sy >= (int32_t)g_screen_h) continue;
                    for (int32_t wx = 0; wx < w->w; wx++) {
                        int32_t sx = w->x + wx;
                        if (sx < 0 || sx >= (int32_t)g_screen_w) continue;
                        g_backbuffer[sy * g_screen_w + sx] = w->buffer[wy * w->w + wx];
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
