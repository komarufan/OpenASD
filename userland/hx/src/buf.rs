//! Text buffer: static storage, no heap allocation.

pub const MAX_LINES: usize = 4096;
pub const MAX_COLS:  usize = 512;

pub struct Buffer {
    lines: [[u8; MAX_COLS]; MAX_LINES],
    lens:  [usize; MAX_LINES],
    pub count: usize,
    pub dirty: bool,
}

impl Buffer {
    pub const fn new() -> Self {
        // NOTE: every field MUST be zero here so the ~2 MB static Editor lands
        // in __bss (zero-fill) instead of __DATA (stored in the file).  A single
        // non-zero field (e.g. count: 1) forces the whole 2 MB into the binary,
        // overflowing the loader/ramfs size limit -> "invalid Mach-O".  count is
        // bumped to 1 at runtime by Editor::boot_init().
        Buffer {
            lines: [[0u8; MAX_COLS]; MAX_LINES],
            lens:  [0usize; MAX_LINES],
            count: 0,
            dirty: false,
        }
    }

    pub fn line(&self, row: usize) -> &[u8] {
        if row >= self.count { return b""; }
        &self.lines[row][..self.lens[row]]
    }

    pub fn line_len(&self, row: usize) -> usize {
        if row >= self.count { 0 } else { self.lens[row] }
    }

    pub fn insert_char(&mut self, row: usize, col: usize, ch: u8) {
        if row >= MAX_LINES { return; }
        if self.count <= row { self.count = row + 1; }
        let len = self.lens[row];
        if len >= MAX_COLS - 1 { return; }
        let col = col.min(len);
        let line = &mut self.lines[row];
        let move_len = len - col;
        for i in (0..move_len).rev() {
            line[col + 1 + i] = line[col + i];
        }
        line[col] = ch;
        self.lens[row] += 1;
        self.dirty = true;
    }

    pub fn delete_char(&mut self, row: usize, col: usize) {
        if row >= self.count { return; }
        let len = self.lens[row];
        if col >= len { return; }
        let line = &mut self.lines[row];
        for i in col..len - 1 {
            line[i] = line[i + 1];
        }
        line[len - 1] = 0;
        self.lens[row] -= 1;
        self.dirty = true;
    }

    pub fn delete_line(&mut self, row: usize) {
        if row >= self.count { return; }
        for r in row..self.count - 1 {
            self.lines[r] = self.lines[r + 1];
            self.lens[r]  = self.lens[r + 1];
        }
        if self.count > 1 {
            self.count -= 1;
        } else {
            self.lens[0] = 0;
        }
        self.dirty = true;
    }

    pub fn insert_newline(&mut self, row: usize, col: usize) {
        if self.count >= MAX_LINES { return; }
        // shift lines down
        let mut r = self.count;
        while r > row + 1 {
            self.lines[r] = self.lines[r - 1];
            self.lens[r]  = self.lens[r - 1];
            r -= 1;
        }
        self.count += 1;
        // split current line at col
        let old_len = self.lens[row];
        let col = col.min(old_len);
        let tail_len = old_len - col;
        // new line gets tail
        for i in 0..tail_len {
            self.lines[row + 1][i] = self.lines[row][col + i];
        }
        self.lens[row + 1] = tail_len;
        // current line truncated
        for i in col..old_len {
            self.lines[row][i] = 0;
        }
        self.lens[row] = col;
        self.dirty = true;
    }

    /// Load content from a byte slice (file contents).
    pub fn load(&mut self, data: &[u8]) {
        self.count = 0;
        self.dirty = false;
        let mut row = 0usize;
        let mut col = 0usize;
        for &b in data {
            if b == b'\n' {
                if row < MAX_LINES {
                    self.count = row + 1;
                }
                row += 1;
                col = 0;
                if row >= MAX_LINES { break; }
            } else if b != b'\r' {
                if col < MAX_COLS - 1 && row < MAX_LINES {
                    self.lines[row][col] = b;
                    col += 1;
                    self.lens[row] = col;
                    if row + 1 > self.count { self.count = row + 1; }
                }
            }
        }
        if self.count == 0 { self.count = 1; }
    }
}
