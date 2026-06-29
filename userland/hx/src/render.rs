//! ANSI terminal rendering for hx.
//! fbcon.c in the kernel supports: ESC[H, ESC[2J, ESC[%d;%dH, ESC[7m, ESC[0m

use crate::syscall::write;
use crate::buf::Buffer;

const STDOUT: i32 = 1;

// Max screen size we'll ever draw
const MAX_ROWS: usize = 50;
const MAX_COLS: usize = 220;

pub struct Screen {
    pub rows: usize,
    pub cols: usize,
}

impl Screen {
    pub const fn new() -> Self {
        // Zero so the enclosing static Editor stays in __bss (see buf.rs);
        // real dimensions are set in Editor::boot_init().
        Screen { rows: 0, cols: 0 }
    }
}

fn write_str(s: &str) {
    write(STDOUT, s.as_bytes());
}

fn write_bytes(b: &[u8]) {
    write(STDOUT, b);
}

fn write_u32(mut v: u32) {
    if v == 0 { write(STDOUT, b"0"); return; }
    let mut buf = [0u8; 10];
    let mut i = 10usize;
    while v > 0 {
        i -= 1;
        buf[i] = b'0' + (v % 10) as u8;
        v /= 10;
    }
    write(STDOUT, &buf[i..]);
}

/// ESC[row;colH  (1-based)
fn goto(row: usize, col: usize) {
    write_str("\x1b[");
    write_u32((row + 1) as u32);
    write_str(";");
    write_u32((col + 1) as u32);
    write_str("H");
}

pub fn clear_screen() {
    write_str("\x1b[2J\x1b[H");
}

pub fn render_full(buf: &Buffer, scr: &Screen, top_row: usize,
                   cursor_row: usize, cursor_col: usize,
                   mode_name: &str, filename: &[u8], status_msg: &[u8]) {
    clear_screen();

    let view_rows = if scr.rows > 2 { scr.rows - 2 } else { 1 };

    for screen_row in 0..view_rows.min(MAX_ROWS) {
        let buf_row = top_row + screen_row;
        goto(screen_row, 0);

        // line number (3 digits)
        let linenum = buf_row + 1;
        if linenum < 10   { write_str("  "); }
        else if linenum < 100 { write_str(" "); }
        write_u32(linenum as u32);
        write_str(" ");

        if buf_row < buf.count {
            let line = buf.line(buf_row);
            let draw_len = line.len().min(scr.cols.saturating_sub(4));
            write_bytes(&line[..draw_len]);
        } else {
            write_str("~");
        }
    }

    // Status bar (second-to-last row)
    if scr.rows >= 2 {
        goto(scr.rows - 2, 0);
        write_str("\x1b[7m"); // reverse video
        write_str(" ");
        write_str(mode_name);
        write_str(" | ");
        if !filename.is_empty() {
            write_bytes(filename);
        } else {
            write_str("[No Name]");
        }
        write_str(" ");
        if buf.dirty { write_str("[+] "); }
        // pad to fill row
        write_str("\x1b[0m");
    }

    // Message/command line (last row)
    if scr.rows >= 1 {
        goto(scr.rows - 1, 0);
        write_str("\x1b[K"); // clear to end of line
        if !status_msg.is_empty() {
            write_bytes(status_msg);
        }
    }

    // Restore cursor position
    let screen_cursor_row = cursor_row.saturating_sub(top_row);
    goto(screen_cursor_row, cursor_col + 4); // +4 for line number gutter
}
