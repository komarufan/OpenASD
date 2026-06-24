//! Editor state machine: normal / insert / command modes.

use crate::buf::Buffer;
use crate::render::{Screen, render_full, clear_screen};
use crate::syscall::{self, O_RDONLY, O_WRONLY, O_CREAT, O_TRUNC};

const STDIN:  i32 = 0;
const STDOUT: i32 = 1;

#[derive(PartialEq, Clone, Copy)]
enum Mode {
    Normal,
    Insert,
    Command,
}

pub struct Editor {
    buf: Buffer,
    screen: Screen,
    cursor_row: usize,
    cursor_col: usize,
    top_row: usize,
    mode: Mode,
    filename: [u8; 256],
    filename_len: usize,
    cmd_buf: [u8; 128],
    cmd_len: usize,
    status: [u8; 128],
    status_len: usize,
    quit: bool,
}

impl Editor {
    pub const fn new() -> Self {
        Editor {
            buf: Buffer::new(),
            screen: Screen::new(),
            cursor_row: 0,
            cursor_col: 0,
            top_row: 0,
            mode: Mode::Normal,
            filename: [0u8; 256],
            filename_len: 0,
            cmd_buf: [0u8; 128],
            cmd_len: 0,
            status: [0u8; 128],
            status_len: 0,
            quit: false,
        }
    }

    pub fn open(&mut self, path: &[u8]) {
        let len = path.len().min(255);
        self.filename[..len].copy_from_slice(&path[..len]);
        self.filename_len = len;
        // null-terminate for syscall
        let mut pathbuf = [0u8; 257];
        pathbuf[..len].copy_from_slice(&path[..len]);

        let fd = syscall::open(&pathbuf[..len + 1], O_RDONLY);
        if fd < 0 {
            self.set_status(b"[New file]");
            return;
        }
        // read up to 512KB
        static mut FILE_BUF: [u8; 524288] = [0u8; 524288];
        let n = unsafe { syscall::read(fd, &mut FILE_BUF) };
        syscall::close(fd);
        if n > 0 {
            let data = unsafe { &FILE_BUF[..n as usize] };
            self.buf.load(data);
        }
        self.set_status(b"");
    }

    fn save(&mut self) {
        if self.filename_len == 0 {
            self.set_status(b"No filename");
            return;
        }
        let mut pathbuf = [0u8; 257];
        pathbuf[..self.filename_len].copy_from_slice(&self.filename[..self.filename_len]);
        let fd = syscall::open(&pathbuf[..self.filename_len + 1], O_WRONLY | O_CREAT | O_TRUNC);
        if fd < 0 {
            self.set_status(b"Cannot write file");
            return;
        }
        for row in 0..self.buf.count {
            let line = self.buf.line(row);
            if !line.is_empty() {
                syscall::write(fd, line);
            }
            syscall::write(fd, b"\n");
        }
        syscall::close(fd);
        self.buf.dirty = false;
        self.set_status(b"Saved");
    }

    fn set_status(&mut self, msg: &[u8]) {
        let n = msg.len().min(127);
        self.status[..n].copy_from_slice(&msg[..n]);
        self.status_len = n;
    }

    fn mode_name(&self) -> &str {
        match self.mode {
            Mode::Normal  => "NORMAL",
            Mode::Insert  => "INSERT",
            Mode::Command => "COMMAND",
        }
    }

    fn clamp_cursor(&mut self) {
        if self.cursor_row >= self.buf.count {
            self.cursor_row = self.buf.count.saturating_sub(1);
        }
        let line_len = self.buf.line_len(self.cursor_row);
        if self.mode == Mode::Normal {
            // In normal mode cursor can't be past last char
            if line_len == 0 {
                self.cursor_col = 0;
            } else if self.cursor_col >= line_len {
                self.cursor_col = line_len - 1;
            }
        } else if self.cursor_col > line_len {
            self.cursor_col = line_len;
        }
    }

    fn scroll(&mut self) {
        let view_rows = if self.screen.rows > 2 { self.screen.rows - 2 } else { 1 };
        if self.cursor_row < self.top_row {
            self.top_row = self.cursor_row;
        } else if self.cursor_row >= self.top_row + view_rows {
            self.top_row = self.cursor_row - view_rows + 1;
        }
    }

    fn redraw(&mut self) {
        let status = &self.status[..self.status_len];
        let fname  = &self.filename[..self.filename_len];
        render_full(
            &self.buf, &self.screen,
            self.top_row, self.cursor_row, self.cursor_col,
            self.mode_name(), fname, status,
        );
    }

