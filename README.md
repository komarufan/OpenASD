<div align="center">
  <img src="documentation/logo.png" alt="OpenASD Logo" width="500" />
  <br><br>
  <p><b>A modern, experimental x86-64 microkernel operating system</b></p>
  
  <a href="https://openasd.msh356.store/"><img src="https://img.shields.io/badge/Website-000000?style=for-the-badge&logo=googlechrome&logoColor=white" alt="Site" /></a>
  <a href="https://discord.gg/ByrXNj8YH"><img src="https://img.shields.io/badge/Discord-5865F2?style=for-the-badge&logo=discord&logoColor=white" alt="Discord" /></a>
  <a href="https://t.me/openasd_ru"><img src="https://img.shields.io/badge/Telegram-2CA5E0?style=for-the-badge&logo=telegram&logoColor=white" alt="Telegram" /></a>
  <a href="https://wiki.openasd.msh356.store/doku.php?id=start"><img src="https://img.shields.io/badge/Wiki-FFFFFF?style=for-the-badge&logo=wikipedia&logoColor=black" alt="Wiki" /></a>
  <br><br>
  
  [![License: BSD 2-Clause](https://img.shields.io/badge/License-BSD_2--Clause-blue.svg)](https://opensource.org/licenses/BSD-2-Clause)
  [![Platform: x86-64](https://img.shields.io/badge/Platform-x86--64-lightgrey.svg)](#)
  [![Language: C11](https://img.shields.io/badge/Language-C11-00599C.svg)](#)
  [![Language: Assembly](https://img.shields.io/badge/Language-Assembly-4EAA25.svg)](#)
</div>

---

**OpenASD** is a research microkernel operating system built from scratch for the x86-64 architecture. Written entirely in freestanding C and assembly, it aims to explore modern OS concepts while remaining minimalistic and highly educational.

From its custom UEFI bootloader to a robust VFS and a growing userspace, OpenASD is a complete system that runs on bare metal.

## Core Features

- **Custom Bootloader**  
  Native UEFI boot process (`asdboot`) that sets up the hardware environment before handing off to the kernel.
- **Advanced Scheduling**  
  Implements a CFS-style (Completely Fair Scheduler) using red-black trees and per-CPU run queues for efficient multitasking.
- **Robust VFS & File Systems**  
  Supports FAT32 (read-only) for boot media and a fully functional `ramfs` (read-write) for temporary data.
- **Secure Foundation**  
  - Passwords are hashed using a salted, iterated mixing function (FNV-1a + Murmur-style finalization).
  - Strict pointer validation across all system calls to prevent userland from corrupting kernel space.
- **Port-based IPC**  
  Inter-process communication is handled via high-performance message ports and ring buffers.
- **TUI Installer**  
  A pseudo-graphical installer featuring disk selection, hostname configuration, and automated user account creation.

## Architecture

OpenASD follows a clean separation of concerns, dividing the system into distinct functional layers:

```text
boot/        — UEFI bootloader (asdboot)
kernel/      — The core kernel
  ├─ arch/   — Hardware specifics (GDT, IDT, ISR, PIC, PIT, Syscall, ELF)
  ├─ block/  — Block device layer and GPT parsing
  ├─ drv/    — Device drivers (virtio-blk, PS/2 keyboard, ADF)
  ├─ ipc/    — Port-based IPC mechanism
  ├─ mm/     — Physical buddy allocator & virtual memory maps
  ├─ sched/  — CFS-style scheduler
  └─ vfs/    — Virtual File System (FAT32, ramfs)
init/        — PID 1 (asdinit), TUI installer, login, and kernel shell
userland/    — Bare-metal userspace environment
  ├─ libasd/ — Minimal freestanding libc
  └─ sh/     — asdsh: The system shell
```

## Under the Hood

### System V AMD64 ABI
The kernel strictly adheres to the System V AMD64 ABI. It correctly constructs the initial stack (`argc`, `argv`, `envp`) for userland processes, ensuring seamless compatibility and standard behavior for compiled C programs.

### Syscall Interface
OpenASD provides a minimal POSIX-like system call interface that closely mirrors the Linux x86-64 ABI. This compatibility layer allows for straightforward porting of existing simple C programs into the OpenASD ecosystem.

- `rax` holds the system call number.
- `rdi`, `rsi`, `rdx`, `r10`, `r8`, `r9` carry the arguments.
- Returns are passed back via `rax`.

## Documentation

For more in-depth technical details on various subsystems, check out the `documentation/` directory in this repository. It covers everything from writing filesystem drivers to using the ASD Package Manager (APM).

## License

OpenASD is open-source software licensed under the **BSD 2-Clause License**. See the [LICENSE](LICENSE) file for more details.

## Star History

<a href="https://star-history.com/#komarufan/OpenASD&Date">
 <picture>
   <source media="(prefers-color-scheme: dark)" srcset="https://api.star-history.com/svg?repos=komarufan/OpenASD&type=Date&theme=dark" />
   <source media="(prefers-color-scheme: light)" srcset="https://api.star-history.com/svg?repos=komarufan/OpenASD&type=Date" />
   <img alt="Star History Chart" src="https://api.star-history.com/svg?repos=komarufan/OpenASD&type=Date" />
 </picture>
</a>
