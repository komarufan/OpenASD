#include <asd/syscall.h>
#include <asd/stdio.h>
#include <asd/string.h>
#include "../libgui/gui.h"
#include "term_icon.h"

static void draw_icon(int win_id, int32_t x, int32_t y) {
    for (int iy = 0; iy < TERM_ICON_H; iy++) {
        for (int ix = 0; ix < TERM_ICON_W; ix++) {
            uint32_t p = g_term_icon[iy * TERM_ICON_W + ix];
            uint32_t a = p >> 24;
            if (a < 64) continue;
            gui_draw_rect(win_id, x + ix, y + iy, 1, 1, p & 0x00FFFFFF);
        }
        /* Keep the ws_port queue below PORT_QUEUE_DEPTH while drawing the icon. */
        asd_yield();
    }
}

int main(void) {
    /* Initialize GUI library */
    if (gui_init() != 0) {
        printf("dock: failed to connect to window server\n");
        return 1;
    }

    uint32_t sw = 800, sh = 600, stride, fmt;
    asd_fb_info(&sw, &sh, &stride, &fmt);

    /* macOS-style dock: a horizontal bar pressed flush against the bottom edge,
     * holding two app icons side by side (terminal + file explorer). */
    const int32_t ICON = TERM_ICON_W;             /* 72 (square icons)           */
    int32_t pad_x  = 24;                           /* side padding                */
    int32_t pad_y  = 10;
    int32_t gap    = 16;                           /* spacing between icons       */
    int32_t dock_w = pad_x * 2 + ICON * 2 + gap;   /* 48 + 144 + 16 = 208         */
    int32_t dock_h = ICON + pad_y * 2;             /* 72 + 20 = 92                */
    int32_t dock_x = (sw - dock_w) / 2;
    int32_t dock_y = sh - dock_h;                  /* flush to the very bottom    */
    int32_t icon_y = pad_y;
    int32_t term_x  = pad_x;                        /* slot 0: terminal            */
    int32_t files_x = pad_x + ICON + gap;           /* slot 1: file explorer       */

    int win_id = gui_create_window_ex(dock_x, dock_y, dock_w, dock_h, 0x001E293B,
                                      WS_WIN_NODECOR); /* panel: no title bar / close button */
    if (win_id < 0) {
        printf("dock: failed to create window\n");
        return 1;
    }

    /* Dock background + icons. */
    gui_draw_rect(win_id, 0, 0, dock_w, dock_h, 0x001E293B);
    gui_draw_rect(win_id, 4, 4, dock_w - 8, dock_h - 8, 0x00334155);
    draw_icon(win_id, term_x, icon_y);                       /* terminal icon */
    /* file explorer: placeholder square (real icon TBD). */
    gui_draw_rect(win_id, files_x, icon_y, ICON, ICON, 0x0094A3B8);
    gui_draw_rect(win_id, files_x + 3, icon_y + 3, ICON - 6, ICON - 6, 0x00CBD5E1);

    gui_flush(win_id);

    printf("dock: desktop dock initialized successfully\n");

    ws_msg_t evt;
    while (1) {
        if (gui_poll_event(&evt)) {
            if (evt.type == WS_EVT_MOUSE_CLICK) {
                int32_t mx = evt.mouse.x;
                int32_t my = evt.mouse.y;
                if (my >= icon_y && my < icon_y + ICON) {
                    if (mx >= term_x && mx < term_x + ICON) {
                        const char *argv[] = { "/bin/term", NULL };
                        const char *envp[] = { NULL };
                        printf("dock: spawning terminal...\n");
                        asd_spawn("/bin/term", argv, envp);
                    } else if (mx >= files_x && mx < files_x + ICON) {
                        const char *argv[] = { "/bin/files", NULL };
                        const char *envp[] = { NULL };
                        printf("dock: spawning file explorer...\n");
                        asd_spawn("/bin/files", argv, envp);
                    }
                }
            }
        }
        asd_yield();
    }

    return 0;
}
