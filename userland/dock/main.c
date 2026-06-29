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

    /* macOS-style dock: a horizontal bar pressed flush against the bottom edge. */
    int32_t pad_x  = 40;                          /* side padding -> bar shape   */
    int32_t pad_y  = 10;
    int32_t dock_w = TERM_ICON_W + pad_x * 2;     /* 72 + 80 = 152               */
    int32_t dock_h = TERM_ICON_H + pad_y * 2;     /* 72 + 20 = 92                */
    int32_t dock_x = (sw - dock_w) / 2;
    int32_t dock_y = sh - dock_h;                 /* flush to the very bottom    */
    int32_t icon_x = pad_x;                        /* icon centred in the bar     */
    int32_t icon_y = pad_y;

    int win_id = gui_create_window_ex(dock_x, dock_y, dock_w, dock_h, 0x001E293B,
                                      WS_WIN_NODECOR); /* panel: no title bar / close button */
    if (win_id < 0) {
        printf("dock: failed to create window\n");
        return 1;
    }

    /* Dock background: rounded-ish dark bar with the terminal icon centred. */
    gui_draw_rect(win_id, 0, 0, dock_w, dock_h, 0x001E293B);
    gui_draw_rect(win_id, 4, 4, dock_w - 8, dock_h - 8, 0x00334155);
    draw_icon(win_id, icon_x, icon_y);
    
    gui_flush(win_id);

    printf("dock: desktop dock initialized successfully\n");

    ws_msg_t evt;
    while (1) {
        if (gui_poll_event(&evt)) {
            if (evt.type == WS_EVT_MOUSE_CLICK) {
                int32_t mx = evt.mouse.x;
                int32_t my = evt.mouse.y;
                /* Check if clicked terminal icon */
                if (mx >= icon_x && mx < icon_x + TERM_ICON_W &&
                    my >= icon_y && my < icon_y + TERM_ICON_H) {
                    const char *argv[] = { "/bin/term", NULL };
                    const char *envp[] = { NULL };
                    printf("dock: spawning terminal...\n");
                    asd_spawn("/bin/term", argv, envp);
                }
            }
        }
        asd_yield();
    }

    return 0;
}
