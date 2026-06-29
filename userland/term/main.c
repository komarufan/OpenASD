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

int g_esc_state = 0;
int g_csi_arg1 = 0;
int g_csi_arg2 = 0;
int g_csi_have_arg1 = 0;
int g_csi_have_arg2 = 0;

int g_win_id = -1;

static void clear_row(int r, int from_col) {
    if (r < 0 || r >= ROWS) return;
    if (from_col < 0) from_col = 0;
    if (from_col >= COLS) return;
    memset(&g_grid[r][from_col], ' ', COLS - from_col);
    for (int c = from_col; c < COLS; c++) {
        g_colors[r][c] = 0x00E2E8F0;
    }
}

static void clear_screen(void) {
    for (int r = 0; r < ROWS; r++) {
        clear_row(r, 0);
    }
}

static void reset_escape(void) {
    g_esc_state = 0;
    g_csi_arg1 = 0;
    g_csi_arg2 = 0;
    g_csi_have_arg1 = 0;
    g_csi_have_arg2 = 0;
}

static void handle_csi(char final) {
    int arg1 = g_csi_have_arg1 ? g_csi_arg1 : 0;
    int arg2 = g_csi_have_arg2 ? g_csi_arg2 : 0;

    switch (final) {
    case 'J':
        if (arg1 == 2 || arg1 == 3) {
            clear_screen();
        }
        break;
    case 'K':
        if (arg1 == 0) {
            clear_row(g_cursor_row, g_cursor_col);
        } else if (arg1 == 2) {
            clear_row(g_cursor_row, 0);
        }
        break;
    case 'H':
    case 'f': {
        int row = (arg1 > 0 ? arg1 : 1) - 1;
        int col = (arg2 > 0 ? arg2 : 1) - 1;
        if (row < 0) row = 0;
        if (col < 0) col = 0;
        if (row >= ROWS) row = ROWS - 1;
        if (col >= COLS) col = COLS - 1;
        g_cursor_row = row;
        g_cursor_col = col;
        break;
    }
    case 'A':
        g_cursor_row -= arg1 > 0 ? arg1 : 1;
        if (g_cursor_row < 0) g_cursor_row = 0;
        break;
    case 'B':
        g_cursor_row += arg1 > 0 ? arg1 : 1;
        if (g_cursor_row >= ROWS) g_cursor_row = ROWS - 1;
        break;
    case 'C':
        g_cursor_col += arg1 > 0 ? arg1 : 1;
        if (g_cursor_col >= COLS) g_cursor_col = COLS - 1;
        break;
    case 'D':
        g_cursor_col -= arg1 > 0 ? arg1 : 1;
        if (g_cursor_col < 0) g_cursor_col = 0;
        break;
    default:
        break;
    }

    reset_escape();
}

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
    if (g_esc_state == 1) {
        if (c == '[') {
            g_esc_state = 2;
            g_csi_arg1 = 0;
            g_csi_arg2 = 0;
            g_csi_have_arg1 = 0;
            g_csi_have_arg2 = 0;
        } else {
            reset_escape();
        }
        return;
    }

    if (g_esc_state == 2) {
        if (c >= '0' && c <= '9') {
            if (g_csi_have_arg2) {
                g_csi_arg2 = g_csi_arg2 * 10 + (c - '0');
            } else {
                g_csi_arg1 = g_csi_arg1 * 10 + (c - '0');
                g_csi_have_arg1 = 1;
            }
        } else if (c == ';') {
            g_csi_have_arg2 = 1;
        } else {
            handle_csi(c);
        }
        return;
    }

    if (c == '\033') {
        g_esc_state = 1;
        return;
    }

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
    gui_draw_rect(g_win_id, g_cursor_col * CELL_W, g_cursor_row * CELL_H, CELL_W, CELL_H, 0x00FFFFFF); /* White cursor */

    /* Draw text a row at a time.  Sending one DRAW_TEXT per character can
     * overflow the window server's small IPC queue during full redraws and
     * drop glyphs; each terminal row fits in gui_draw_text()'s 64-byte text
     * payload. */
    for (int r = 0; r < ROWS; r++) {
        int first = 0;
        int last = COLS - 1;

        while (first < COLS && g_grid[r][first] == ' ') first++;
        while (last >= first && g_grid[r][last] == ' ') last--;
        if (first >= COLS) continue;

        if (r == g_cursor_row && g_cursor_col >= first && g_cursor_col <= last) {
            if (first < g_cursor_col) {
                char text[COLS + 1];
                int len = g_cursor_col - first;
                memcpy(text, &g_grid[r][first], len);
                text[len] = '\0';
                gui_draw_text(g_win_id, first * CELL_W, r * CELL_H, g_colors[r][first], text);
            }

            if (g_grid[r][g_cursor_col] != ' ') {
                char text[2] = { g_grid[r][g_cursor_col], '\0' };
                gui_draw_text(g_win_id, g_cursor_col * CELL_W, r * CELL_H,
                              0x000F172A, text); /* Invert cursor character color */
            }

            if (g_cursor_col < last) {
                char text[COLS + 1];
                int start = g_cursor_col + 1;
                int len = last - start + 1;
                memcpy(text, &g_grid[r][start], len);
                text[len] = '\0';
                gui_draw_text(g_win_id, start * CELL_W, r * CELL_H, g_colors[r][start], text);
            }
        } else {
            char text[COLS + 1];
            int len = last - first + 1;
            memcpy(text, &g_grid[r][first], len);
            text[len] = '\0';
            gui_draw_text(g_win_id, first * CELL_W, r * CELL_H, g_colors[r][first], text);
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
    if (g_win_id >= 0) gui_set_title(g_win_id, "term");
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

    /* Spawn Shell (quiet: term shows its own greeting instead of the banner) */
    const char *argv[] = { "/bin/asdsh", "-q", NULL };
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

    /* Initial greeting shown when term opens. */
    const char *greeting = "Welcome to term\n\n";
    for (const char *p = greeting; *p; p++) print_char(*p);

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
