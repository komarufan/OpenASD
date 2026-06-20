# OpenASD — x86-64 Operating System

[![Build](https://github.com/komarufan/OpenASD/actions/workflows/build.yml/badge.svg)](https://github.com/komarufan/OpenASD/actions/workflows/build.yml)

OpenASD is a research microkernel operating system for x86-64 written in freestanding C and assembly.
It boots via UEFI, has its own bootloader, kernel, VFS, scheduler, block driver, and a growing userspace.

## Recent Improvements

- **TUI Installer**: Pseudo-graphical installer with disk selection, hostname configuration, and user account creation.
- **Installation Speed**: Block copy operations are now performed in 32 KiB chunks (64 sectors), significantly speeding up the installation process.
- **Security**: Passwords are now hashed using a salted, iterated mixing function (FNV-1a + Murmur-style finalisation) to resist brute-force attacks. Pointer validation has been added to all system calls.
- **ABI & Shell**: The kernel now correctly builds the System V AMD64 ABI initial stack (argc, argv, envp) for userland processes. The `asdsh` shell has been improved with new commands (`whoami`, `id`, `users`, `uname`, `clear`, `reboot`, `shutdown`, `ps`, `uptime`) and support for executing external binaries from `/bin`.

## Architecture

```
boot/        — UEFI bootloader (asdboot)
kernel/      — kernel (entry, MM, sched, VFS, IPC, drivers)
  arch/      — x86-64: GDT, IDT, ISR stubs, PIC, PIT, syscall, ELF loader
  block/     — block layer + GPT
  console/   — framebuffer console (VGA-ROM 8×16 font)
  drv/       — virtio-blk, PS/2 keyboard, ADF driver framework
  ipc/       — port-based IPC, ring buffers
  mm/        — physical buddy allocator, virtual memory maps
  sched/     — CFS-style scheduler (red-black tree, per-CPU run queues)
  usr/       — user/group database
  vfs/       — VFS layer, FAT32 (read-only), ramfs (read-write)
init/        — asdinit: PID 1, installer, login, kernel-mode shell
userland/    — bare-metal userspace
  libasd/    — freestanding libc (syscall wrappers, stdio, string)
  sh/        — asdsh: userspace shell (ELF, links against libasd)
  drvmgr/    — driver manager stub
doc/         — design documents
```

## Building & Running

### Requirements

- `clang` (targeting `x86_64-unknown-elf`)
- `ld.lld`
- `make`
- `mtools` and `dosfstools` (for `make run`)
- OVMF firmware for QEMU testing

### Running in QEMU

```sh
make run
```

### Running on real hardware (USB boot)

Build a bootable USB image with a proper GPT + EFI System Partition:

```sh
make usb-image
# Output: build/usb/openasd-1.0.img
```

Write to a USB drive:

```sh
# Linux (replace /dev/sdX with your USB device)
sudo dd if=build/usb/openasd-1.0.img of=/dev/sdX bs=4M status=progress && sync

# Windows
# Use Rufus → select the .img file → mode: DD Image → Start
```

Then select the USB drive in your UEFI boot menu. OpenASD will appear as **"OpenASD 1.0"**.

## Porting Software to OpenASD

OpenASD provides a minimal POSIX-like system call interface that closely mirrors the Linux x86-64 ABI. This makes porting simple C programs relatively straightforward.

### System Call Interface

The system call convention is identical to Linux x86-64:
- `rax` = syscall number
- `rdi`, `rsi`, `rdx`, `r10`, `r8`, `r9` = arguments
- Return value in `rax` (negative values indicate errors).

The `libasd` library provides low-level wrappers for these system calls in `<asd/syscall.h>`.

### Example: Porting `fastfetch`

To port a program like `fastfetch` (or a minimal clone of it) to OpenASD, follow these steps:

#### 1. Prepare the Source Code

Create a minimal C file that uses the `libasd` system calls to gather system information. OpenASD provides the `SYS_UNAME` system call to retrieve OS details.

```c
/* fastfetch_asd.c */
#include <asd/syscall.h>
#include <asd/stdio.h>    /* printf, puts */

int main(int argc, char **argv) {
    asd_utsname_t uts;
    
    if (asd_uname(&uts) == 0) {
        printf("       /\\        OS: %s %s %s\n", uts.sysname, uts.release, uts.machine);
        printf("      /  \\       Host: %s\n", uts.nodename);
        printf("     /____\\      Kernel: OpenASD\n");
        printf("    /      \\     Shell: asdsh\n");
        printf("   /        \\    \n");
    } else {
        printf("Error retrieving system info.\n");
    }
    
    return 0;
}
```

#### 2. Update the Build System

Add your program to the userland build system. OpenASD compiles userland binaries as freestanding ELF executables linked against `libasd`.

Edit `userland/bin/Makefile`:

```makefile
# Add your binary to the TARGETS list
TARGETS = ls cat mkdir rm touch echo pwd fastfetch

# Add a build rule for your binary
$(BUILD)/fastfetch: $(BUILD)/fastfetch.o $(LIBASD)
	$(LD) $(LDFLAGS) -o $@ ../libasd/build/start.o $< $(LIBASD)
```

#### 3. Build and Run

Rebuild the OS image:

```bash
make clean
make all
make run
```

Once booted into OpenASD, you can run your ported program directly from the shell:

```
root@asd:/root # fastfetch
       /\        OS: OpenASD 1.0 x86_64
      /  \       Host: asd
     /____\      Kernel: OpenASD
    /      \     Shell: asdsh
   /        \    
```

### Limitations

Currently, OpenASD does not support:
- Dynamic linking (all binaries must be statically linked).
- Standard C library (`libc`) beyond the minimal `libasd` provided.
- Advanced memory management (e.g., `mmap` is limited to anonymous private mappings).
- Complex terminal I/O (`ioctl` returns `ENOTTY`).

When porting larger software, you may need to stub out unsupported features or provide minimal implementations within `libasd`.

## License

BSD 2-Clause. See [LICENSE](LICENSE).
