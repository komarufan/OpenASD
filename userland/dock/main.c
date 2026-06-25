#include <asd/syscall.h>
#include <asd/stdio.h>
#include <asd/string.h>
#include "../libgui/gui.h"

int main(void) {
    /* Initialize GUI library */
    if (gui_init() != 0) {
        printf("dock: failed to connect to window server\n");
        return 1;
    }

    uint32_t sw = 800, sh = 600, stride, fmt;
    asd_fb_info(&sw, &sh, &stride, &fmt);

    /* Position dock at the bottom center of the screen */
    int32_t dock_w = 200;
    int32_t dock_h = 40;
    int32_t dock_x = (sw - dock_w) / 2;
    int32_t dock_y = sh - dock_h - 10;

    int win_id = gui_create_window(dock_x, dock_y, dock_w, dock_h, 0x001E293B); /* slate blue background */
    if (win_id < 0) {
        printf("dock: failed to create window\n");
        return 1;
    }

    /* Draw "Terminal" button */
    /* Button background */
    gui_draw_rect(win_id, 10, 5, 180, 30, 0x003B82F6); /* blue button */
    /* Button text */
    gui_draw_text(win_id, 60, 12, 0x00FFFFFF, "Terminal");
    
    gui_flush(win_id);

    printf("dock: desktop dock initialized successfully\n");

    ws_msg_t evt;
    while (1) {
        if (gui_poll_event(&evt)) {
            if (evt.type == WS_EVT_MOUSE_CLICK) {
                int32_t mx = evt.mouse.x;
                int32_t my = evt.mouse.y;
                /* Check if clicked "Terminal" button */
                if (mx >= 10 && mx < 190 && my >= 5 && my < 35) {
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
