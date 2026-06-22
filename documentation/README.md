# OpenASD 1.0 — Developer Documentation

OpenASD is a 64-bit microkernel operating system for x86-64. This documentation covers everything needed to write programs, drivers, and filesystem backends for the platform.

---

## Contents

| Document | Description |
|----------|-------------|
| [01-architecture.md](01-architecture.md) | OS architecture: kernel, userland, boot sequence |
| [02-syscall-reference.md](02-syscall-reference.md) | Complete syscall reference (all 39 syscalls) |
| [03-writing-programs-c.md](03-writing-programs-c.md) | Writing userland programs in C with libasd |
| [04-writing-programs-rust.md](04-writing-programs-rust.md) | Writing userland programs in Rust (no_std) |
| [05-writing-drivers.md](05-writing-drivers.md) | Writing ADF kernel drivers |
| [06-writing-filesystem.md](06-writing-filesystem.md) | Writing VFS filesystem backends |
| [07-build-system.md](07-build-system.md) | Build system, toolchain, QEMU setup |
| [08-ipc-and-pipes.md](08-ipc-and-pipes.md) | Pipes, ringbufs, and IPC primitives |
| [09-debugging.md](09-debugging.md) | Debugging with QEMU serial output |
| [10-apm-package-manager.md](10-apm-package-manager.md) | APM package manager: config, database, .apkg format, repo creation |

---

## Quick start — minimal C program

```c
#include <asd/syscall.h>

int main(int argc, const char **argv) {
    asd_write(1, "hello, OpenASD\n", 15);
    asd_exit(0);
}
```

Build:
```sh
clang --target=x86_64-apple-macosx13.0 \
      -nostdlib -nostdinc -ffreestanding \
      -fno-stack-protector -fno-builtin \
      -mno-red-zone -mno-sse -mno-sse2 -mno-avx \
      -I userland/libasd/include \
      -Os -o hello.o -c hello.c

ld64.lld -arch x86_64 -platform_version macos 13.0 13.0 \
         -e _start \
         userland/libasd/build/start.o \
         hello.o \
         userland/libasd/build/libasd.a \
         -o hello
```

---

## Platform summary

| Property | Value |
|----------|-------|
| Architecture | x86-64 |
| Binary format | Mach-O 64-bit |
| Boot | UEFI → asdboot → kernel |
| Root FS | ramfs (live) / FFS (installed) |
| Syscall interface | SYSCALL/SYSRET, 39 syscalls |
| C runtime | libasd (freestanding) |
| Rust runtime | libasd_sys (no_std) |
| Signals | SIGTERM, SIGKILL, SIGCHLD |
| Page size | 4096 bytes |
| User stack | 2 MiB at 0x7FFE00000000–0x7FFF00000000 |
