/*
 * files — a minimal graphical file explorer for OpenASD.
 *
 * Browse the filesystem with the mouse: click a folder to enter it, click ".."
 * to go up.  Space / 'n' page down, 'p' page up, Backspace goes up a level.
 * Cooperative event loop (yields) so it never starves the desktop.
 */

#include <asd/syscall.h>
#include <asd/stdio.h>
#include <asd/string.h>
#include "../libgui/gui.h"

#define WIN_W    520
#define WIN_H    440
#define ROW_H    16
#define ROWS_Y0  28          /* first row, below the path bar */
#define LEFT_PAD 8

#define BG      0x000F172A   /* slate 900   */
#define BARBG   0x001E293B   /* path bar    */
#define FG      0x00E2E8F0   /* slate 200   */
#define DIRCOL  0x0060A5FA   /* blue 400 (folders) */

static char         cwd[512] = "/";
static asd_dirent_t ents[512];
static uint32_t     nents = 0;
static int          disp[514];      /* display order: dirs first; -2 == ".." */
static int          ndisp = 0;
static int          scroll_top = 0;
static int          g_win = -1;

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int visible_rows(void)  { return (WIN_H - ROWS_Y0 - 4) / ROW_H; }
static int at_root(void)       { return cwd[0] == '/' && cwd[1] == 0; }
static int is_dot(const char *n) {
    return n[0] == '.' && (n[1] == 0 || (n[1] == '.' && n[2] == 0));
}

static void rebuild_disp(void) {
    ndisp = 0;
    if (!at_root()) disp[ndisp++] = -2;                 /* ".." */
    for (uint32_t i = 0; i < nents; i++)                 /* directories first */
        if (ents[i].kind == ASD_NODE_DIR && !is_dot(ents[i].name)) disp[ndisp++] = (int)i;
    for (uint32_t i = 0; i < nents; i++)                 /* then files */
        if (ents[i].kind != ASD_NODE_DIR && !is_dot(ents[i].name)) disp[ndisp++] = (int)i;
}

static void load_dir(void) {
    nents = 0;
    scroll_top = 0;
    asd_readdir(cwd, ents, 512, &nents);
    rebuild_disp();
}

static void path_join(const char *name) {
    int L = slen(cwd);
    if (L > 0 && cwd[L - 1] != '/') cwd[L++] = '/';
    int i = 0;
    while (name[i] && L < (int)sizeof(cwd) - 1) cwd[L++] = name[i++];
    cwd[L] = 0;
}

static void go_parent(void) {
    int L = slen(cwd);
    if (L <= 1) return;                 /* already root */
    if (cwd[L - 1] == '/') L--;          /* ignore trailing slash */
    while (L > 0 && cwd[L - 1] != '/') L--;
    if (L <= 1) { cwd[0] = '/'; cwd[1] = 0; }
    else        { cwd[L - 1] = 0; }
}

static void draw_row(int y, const char *name, int isdir) {
    char line[64];
    int k = 0;
    while (name[k] && k < 58) { line[k] = name[k]; k++; }
    if (isdir && k < 59) line[k++] = '/';
    line[k] = 0;
    gui_draw_text(g_win, LEFT_PAD, y, isdir ? DIRCOL : FG, line);
}

static void render(void) {
    gui_draw_rect(g_win, 0, 0, WIN_W, WIN_H, BG);

    /* path bar */
    gui_draw_rect(g_win, 0, 0, WIN_W, ROWS_Y0 - 4, BARBG);
    gui_draw_text(g_win, LEFT_PAD, 6, FG, cwd);

    int vr = visible_rows();
    for (int r = 0; r < vr; r++) {
        int di = scroll_top + r;
        if (di >= ndisp) break;
        int y = ROWS_Y0 + r * ROW_H;
        if (disp[di] == -2) {
            draw_row(y, "..", 1);
        } else {
            asd_dirent_t *e = &ents[disp[di]];
            draw_row(y, e->name, e->kind == ASD_NODE_DIR);
        }
    }
    gui_flush(g_win);
}

int main(void) {
    if (gui_init() != 0) { printf("files: cannot connect to window server\n"); return 1; }

    g_win = gui_create_window(140, 110, WIN_W, WIN_H, BG);
    if (g_win < 0) { printf("files: cannot create window\n"); return 1; }
    gui_set_title(g_win, "Files");

    cwd[0] = '/'; cwd[1] = 0;
    load_dir();
    render();

    ws_msg_t evt;
    for (;;) {
        if (gui_poll_event(&evt)) {
            if (evt.type == WS_EVT_MOUSE_CLICK) {
                int my = evt.mouse.y;
                if (my >= ROWS_Y0) {
                    int di = scroll_top + (my - ROWS_Y0) / ROW_H;
                    if (di >= 0 && di < ndisp) {
                        if (disp[di] == -2) {
                            go_parent(); load_dir(); render();
                        } else if (ents[disp[di]].kind == ASD_NODE_DIR) {
                            path_join(ents[disp[di]].name); load_dir(); render();
                        }
                        /* clicking a regular file is a no-op for now */
                    }
                }
            } else if (evt.type == WS_EVT_KEYPRESS) {
                char c = evt.key.ch;
                int vr = visible_rows();
                if (c == ' ' || c == 'n') {
                    if (scroll_top + vr < ndisp) { scroll_top += vr; render(); }
                } else if (c == 'p') {
                    if (scroll_top > 0) { scroll_top -= vr; if (scroll_top < 0) scroll_top = 0; render(); }
                } else if (c == '\b' || c == 0x7f) {
                    go_parent(); load_dir(); render();
                }
            }
        }
        asd_yield();
    }
    return 0;
}