    fn handle_normal(&mut self, ch: u8) {
        match ch {
            b'h' | 0x82 => {  /* h or ← */
                if self.cursor_col > 0 { self.cursor_col -= 1; }
            }
            b'l' | 0x83 => {  /* l or → */
                let len = self.buf.line_len(self.cursor_row);
                if len > 0 && self.cursor_col < len - 1 { self.cursor_col += 1; }
            }
            b'k' | 0x80 => {  /* k or ↑ */
                if self.cursor_row > 0 { self.cursor_row -= 1; }
            }
            b'j' | 0x81 => {  /* j or ↓ */
                if self.cursor_row + 1 < self.buf.count { self.cursor_row += 1; }
            }
            b'0' => { self.cursor_col = 0; }
            b'$' => {
                let len = self.buf.line_len(self.cursor_row);
                self.cursor_col = if len > 0 { len - 1 } else { 0 };
            }
            b'g' => { self.cursor_row = 0; self.cursor_col = 0; }
            b'G' => {
                self.cursor_row = self.buf.count.saturating_sub(1);
                self.cursor_col = 0;
            }
            b'i' => {
                self.mode = Mode::Insert;
                self.set_status(b"");
            }
            b'a' => {
                // append after cursor
                let len = self.buf.line_len(self.cursor_row);
                if len > 0 { self.cursor_col += 1; }
                self.mode = Mode::Insert;
                self.set_status(b"");
            }
            b'o' => {
                // open new line below
                let row = self.cursor_row;
                let len = self.buf.line_len(row);
                self.buf.insert_newline(row, len);
                self.cursor_row = row + 1;
                self.cursor_col = 0;
                self.mode = Mode::Insert;
            }
            b'O' => {
                // open new line above
                let row = self.cursor_row;
                self.buf.insert_newline(row, 0);
                self.cursor_col = 0;
                self.mode = Mode::Insert;
            }
            b'x' => {
                // delete char under cursor
                self.buf.delete_char(self.cursor_row, self.cursor_col);
            }
            b'd' => {
                // dd — handled as single keypress 'd' = delete line in simplified mode
                self.buf.delete_line(self.cursor_row);
                self.clamp_cursor();
            }
            b':' => {
                self.mode = Mode::Command;
                self.cmd_len = 0;
                self.cmd_buf[0] = 0;
                self.set_status(b":");
            }
            _ => {}
        }
    }

    fn handle_insert(&mut self, ch: u8) {
        match ch {
            0x1b => {
                // ESC → normal mode
                self.mode = Mode::Normal;
                if self.cursor_col > 0 { self.cursor_col -= 1; }
                self.set_status(b"");
            }
            0x80 => { /* ↑ in insert mode */
                if self.cursor_row > 0 { self.cursor_row -= 1; }
            }
            0x81 => { /* ↓ in insert mode */
                if self.cursor_row + 1 < self.buf.count { self.cursor_row += 1; }
            }
            0x82 => { /* ← in insert mode */
                if self.cursor_col > 0 { self.cursor_col -= 1; }
            }
            0x83 => { /* → in insert mode */
                let len = self.buf.line_len(self.cursor_row);
                if len > 0 && self.cursor_col < len { self.cursor_col += 1; }
            }
            0x0d | b'\n' => {
                // Enter
                let row = self.cursor_row;
                let col = self.cursor_col;
                self.buf.insert_newline(row, col);
                self.cursor_row += 1;
                self.cursor_col = 0;
            }
            0x7f | 0x08 => {
                // Backspace
                if self.cursor_col > 0 {
                    self.cursor_col -= 1;
                    self.buf.delete_char(self.cursor_row, self.cursor_col);
                } else if self.cursor_row > 0 {
                    // join with previous line
                    let prev_len = self.buf.line_len(self.cursor_row - 1);
                    // copy current line to end of previous
                    let cur_len = self.buf.line_len(self.cursor_row);
                    for i in 0..cur_len {
                        let ch = self.buf.line(self.cursor_row)[i];
                        self.buf.insert_char(self.cursor_row - 1, prev_len + i, ch);
                    }
                    self.buf.delete_line(self.cursor_row);
                    self.cursor_row -= 1;
                    self.cursor_col = prev_len;
                }
            }
            0x20..=0x7e => {
                // Printable ASCII
                self.buf.insert_char(self.cursor_row, self.cursor_col, ch);
                self.cursor_col += 1;
            }
            _ => {}
        }
    }

    fn handle_command(&mut self, ch: u8) {
        match ch {
            0x1b => {
                self.mode = Mode::Normal;
                self.set_status(b"");
            }
            0x0d | b'\n' => {
                // Execute command
                let cmd = &self.cmd_buf[..self.cmd_len];
                if cmd == b"w" || cmd == b"write" {
                    self.save();
                } else if cmd == b"q" || cmd == b"quit" {
                    if self.buf.dirty {
                        self.set_status(b"Unsaved changes! Use :q! to force quit");
                    } else {
                        self.quit = true;
                    }
                } else if cmd == b"q!" {
                    self.quit = true;
                } else if cmd == b"wq" {
                    self.save();
                    self.quit = true;
                } else {
                    self.set_status(b"Unknown command");
                }
                self.mode = Mode::Normal;
                self.cmd_len = 0;
            }
            0x7f | 0x08 => {
                if self.cmd_len > 0 {
                    self.cmd_len -= 1;
                    // update status to show current command
                    let mut s = [b':'; 128];
                    s[1..1 + self.cmd_len].copy_from_slice(&self.cmd_buf[..self.cmd_len]);
                    self.set_status(&s[..1 + self.cmd_len]);
                } else {
                    self.mode = Mode::Normal;
                    self.set_status(b"");
                }
            }
            0x20..=0x7e => {
                if self.cmd_len < 127 {
                    self.cmd_buf[self.cmd_len] = ch;
                    self.cmd_len += 1;
                    // show in status line
                    let mut s = [0u8; 128];
                    s[0] = b':';
                    s[1..1 + self.cmd_len].copy_from_slice(&self.cmd_buf[..self.cmd_len]);
                    self.set_status(&s[..1 + self.cmd_len]);
                }
            }
            _ => {}
        }
    }

    pub fn run(&mut self) {
        clear_screen();
        loop {
            self.clamp_cursor();
            self.scroll();
            self.redraw();

            if self.quit { break; }

            // Read one byte from stdin
            let mut ch = [0u8; 1];
            let n = syscall::read(STDIN, &mut ch);
            if n <= 0 { continue; }

            match self.mode {
                Mode::Normal  => self.handle_normal(ch[0]),
                Mode::Insert  => self.handle_insert(ch[0]),
                Mode::Command => self.handle_command(ch[0]),
            }
        }
        clear_screen();
    }
}
