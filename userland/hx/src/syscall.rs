extern "C" {
    // The C function is `__syscall` → Mach-O symbol `___syscall`.
    // Rust on darwin prepends one `_` to link_name, so link_name = "__syscall"
    // produces the reference `___syscall` which matches the actual symbol.
    #[link_name = "__syscall"]
    fn raw_syscall(nr: i64, a0: i64, a1: i64, a2: i64, a3: i64, a4: i64) -> i64;
}

pub const SYS_EXIT:   i64 = 1;
pub const SYS_OPEN:   i64 = 2;
pub const SYS_CLOSE:  i64 = 3;
pub const SYS_READ:   i64 = 4;
pub const SYS_WRITE:  i64 = 5;
pub const SYS_SEEK:   i64 = 7;
pub const SYS_YIELD:  i64 = 14;

// Open flags — must match kernel/arch/syscall.h and libasd/include/asd/syscall.h
pub const O_RDONLY: i64 = 0x01;
pub const O_WRONLY: i64 = 0x02;
pub const O_RDWR:   i64 = 0x03;
pub const O_CREAT:  i64 = 0x10;
pub const O_TRUNC:  i64 = 0x20;

pub fn exit(code: i32) -> ! {
    unsafe { raw_syscall(SYS_EXIT, code as i64, 0, 0, 0, 0) };
    loop {}
}

pub fn write(fd: i32, buf: &[u8]) -> i64 {
    unsafe { raw_syscall(SYS_WRITE, fd as i64, buf.as_ptr() as i64, buf.len() as i64, 0, 0) }
}

pub fn read(fd: i32, buf: &mut [u8]) -> i64 {
    unsafe { raw_syscall(SYS_READ, fd as i64, buf.as_mut_ptr() as i64, buf.len() as i64, 0, 0) }
}

pub fn open(path: &[u8], flags: i64) -> i32 {
    unsafe { raw_syscall(SYS_OPEN, path.as_ptr() as i64, flags, 0, 0, 0) as i32 }
}

pub fn close(fd: i32) {
    unsafe { raw_syscall(SYS_CLOSE, fd as i64, 0, 0, 0, 0); }
}

pub fn seek(fd: i32, offset: i64, whence: i32) -> i64 {
    unsafe { raw_syscall(SYS_SEEK, fd as i64, offset, whence as i64, 0, 0) }
}

/// Cooperatively yield the CPU.  Essential in the input loop: a foreground
/// child is not preempted (sched_tick early-returns on g_reap_waiter), so
/// busy-spinning on an empty stdin pipe would starve ws/dock/term and freeze
/// the whole desktop.
pub fn yield_now() {
    unsafe { raw_syscall(SYS_YIELD, 0, 0, 0, 0, 0); }
}
