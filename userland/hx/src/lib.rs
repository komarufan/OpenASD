//! hx — Helix-inspired modal editor for OpenASD
//! Runs bare-metal: no_std, Mach-O, raw ASD syscalls.

#![no_std]

mod panic_handler;
mod syscall;
mod buf;
mod render;
mod editor;

use editor::Editor;

// Editor lives in BSS (static), not on the 2MB userland stack.
// Buffer alone is ~2MB — putting it on the stack causes immediate overflow.
static mut EDITOR: Editor = Editor::new();

#[no_mangle]
pub extern "C" fn main(argc: i32, argv: *const *const u8, _envp: *const *const u8) -> i32 {
    let ed = unsafe { &mut EDITOR };
    ed.boot_init();

    if argc >= 2 {
        unsafe {
            let ptr = *argv.add(1);
            if !ptr.is_null() {
                let mut len = 0usize;
                while *ptr.add(len) != 0 { len += 1; }
                ed.open(core::slice::from_raw_parts(ptr, len));
            }
        }
    }

    ed.run();
    0
}
