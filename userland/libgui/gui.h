#ifndef ASD_GUI_H
#define ASD_GUI_H

#include <stdint.h>
#include <stddef.h>

#define WS_PORT_NAME "ws_port"

/* Window Server IPC Messages */
enum ws_msg_type {
    WS_MSG_CREATE_WINDOW = 1,
    WS_MSG_DESTROY_WINDOW,
    WS_MSG_DRAW_RECT,
    WS_MSG_DRAW_TEXT,
    WS_MSG_BLIT,
    WS_MSG_FLUSH,
    WS_MSG_SET_TITLE,
    /* Events from WS to App */
    WS_EVT_MOUSE_CLICK,
    WS_EVT_KEYPRESS
};

typedef struct {
    uint32_t type;
    int32_t win_id;
    union {
        /* port_name carries the client's reply-port; it must live INSIDE the
         * create struct (not share offsets with .text) or it overwrites h/bg. */
        struct { int32_t x, y, w, h; uint32_t bg_color; char port_name[32]; uint32_t flags; } create;
        struct { int32_t x, y, w, h; uint32_t color; } rect;
        struct { int32_t x, y; uint32_t color; char text[64]; } text;
        struct { char title[32]; } title;
        struct { int32_t x, y; uint32_t btn; } mouse;
        struct { char ch; } key;
    };
} ws_msg_t;

/* GUI Library Initialization */
int gui_init(void);

/* Window creation flags */
#define WS_WIN_NODECOR  0x1   /* no title bar / border / close button (panels, docks) */

/* Window Management */
int gui_create_window(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t bg_color);
int gui_create_window_ex(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t bg_color, uint32_t flags);
int gui_set_title(int32_t win_id, const char *title);
int gui_destroy_window(int32_t win_id);

/* Drawing Commands */
int gui_draw_rect(int32_t win_id, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
int gui_draw_text(int32_t win_id, int32_t x, int32_t y, uint32_t color, const char *text);
int gui_flush(int32_t win_id);

/* Event Polling */
int gui_poll_event(ws_msg_t *evt);

#endif
