#include "gui.h"
#include <asd/syscall.h>
#include <asd/string.h>
#include <asd/stdio.h>

static int g_ws_port = -1;
static int g_my_port = -1;
static char g_my_port_name[32];

int gui_init(void) {
    /* Wait for WS to be ready and open its port */
    while ((g_ws_port = asd_port_open(WS_PORT_NAME)) < 0) {
        asd_yield();
    }

    /* Create a port for receiving events from WS */
    int pid = asd_getpid();
    snprintf(g_my_port_name, sizeof(g_my_port_name), "gui_%d", pid);
    g_my_port = asd_port_create(g_my_port_name, 1024);
    if (g_my_port < 0) return -1;

    return 0;
}

int gui_create_window(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t bg_color) {
    ws_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = WS_MSG_CREATE_WINDOW;
    msg.create.x = x;
    msg.create.y = y;
    msg.create.w = w;
    msg.create.h = h;
    msg.create.bg_color = bg_color;
    
    /* Send my port name so WS can reply */
    strncpy(msg.text.text, g_my_port_name, sizeof(msg.text.text));
    
    asd_port_send(g_ws_port, &msg, sizeof(msg));
    
    /* Wait for WS to reply with win_id */
    ws_msg_t reply;
    while (asd_port_recv(g_my_port, &reply, sizeof(reply)) < 0) {
        asd_yield();
    }
    
    return reply.win_id;
}

int gui_draw_rect(int32_t win_id, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    ws_msg_t msg;
    msg.type = WS_MSG_DRAW_RECT;
    msg.win_id = win_id;
    msg.rect.x = x;
    msg.rect.y = y;
    msg.rect.w = w;
    msg.rect.h = h;
    msg.rect.color = color;
    return asd_port_send(g_ws_port, &msg, sizeof(msg));
}

int gui_draw_text(int32_t win_id, int32_t x, int32_t y, uint32_t color, const char *text) {
    ws_msg_t msg;
    msg.type = WS_MSG_DRAW_TEXT;
    msg.win_id = win_id;
    msg.text.x = x;
    msg.text.y = y;
    msg.text.color = color;
    strncpy(msg.text.text, text, sizeof(msg.text.text)-1);
    msg.text.text[sizeof(msg.text.text)-1] = '\0';
    return asd_port_send(g_ws_port, &msg, sizeof(msg));
}

int gui_flush(int32_t win_id) {
    ws_msg_t msg;
    msg.type = WS_MSG_FLUSH;
    msg.win_id = win_id;
    return asd_port_send(g_ws_port, &msg, sizeof(msg));
}

int gui_poll_event(ws_msg_t *evt) {
    if (asd_port_recv(g_my_port, evt, sizeof(ws_msg_t)) == sizeof(ws_msg_t)) {
        return 1;
    }
    return 0;
}
