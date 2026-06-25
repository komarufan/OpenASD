#include <asd/syscall.h>
#include <asd/stdio.h>
#include <asd/string.h>
#include "../libgui/gui.h"

#define COLS 60
#define ROWS 20
#define CELL_W 8
#define CELL_H 16

char g_grid[ROWS][COLS];
uint32_t g_colors[ROWS][COLS];
int g_cursor_col = 0;
int g_cursor_row = 0;

int g_win_id = -1;

static void scroll(void) {
    for (int r = 0; r < ROWS - 1; r++) {
        memcpy(g_grid[r], g_grid[r + 1], COLS);
        memcpy(g_colors[r], g_colors[r + 1], COLS * 4);
    }
    memset(g_grid[ROWS - 1], ' ', COLS);
    for (int c = 0; c < COLS; c++) {
        g_colors[ROWS - 1][c] = 0x00E2E8F0; /* Slate 200 */
    }
    g_cursor_row = ROWS - 1;
}

static void print_char(char c) {
    if (c == '\n') {
        g_cursor_col = 0;
        g_cursor_row++;
        if (g_cursor_row >= ROWS) {
            scroll();
        }
    } else if (c == '\r') {
        g_cursor_col = 0;
    } else if (c == '\b' || c == 127) {
        if (g_cursor_col > 0) {
            g_cursor_col--;
            g_grid[g_cursor_row][g_cursor_col] = ' ';
        }
    } else {
        g_grid[g_cursor_row][g_cursor_col] = c;
        g_colors[g_cursor_row][g_cursor_col] = 0x00E2E8F0;
        g_cursor_col++;
        if (g_cursor_col >= COLS) {
            g_cursor_col = 0;
            g_cursor_row++;
            if (g_cursor_row >= ROWS) {
                scroll();
            }
        }
    }
}

static void redraw(void) {
    /* Clear client area */
    gui_draw_rect(g_win_id, 0, 0, COLS * CELL_W, ROWS * CELL_H, 0x000F172A); /* Slate 900 */

    /* Draw block cursor */
    gui_draw_rect(g_win_id, g_cursor_col * CELL_W, g_cursor_row * CELL_H, CELL_W, CELL_H, 0x0022C55E); /* Green cursor */

    /* Draw text */
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            char ch = g_grid[r][c];
            if (ch != ' ') {
                char str[2] = { ch, '\0' };
                /* Draw text character */
                uint32_t color = g_colors[r][c];
                if (r == g_cursor_row && c == g_cursor_col) {
                    color = 0x000F172A; /* Invert cursor character color */
                }
                gui_draw_text(g_win_id, c * CELL_W, r * CELL_H, color, str);
            }
        }
    }
    gui_flush(g_win_id);
}

int main(void) {
    /* Initialize GUI library */
    if (gui_init() != 0) {
        printf("term: failed to connect to window server\n");
        return 1;
    }

    g_win_id = gui_create_window(100, 100, COLS * CELL_W, ROWS * CELL_H, 0x000F172A);
    if (g_win_id < 0) {
        printf("term: failed to create window\n");
        return 1;
    }

    /* Initialize terminal grid */
    for (int r = 0; r < ROWS; r++) {
        memset(g_grid[r], ' ', COLS);
        for (int c = 0; c < COLS; c++) {
            g_colors[r][c] = 0x00E2E8F0;
        }
    }

    /* Create Pipes */
    int pipe_stdin[2];
    int pipe_stdout[2];
    if (asd_pipe(pipe_stdin) != 0 || asd_pipe(pipe_stdout) != 0) {
        printf("term: failed to create pipes\n");
        return 1;
    }

    /* Save original stdin/stdout descriptors */
    asd_dup2(0, 10);
    asd_dup2(1, 11);

    /* Redirect standard descriptors */
    asd_dup2(pipe_stdin[0], 0);
    asd_dup2(pipe_stdout[1], 1);
    asd_dup2(pipe_stdout[1], 2);

    /* Close unused endpoints */
    asd_close(pipe_stdin[0]);
    asd_close(pipe_stdout[1]);

    /* Spawn Shell */
    const char *argv[] = { "/bin/asdsh", NULL };
    const char *envp[] = { NULL };
    int child_pid = asd_spawn("/bin/asdsh", argv, envp);

    /* Restore original stdin/stdout descriptors */
    asd_dup2(10, 0);
    asd_dup2(11, 1);
    asd_dup2(11, 2);
    asd_close(10);
    asd_close(11);

    if (child_pid < 0) {
        printf("term: failed to spawn shell\n");
        return 1;
    }

    int in_fd = pipe_stdout[0];
    int out_fd = pipe_stdin[1];

    redraw();

    char buf[128];
    while (1) {
        /* Read from child process */
        long n = asd_read(in_fd, buf, sizeof(buf));
        if (n > 0) {
            for (long i = 0; i < n; i++) {
                print_char(buf[i]);
            }
            redraw();
        }

        /* Read GUI events */
        ws_msg_t evt;
        if (gui_poll_event(&evt)) {
            if (evt.type == WS_EVT_KEYPRESS) {
                char k = evt.key.ch;
                /* Send typed character to shell */
                asd_write(out_fd, &k, 1);
            }
        }

        asd_yield();
    }

    return 0;
}
