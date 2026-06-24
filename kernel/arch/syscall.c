/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, ASD Project Contributors
 *
 * SYSCALL/SYSRET setup and dispatch table.
 *
 * Calling convention (matches Linux x86-64 for easy userland porting):
 *   rax = syscall number
 *   rdi, rsi, rdx, r10, r8, r9 = arguments (up to 6)
 *   Return value in rax.
 *
 * Security improvements (v5):
 *  - All user-space pointer arguments are validated before use.
 *  - sys_write validates buf pointer.
 *  - sys_stat validates both path and stat-buffer pointers.
 *  - sys_mkdir, sys_unlink, sys_readdir validate path pointers.
 *  - sys_spawn validates path and argv/envp pointers.
 *  - FIX: sys_mmap and sys_brk now use a spinlock to prevent race
 *    conditions when multiple processes call mmap/brk concurrently.
 *  - FIX: sys_writev now checks for integer overflow in total counter.
 *  - FIX: sys_mmap validates bump pointer does not overflow.
 *  - FIX: sys_brk limit raised to 256 MiB and protected by lock.
 *  - NEW: SYS_GETTIME_NS (27) — monotonic nanoseconds from PIT.
 *  - NEW: SYS_SETUID (28) / SYS_SETGID (29) — privilege-checked uid/gid.
 */

#include "syscall.h"
#include "macho.h"
#include "gdt.h"
#include "../net/net.h"
#include "../net/tls.h"
#include "../vfs/vfs.h"
#include "../ipc/ringbuf.h"
#include "../sched/sched.h"
#include "../usr/usr.h"
#include "../mm/mm.h"
#include "../console/fbcon.h"
#include "../drv/ps2kbd.h"
#include <stdint.h>
#include <stddef.h>

#define COM1_PORT 0x3F8

static inline void io_out8(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t io_in8(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static void console_putc(char c) {
    /* Match init serial_putc timeout so userland (fastfetch) appears in -serial file logs. */
    for (uint32_t spins = 0; spins < 1000000; spins++) {
        if ((io_in8(COM1_PORT + 5) & 0x20) != 0) {
            io_out8(COM1_PORT, (uint8_t)c);
            break;
        }
        __asm__ volatile("pause");
    }
    fb_console_putc(c);
}

void console_drain_rx(void) {
    /* QEMU -serial stdio loops TX back into RX; drain so editors/shell don't eat echo. */
    for (int n = 0; n < 4096; n++) {
        uint8_t lsr = io_in8(COM1_PORT + 5);
        if ((lsr & 0x01) == 0)
            break;
        if (lsr & 0x1E) {
            (void)io_in8(COM1_PORT);
            continue;
        }
        (void)io_in8(COM1_PORT);
    }
}

static int console_serial_getc_nb(char *out) {
    uint8_t lsr = io_in8(COM1_PORT + 5);
    if ((lsr & 0x01) == 0)
        return 0;
    if (lsr & 0x1E) {
        (void)io_in8(COM1_PORT);
        return 0;
    }
    uint8_t b = io_in8(COM1_PORT);
    if (b == 0)
        return 0;
    *out = (char)b;
    return 1;
}

static int console_getc_nb(char *out) {
    /* Keyboard before serial — matches init/io.c; avoids ANSI echo as keypresses. */
    if (ps2kbd_getc(out))
        return 1;
    return console_serial_getc_nb(out);
}

/* MSR numbers */
#define MSR_EFER          0xC0000080
#define MSR_STAR          0xC0000081
#define MSR_LSTAR         0xC0000082
#define MSR_FMASK         0xC0000084
#define MSR_KERNEL_GSBASE 0xC0000102

/* Per-CPU scratch structure accessed via GS after swapgs */
typedef struct {
    uint64_t user_rsp;
    uint64_t kernel_rsp;
} percpu_t;

static percpu_t g_percpu __attribute__((aligned(16)));

#define KSTACK_SIZE (64 * 1024)
static uint8_t g_kstack[KSTACK_SIZE] __attribute__((aligned(16)));

static inline void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile("wrmsr"
        :: "c"(msr), "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

extern void syscall_entry(void);

uint64_t syscall_kernel_rsp0(void) {
    return g_percpu.kernel_rsp > 16 ? g_percpu.kernel_rsp - 16 : g_percpu.kernel_rsp;
}

void syscall_on_thread_switch(pcb_t *pcb) {
    if (!pcb || !pcb->kstack_top)
        return;
    g_percpu.kernel_rsp = pcb->kstack_top;
    /* Restore KernelGSBase = &g_percpu after reap-jmp paths that skip SYSRET.
     * Normally SYSRET's swapgs restores this, but sched_exit takes a longjmp
     * shortcut that bypasses SYSRET, leaving KernelGSBase = 0 (user's GS).
     * The next SYSCALL entry's swapgs would then put 0 into GS.base, causing
     * a NULL write on "movq %rsp, %gs:0". */
    wrmsr(MSR_KERNEL_GSBASE, (uint64_t)(uintptr_t)&g_percpu);
    extern void tss_set_rsp0(uint64_t);
    tss_set_rsp0(pcb->kstack_top);
}

void syscall_init(void) {
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | 1);

    uint64_t star = ((uint64_t)SEG_USER_CODE32 << 48)
                  | ((uint64_t)SEG_KERNEL_CODE << 32);
    wrmsr(MSR_STAR, star);
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry);
    /* Clear IF, DF, AC on syscall entry */
    wrmsr(MSR_FMASK, (1U << 9) | (1U << 10) | (1U << 18));

    g_percpu.user_rsp   = 0;
    g_percpu.kernel_rsp = (uint64_t)(uintptr_t)(g_kstack + KSTACK_SIZE);
    wrmsr(MSR_KERNEL_GSBASE, (uint64_t)(uintptr_t)&g_percpu);

    extern void tss_set_rsp0(uint64_t);
    tss_set_rsp0(g_percpu.kernel_rsp);
}

/* ------------------------------------------------------------------ */
/* User memory access (current process vmap via PHYS_MAP_BASE)          */
/* ------------------------------------------------------------------ */

static int user_access_page(vaddr_t va, int for_write) {
    pcb_t *cur = sched_current();
    if (!cur || !cur->va_map)
        return -1;
    if (amm_virt_to_kva(cur->va_map, va & ~(PAGE_SIZE - 1ULL)))
        return 0;
    return amm_page_fault(cur->va_map, va, for_write);
}

static int copy_from_user(void *kdst, const void *usrc, size_t n) {
    if (!kdst || !usrc || n == 0)
        return 0;
    pcb_t *cur = sched_current();
    if (!cur || !cur->va_map)
        return -1;

    uint8_t       *dst = (uint8_t *)kdst;
    const uint8_t *src = (const uint8_t *)usrc;
    size_t done = 0;

    while (done < n) {
        vaddr_t va  = (vaddr_t)(uintptr_t)(src + done);
        vaddr_t pg  = va & ~(PAGE_SIZE - 1ULL);
        size_t  off = (size_t)(va & (PAGE_SIZE - 1ULL));
        size_t chunk = PAGE_SIZE - off;
        if (chunk > n - done)
            chunk = n - done;

        if (user_access_page(va, 0) != 0)
            return -1;
        void *kva = amm_virt_to_kva(cur->va_map, pg);
        if (!kva)
            return -1;

        const uint8_t *kp = (const uint8_t *)kva + off;
        for (size_t i = 0; i < chunk; i++)
            dst[done + i] = kp[i];
        done += chunk;
    }
    return 0;
}

static int copy_to_user(void *udst, const void *ksrc, size_t n) {
    if (!udst || !ksrc || n == 0)
        return 0;
    pcb_t *cur = sched_current();
    if (!cur || !cur->va_map)
        return -1;

    uint8_t       *dst = (uint8_t *)udst;
    const uint8_t *src = (const uint8_t *)ksrc;
    size_t done = 0;

    while (done < n) {
        vaddr_t va  = (vaddr_t)(uintptr_t)(dst + done);
        vaddr_t pg  = va & ~(PAGE_SIZE - 1ULL);
        size_t  off = (size_t)(va & (PAGE_SIZE - 1ULL));
        size_t chunk = PAGE_SIZE - off;
        if (chunk > n - done)
            chunk = n - done;

        if (user_access_page(va, 1) != 0)
            return -1;
        void *kva = amm_virt_to_kva(cur->va_map, pg);
        if (!kva)
            return -1;

        uint8_t *kp = (uint8_t *)kva + off;
        for (size_t i = 0; i < chunk; i++)
            kp[i] = src[done + i];
        done += chunk;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Pointer validation                                                   */
/* ------------------------------------------------------------------ */

/*
 * Validate that [ptr, ptr+size) is a valid user-space address range.
 * User memory must be below the canonical-hole boundary on x86-64.
 * Also rejects NULL.
 */
static int validate_user_ptr(const void *ptr, size_t size) {
    if (!ptr) return 0;
    uintptr_t addr = (uintptr_t)ptr;
    /* Canonical user space: 0x1000 .. 0x00007FFFFFFFFFFF */
    if (addr < 0x1000ULL) return 0;
    if (addr >= 0x0000800000000000ULL) return 0;
    /* Overflow check */
    if (size > 0 && addr + size < addr) return 0;
    if (size > 0 && addr + size > 0x0000800000000000ULL) return 0;
    return 1;
}

/*
 * Copy a NUL-terminated user string into a kernel buffer.
 * Returns 0 on success, -1 on fault or missing terminator.
 */
static int copy_user_cstr(char *kbuf, size_t kcap, const char *uptr, size_t max_u) {
    if (!kbuf || kcap == 0 || !validate_user_ptr(uptr, 1))
        return -1;
    size_t lim = kcap - 1;
    if (lim > max_u)
        lim = max_u;
    for (size_t i = 0; i < lim; i++) {
        char c;
        if (copy_from_user(&c, uptr + i, 1) != 0)
            return -1;
        kbuf[i] = c;
        if (c == '\0')
            return 0;
    }
    kbuf[lim] = '\0';
    return -1;
}

static int validate_user_str(const char *ptr, size_t max_len) {
    if (!validate_user_ptr(ptr, 1)) return 0;
    for (size_t i = 0; i < max_len; i++) {
        char c;
        if (copy_from_user(&c, ptr + i, 1) != 0) return 0;
        if (c == '\0') return 1;
    }
    return 0;
}

/* Validate argv/envp without dereferencing user pointers in kernel CR3. */
static int validate_user_str_array(const char **vec, size_t max_count,
                                   size_t max_str_len) {
    if (!vec) return 1;
    for (size_t i = 0; i < max_count; i++) {
        const char *s = NULL;
        if (!validate_user_ptr(&vec[i], sizeof(char *))) return 0;
        if (copy_from_user(&s, &vec[i], sizeof(s)) != 0) return 0;
        if (!s) return 1;
        if (!validate_user_str(s, max_str_len)) return 0;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Error codes                                                          */
/* ------------------------------------------------------------------ */

#define ESYS_FAULT    ((uint64_t)(int64_t)-14)   /* EFAULT */
#define ESYS_INVAL    ((uint64_t)(int64_t)-22)   /* EINVAL */
#define ESYS_PERM     ((uint64_t)(int64_t)-1)    /* EPERM  */
#define ESYS_NOSYS    ((uint64_t)(int64_t)-38)   /* ENOSYS */
#define ESYS_OVERFLOW ((uint64_t)(int64_t)-75)   /* EOVERFLOW */

/* ------------------------------------------------------------------ */
/* Syscall implementations                                              */
/* ------------------------------------------------------------------ */

static uint64_t sys_exit(uint64_t code) {
    sched_exit((int)code);
    /* unreachable */
    return 0;
}

/*
 * If `path` is relative (doesn't start with '/'), rewrite it in place as
 * "<calling-process-cwd>/<path>" so VFS's vfs_canonicalize() resolves it
 * against the process's working directory instead of always assuming root.
 * No-op for already-absolute paths or when there's no current process
 * (kernel-context callers pass NULL caller and already resolve paths
 * themselves before reaching here).
 */
static void resolve_with_cwd(char *path, size_t cap) {
    if (path[0] == '/' || path[0] == '\0') return;
    pcb_t *cur = sched_current();
    if (!cur || cur->cwd[0] != '/') return;

    char tmp[VFS_PATH_MAX];
    size_t cwd_len = 0;
    while (cwd_len < sizeof(tmp) - 1 && cur->cwd[cwd_len]) {
        tmp[cwd_len] = cur->cwd[cwd_len];
        cwd_len++;
    }
    size_t pos = cwd_len;
    if (pos == 0 || tmp[pos - 1] != '/') {
        if (pos < sizeof(tmp) - 1) tmp[pos++] = '/';
    }
    size_t i = 0;
    while (path[i] && pos < sizeof(tmp) - 1) tmp[pos++] = path[i++];
    tmp[pos] = '\0';

    size_t j = 0;
    while (j < cap - 1 && tmp[j]) { path[j] = tmp[j]; j++; }
    path[j] = '\0';
}

static uint64_t sys_open(uint64_t path_u, uint64_t flags,
                          uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    (void)a2; (void)a3; (void)a4; (void)a5;
    char path[VFS_PATH_MAX];
    if (copy_user_cstr(path, sizeof(path), (const char *)(uintptr_t)path_u,
                       VFS_PATH_MAX) != 0)
        return ESYS_FAULT;
    resolve_with_cwd(path, sizeof(path));
    pcb_t *cur = sched_current();
    fd_t fd = vfs_open(path, (int)flags, cur);
    return (uint64_t)(int64_t)(int)fd;
}

static uint64_t sys_close(uint64_t fd) {
    return (uint64_t)(int64_t)vfs_close((fd_t)fd);
}

static uint64_t sys_read(uint64_t fd, uint64_t buf_u, uint64_t n) {
    void *buf = (void *)(uintptr_t)buf_u;
    if (n == 0) return 0;
    if (!validate_user_ptr(buf, (size_t)n)) return ESYS_FAULT;

    /* fd=0 (stdin): check if redirected via fd_table[0], else keyboard */
    if (fd == 0) {
        pcb_t *_cur = sched_current();
        if (_cur && _cur->fd_table[0] != NULL) {
            /* stdin is redirected to a pipe or file — fall through to VFS path */
            goto vfs_read_path;
        }
        size_t got = 0;
        uint8_t kbuf[256];
        while (got < n) {
            char c;
            if (console_getc_nb(&c)) {
                kbuf[0] = (uint8_t)c;
                if (copy_to_user((uint8_t *)buf + got, kbuf, 1) != 0)
                    return ESYS_FAULT;
                got++;
                if (n == 1)
                    break;
                continue;
            }
            if (got > 0)
                break;
            fb_console_tick();
            __asm__ volatile("sti; hlt; cli" ::: "memory");
        }
        return got;
    }
    vfs_read_path:;

    /* VFS read: loop to satisfy large requests (kernel kbuf is 4096 bytes per
     * iteration, but callers like hx request up to 512KB in one syscall). */
    static uint8_t kbuf[4096];
    size_t total = 0;
    while (total < (size_t)n) {
        size_t chunk = (size_t)n - total;
        if (chunk > sizeof(kbuf))
            chunk = sizeof(kbuf);
        size_t got = 0;
        int r = vfs_read((fd_t)fd, kbuf, chunk, &got);
        if (r < 0) {
            if (total == 0) return (uint64_t)(int64_t)r;
            break;
        }
        if (got == 0) break; /* EOF */
        if (copy_to_user((uint8_t *)buf + total, kbuf, got) != 0)
            return ESYS_FAULT;
        total += got;
    }
    return (uint64_t)total;
}

static uint64_t sys_write(uint64_t fd, uint64_t buf_u, uint64_t n) {
    const void *buf = (const void *)(uintptr_t)buf_u;
    if (n == 0) return 0;
    if (!validate_user_ptr(buf, (size_t)n)) return ESYS_FAULT;

    if (fd == 1 || fd == 2) {
        /* Check if stdout/stderr is redirected via fd_table */
        pcb_t *_cur = sched_current();
        if (_cur && _cur->fd_table[fd] != NULL)
            goto vfs_write_path;  /* fall through to VFS path */
        uint8_t kbuf[256];
        size_t done = 0;
        while (done < n) {
            size_t chunk = n - done;
            if (chunk > sizeof(kbuf))
                chunk = sizeof(kbuf);
            if (copy_from_user(kbuf, (const uint8_t *)buf + done, chunk) != 0)
                return ESYS_FAULT;
            for (size_t i = 0; i < chunk; i++)
                console_putc((char)kbuf[i]);
            done += chunk;
        }
        console_drain_rx();
        return n;
    }
    if (fd == 0)
        return ESYS_INVAL;
    vfs_write_path:;

    /* VFS write: loop to handle large buffers (kbuf is 4096 per iteration). */
    static uint8_t kbuf[4096];
    size_t total = 0;
    while (total < (size_t)n) {
        size_t chunk = (size_t)n - total;
        if (chunk > sizeof(kbuf))
            chunk = sizeof(kbuf);
        if (copy_from_user(kbuf, (const uint8_t *)buf + total, chunk) != 0)
            return ESYS_FAULT;
        int r = vfs_write((fd_t)fd, kbuf, chunk);
        if (r < 0) {
            if (total == 0) return (uint64_t)(int64_t)r;
            break;
        }
        total += chunk;
    }
    return (uint64_t)total;
}

static uint64_t sys_stat(uint64_t path_u, uint64_t st_u) {
    char path[VFS_PATH_MAX];
    stat_info_t kst;
    if (!validate_user_ptr((void *)(uintptr_t)st_u, sizeof(stat_info_t))) return ESYS_FAULT;
    if (copy_user_cstr(path, sizeof(path), (const char *)(uintptr_t)path_u,
                       VFS_PATH_MAX) != 0)
        return ESYS_FAULT;
    resolve_with_cwd(path, sizeof(path));
    int r = vfs_stat(path, &kst);
    if (r != 0) return (uint64_t)(int64_t)r;
    if (copy_to_user((void *)(uintptr_t)st_u, &kst, sizeof(kst)) != 0)
        return ESYS_FAULT;
    return 0;
}

static uint64_t sys_seek(uint64_t fd, uint64_t off, uint64_t whence) {
    return (uint64_t)vfs_seek((fd_t)fd, (int64_t)off, (int)whence);
}

static uint64_t sys_mkdir(uint64_t path_u) {
    char path[VFS_PATH_MAX];
    if (copy_user_cstr(path, sizeof(path), (const char *)(uintptr_t)path_u,
                       VFS_PATH_MAX) != 0)
        return ESYS_FAULT;
    resolve_with_cwd(path, sizeof(path));
    return (uint64_t)(int64_t)vfs_mkdir(path);
}

static uint64_t sys_unlink(uint64_t path_u) {
    char path[VFS_PATH_MAX];
    if (copy_user_cstr(path, sizeof(path), (const char *)(uintptr_t)path_u,
                       VFS_PATH_MAX) != 0)
        return ESYS_FAULT;
    resolve_with_cwd(path, sizeof(path));
    return (uint64_t)(int64_t)vfs_unlink(path);
}

static uint64_t sys_readdir(uint64_t path_u, uint64_t buf_u,
                             uint64_t cap, uint64_t nout_u) {
    char path[VFS_PATH_MAX];
    static vfs_dirent_t kents[256];
    uint32_t n = 0;
    if (copy_user_cstr(path, sizeof(path), (const char *)(uintptr_t)path_u,
                       VFS_PATH_MAX) != 0)
        return ESYS_FAULT;
    resolve_with_cwd(path, sizeof(path));
    if (cap > 256) cap = 256;
    if (cap > 0 && !validate_user_ptr((void *)(uintptr_t)buf_u,
                                      (size_t)cap * sizeof(vfs_dirent_t)))
        return ESYS_FAULT;
    if (!validate_user_ptr((void *)(uintptr_t)nout_u, sizeof(uint32_t)))
        return ESYS_FAULT;
    int r = vfs_readdir(path, kents, (uint32_t)cap, &n);
    if (r != 0) return (uint64_t)(int64_t)r;
    if (n > 0 && copy_to_user((void *)(uintptr_t)buf_u, kents,
                              (size_t)n * sizeof(vfs_dirent_t)) != 0)
        return ESYS_FAULT;
    if (copy_to_user((void *)(uintptr_t)nout_u, &n, sizeof(n)) != 0)
        return ESYS_FAULT;
    return 0;
}

static uint64_t sys_getpid(void) {
    pcb_t *p = sched_current();
    return p ? (uint64_t)p->pid : 0;
}

static uint64_t sys_yield(void) {
    sched_yield();
    return 0;
}

static uint64_t sys_time(void) {
    extern uint64_t pit_ticks(void);
    return pit_ticks() * 10000000ULL;
}

static uint64_t sys_getuid(void) {
    pcb_t *p = sched_current();
    return p ? (uint64_t)p->uid : 0;
}

static uint64_t sys_getgid(void) {
    pcb_t *p = sched_current();
    return p ? (uint64_t)p->gid : 0;
}

static char g_spawn_argv_str[64][VFS_PATH_MAX];
static const char *g_spawn_argv_ptrs[64];
static char g_spawn_env_str[64][VFS_PATH_MAX];
static const char *g_spawn_env_ptrs[64];

static int copy_user_cstr_array(const char **uvec,
                                char (*kstr)[VFS_PATH_MAX],
                                const char **kvec) {
    if (!uvec) {
        kvec[0] = NULL;
        return 0;
    }
    for (int i = 0; i < 63; i++) {
        const char *us = NULL;
        if (!validate_user_ptr(&uvec[i], sizeof(char *)))
            return -1;
        if (copy_from_user(&us, &uvec[i], sizeof(us)) != 0)
            return -1;
        if (!us) {
            kvec[i] = NULL;
            return 0;
        }
        if (copy_user_cstr(kstr[i], VFS_PATH_MAX, us, VFS_PATH_MAX) != 0)
            return -1;
        kvec[i] = kstr[i];
    }
    kvec[63] = NULL;
    return 0;
}

static uint64_t sys_spawn(uint64_t path_u, uint64_t argv_u, uint64_t envp_u) {
    char kpath[VFS_PATH_MAX];
    const char **uargv = (const char **)(uintptr_t)argv_u;
    const char **uenvp = (const char **)(uintptr_t)envp_u;

    if (copy_user_cstr(kpath, sizeof(kpath), (const char *)(uintptr_t)path_u,
                       VFS_PATH_MAX) != 0)
        return ESYS_FAULT;
    if (!validate_user_str_array(uargv, 64, VFS_PATH_MAX)) return ESYS_FAULT;
    if (!validate_user_str_array(uenvp, 64, VFS_PATH_MAX)) return ESYS_FAULT;

    if (copy_user_cstr_array(uargv, g_spawn_argv_str, g_spawn_argv_ptrs) != 0)
        return ESYS_FAULT;
    if (copy_user_cstr_array(uenvp, g_spawn_env_str, g_spawn_env_ptrs) != 0)
        return ESYS_FAULT;

    uint32_t pid = macho_spawn(kpath, g_spawn_argv_ptrs,
                             uenvp ? g_spawn_env_ptrs : NULL);
    return pid ? (uint64_t)pid : (uint64_t)-1ULL;
}

static uint64_t sys_wait(uint64_t pid_u, uint64_t info_u) {
    exit_info_t kinfo;
    exit_info_t *out = info_u ? &kinfo : NULL;
    if (info_u && !validate_user_ptr((void *)(uintptr_t)info_u, sizeof(exit_info_t)))
        return ESYS_FAULT;
    int r = sched_reap((pid_t)pid_u, out);
    if (r == 0 && info_u && copy_to_user((void *)(uintptr_t)info_u, &kinfo, sizeof(kinfo)) != 0)
        return ESYS_FAULT;
    return (uint64_t)(int64_t)r;
}

/* ------------------------------------------------------------------ */
/* New syscalls for ABI completeness                                    */
/* ------------------------------------------------------------------ */

/* SYS_GETEUID — effective UID (same as real UID for now) */
static uint64_t sys_geteuid(void) {
    return sys_getuid();
}

/* SYS_GETEGID — effective GID */
static uint64_t sys_getegid(void) {
    return sys_getgid();
}

/* SYS_GETPPID — parent PID (returns 1 for all user processes for now) */
static uint64_t sys_getppid(void) {
    return 1;
}

/* SYS_UNAME — fill a 390-byte utsname-like buffer:
 *   sysname[65] nodename[65] release[65] version[65] machine[65] domainname[65]
 */
#define UTSNAME_LEN  65
typedef struct {
    char sysname[UTSNAME_LEN];
    char nodename[UTSNAME_LEN];
    char release[UTSNAME_LEN];
    char version[UTSNAME_LEN];
    char machine[UTSNAME_LEN];
    char domainname[UTSNAME_LEN];
} asd_utsname_t;

extern char g_hostname[64];   /* defined in asdinit.c */

static void str_copy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    while (i < n - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static uint64_t sys_uname(uint64_t buf_u) {
    asd_utsname_t ku;
    if (!validate_user_ptr((void *)(uintptr_t)buf_u, sizeof(asd_utsname_t))) return ESYS_FAULT;
    str_copy(ku.sysname,    "OpenASD",  UTSNAME_LEN);
    str_copy(ku.nodename,   g_hostname, UTSNAME_LEN);
    ku.nodename[UTSNAME_LEN - 1] = '\0';
    str_copy(ku.release,    "1.0",      UTSNAME_LEN);
    str_copy(ku.version,    "#1",       UTSNAME_LEN);
    str_copy(ku.machine,    "x86_64",   UTSNAME_LEN);
    str_copy(ku.domainname, "(none)",   UTSNAME_LEN);
    if (copy_to_user((void *)(uintptr_t)buf_u, &ku, sizeof(ku)) != 0)
        return ESYS_FAULT;
    return 0;
}

/* SYS_MMAP — minimal anonymous mmap for heap support.
 * Only MAP_ANONYMOUS|MAP_PRIVATE is supported.
 * addr hint is ignored; returns a fixed-increment address.
 *
 * FIX (v5): Added spinlock to prevent race condition when two processes
 * call sys_mmap concurrently and both read g_mmap_bump before either
 * updates it, resulting in aliased mappings.
 * FIX (v5): Added overflow check on g_mmap_bump + bytes.
 */
#define MMAP_BASE  0x0000500000000000ULL
#define MMAP_LIMIT 0x0000700000000000ULL  /* hard ceiling for mmap region */

static uint64_t sys_mmap(uint64_t addr, uint64_t len,
                          uint64_t prot, uint64_t flags,
                          uint64_t fd_u, uint64_t off) {
    (void)addr; (void)off;
    /* Only anonymous private mappings */
    if (!(flags & 0x20)) return (uint64_t)-1ULL;   /* MAP_ANONYMOUS = 0x20 */
    if ((int64_t)fd_u != -1 && fd_u != (uint64_t)-1ULL) return (uint64_t)-1ULL;
    if (len == 0) return (uint64_t)-1ULL;

    pcb_t *cur = sched_current();
    if (!cur || !cur->va_map) return ESYS_FAULT;

    /* Initialise per-process bump on first use */
    if (cur->mmap_bump == 0) cur->mmap_bump = MMAP_BASE;

    uint64_t pages = (len + 4095ULL) / 4096ULL;
    uint64_t bytes = pages * 4096ULL;
    int kprot = (int)(prot & (PROT_READ | PROT_WRITE | PROT_EXEC));
    if (kprot == PROT_NONE) kprot = PROT_READ | PROT_WRITE;

    if (cur->mmap_bump + bytes < cur->mmap_bump ||
        cur->mmap_bump + bytes > MMAP_LIMIT)
        return (uint64_t)-1ULL;  /* ENOMEM */
    uint64_t result = cur->mmap_bump;
    cur->mmap_bump += bytes;

    if (amm_region_add(cur->va_map, (vaddr_t)result, (size_t)bytes,
                       kprot, RGN_HEAP, -1) != 0)
        return (uint64_t)-1ULL;
    if (!amm_region_populate(cur->va_map, (vaddr_t)result, (size_t)bytes,
                             kprot))
        return (uint64_t)-1ULL;

    return result;
}

/* SYS_MUNMAP — no-op for now (memory is not reclaimed) */
static uint64_t sys_munmap(uint64_t addr, uint64_t len) {
    (void)addr; (void)len;
    return 0;
}

/* SYS_BRK — minimal brk() for malloc compatibility.
 *
 * FIX (v5): Added spinlock to prevent concurrent brk() calls from
 * corrupting g_brk_cur.  Limit raised to 256 MiB.
 */
#define BRK_BASE  0x0000600000000000ULL
#define BRK_LIMIT (256ULL * 1024 * 1024)  /* 256 MiB max heap */

static uint64_t sys_brk(uint64_t new_brk) {
    pcb_t *cur = sched_current();
    if (!cur || !cur->va_map) return 0;

    if (cur->brk_cur == 0) cur->brk_cur = BRK_BASE;
    if (new_brk == 0) return cur->brk_cur;
    if (new_brk < BRK_BASE) return cur->brk_cur;
    if (new_brk > BRK_BASE + BRK_LIMIT) return cur->brk_cur;

    if (!amm_region_find(cur->va_map, (vaddr_t)BRK_BASE)) {
        if (amm_region_add(cur->va_map, (vaddr_t)BRK_BASE, (size_t)BRK_LIMIT,
                           PROT_READ | PROT_WRITE, RGN_HEAP, -1) != 0)
            return cur->brk_cur;
    }

    if (new_brk > cur->brk_cur) {
        vaddr_t pop_base = (vaddr_t)PAGE_ALIGN_DOWN(cur->brk_cur);
        vaddr_t pop_end  = (vaddr_t)PAGE_ALIGN(new_brk);
        if (pop_end > pop_base &&
            !amm_region_populate(cur->va_map, pop_base,
                                 (size_t)(pop_end - pop_base),
                                 PROT_READ | PROT_WRITE))
            return cur->brk_cur;
    }

    cur->brk_cur = new_brk;
    return cur->brk_cur;
}

/* SYS_IOCTL — stub returning ENOTTY for terminal queries */
static uint64_t sys_ioctl(uint64_t fd, uint64_t req, uint64_t arg) {
    (void)fd; (void)req; (void)arg;
    return (uint64_t)(int64_t)-25;   /* ENOTTY */
}

/* SYS_WRITEV — scatter-write from iovec array.
 *
 * FIX (v5): Added integer overflow check on the running total.
 * Previously, if the sum of iov_len values exceeded UINT64_MAX the
 * counter would silently wrap, returning a bogus byte count.
 */
typedef struct {
    uint64_t iov_base;
    uint64_t iov_len;
} asd_iovec_t;

static uint64_t sys_writev(uint64_t fd, uint64_t iov_u, uint64_t iovcnt) {
    if (iovcnt == 0) return 0;
    if (iovcnt > 16) return ESYS_INVAL;
    const asd_iovec_t *iov = (const asd_iovec_t *)(uintptr_t)iov_u;
    if (!validate_user_ptr(iov, (size_t)iovcnt * sizeof(asd_iovec_t)))
        return ESYS_FAULT;
    asd_iovec_t kiov[16];
    if (copy_from_user(kiov, iov, (size_t)iovcnt * sizeof(asd_iovec_t)) != 0)
        return ESYS_FAULT;

    uint64_t total = 0;
    uint8_t kbuf[4096];
    for (uint64_t i = 0; i < iovcnt; i++) {
        if (kiov[i].iov_len == 0) continue;
        const void *buf = (const void *)(uintptr_t)kiov[i].iov_base;
        if (!validate_user_ptr(buf, (size_t)kiov[i].iov_len)) return ESYS_FAULT;
        if (total + kiov[i].iov_len < total) return ESYS_OVERFLOW;

        size_t left = (size_t)kiov[i].iov_len;
        size_t off  = 0;
        while (left > 0) {
            size_t chunk = left;
            if (chunk > sizeof(kbuf))
                chunk = sizeof(kbuf);
            if (copy_from_user(kbuf, (const uint8_t *)buf + off, chunk) != 0)
                return ESYS_FAULT;
            if (fd == 1 || fd == 2) {
                for (size_t j = 0; j < chunk; j++)
                    console_putc((char)kbuf[j]);
            } else {
                int r = vfs_write((fd_t)fd, kbuf, chunk);
                if (r < 0) return (uint64_t)(int64_t)r;
            }
            off  += chunk;
            left -= chunk;
        }
        total += kiov[i].iov_len;
    }
    if (fd == 1 || fd == 2)
        console_drain_rx();
    return total;
}

/* ------------------------------------------------------------------ */
/* SYS_GETTIME_NS (27) — monotonic nanoseconds since boot.             */
/* NEW (v5): Higher-resolution time than SYS_TIME.                     */
/* ------------------------------------------------------------------ */
static uint64_t sys_gettime_ns(void) {
    extern uint64_t pit_ticks(void);
    return pit_ticks() * 10000000ULL;
}

/* ------------------------------------------------------------------ */
/* SYS_SETUID (28) — change real UID.                                  */
/* NEW (v5): Only root (uid 0) may set an arbitrary uid.               */
/* ------------------------------------------------------------------ */
static uint64_t sys_setuid(uint64_t new_uid) {
    pcb_t *p = sched_current();
    if (!p) return ESYS_FAULT;
    if (p->uid != 0 && (uint32_t)new_uid != p->uid) {
        /* Allow wheel users to become root */
        if ((uint32_t)new_uid == 0) {
            asd_user_t *u = usr_find_by_uid(p->uid);
            if (!u || !(u->flags & USR_FLAG_WHEEL))
                return ESYS_PERM;
        } else {
            return ESYS_PERM;
        }
    }
    p->uid = (uint32_t)new_uid;
    return 0;
}

/* ------------------------------------------------------------------ */
/* SYS_SETGID (29) — change real GID.                                  */
/* NEW (v5): Same privilege model as SYS_SETUID.                       */
/* ------------------------------------------------------------------ */
static uint64_t sys_setgid(uint64_t new_gid) {
    pcb_t *p = sched_current();
    if (!p) return ESYS_FAULT;
    if (p->uid != 0 && (uint32_t)new_gid != p->gid)
        return ESYS_PERM;
    p->gid = (uint32_t)new_gid;
    return 0;
}

/* ------------------------------------------------------------------ */
/* SYS_NET_SEND (30) — send UDP datagram                               */
/* a0 = dst_ip (uint32), a1 = dst_port (uint16), a2 = src_port,       */
/* a3 = user buf ptr, a4 = len                                         */
/* ------------------------------------------------------------------ */
static uint64_t sys_net_send(uint64_t dst_ip, uint64_t dst_port,
                             uint64_t src_port, uint64_t buf_u, uint64_t len) {
    if (len == 0 || len > 1472) return ESYS_INVAL;
    if (!validate_user_ptr((const void *)(uintptr_t)buf_u, (size_t)len))
        return ESYS_FAULT;
    uint8_t kbuf[1472];
    if (copy_from_user(kbuf, (const void *)(uintptr_t)buf_u, (size_t)len) != 0)
        return ESYS_FAULT;
    int r = net_udp_send((uint32_t)dst_ip, (uint16_t)src_port,
                         (uint16_t)dst_port, kbuf, (uint16_t)len);
    return r == 0 ? (uint64_t)len : (uint64_t)(int64_t)-1LL;
}

/* ------------------------------------------------------------------ */
/* SYS_NET_RECV (31) — receive UDP datagram                            */
/* a0 = user buf ptr, a1 = buf_sz, a2 = src_ip_ptr, a3 = src_port_ptr */
/* ------------------------------------------------------------------ */
static uint64_t sys_net_recv(uint64_t buf_u, uint64_t buf_sz,
                             uint64_t src_ip_u, uint64_t src_port_u) {
    if (buf_sz == 0 || buf_sz > 1472) return ESYS_INVAL;
    if (!validate_user_ptr((void *)(uintptr_t)buf_u, (size_t)buf_sz))
        return ESYS_FAULT;
    uint8_t kbuf[1472];
    uint32_t src_ip = 0;
    uint16_t src_port = 0;
    int n = net_udp_recv(kbuf, (uint16_t)buf_sz, &src_ip, &src_port);
    if (n < 0) return (uint64_t)(int64_t)-1LL;
    if (copy_to_user((void *)(uintptr_t)buf_u, kbuf, (size_t)n) != 0)
        return ESYS_FAULT;
    if (src_ip_u && validate_user_ptr((void *)(uintptr_t)src_ip_u, sizeof(uint32_t)))
        copy_to_user((void *)(uintptr_t)src_ip_u, &src_ip, sizeof(uint32_t));
    if (src_port_u && validate_user_ptr((void *)(uintptr_t)src_port_u, sizeof(uint16_t)))
        copy_to_user((void *)(uintptr_t)src_port_u, &src_port, sizeof(uint16_t));
    return (uint64_t)n;
}

/* ------------------------------------------------------------------ */
/* SYS_TCP_CONNECT (40) — open TCP connection.                         */
/* a0=dst_ip (host order), a1=dst_port, a2=ptr to int conn_id         */
/* ------------------------------------------------------------------ */
static uint64_t sys_tcp_connect(uint64_t ip, uint64_t port, uint64_t id_u) {
    if (!validate_user_ptr((void *)(uintptr_t)id_u, sizeof(int))) return ESYS_FAULT;
    int id = -1;
    int r = net_tcp_connect((uint32_t)ip, (uint16_t)port, &id);
    if (r != 0) return (uint64_t)(int64_t)-1LL;
    if (copy_to_user((void *)(uintptr_t)id_u, &id, sizeof(id)) != 0) return ESYS_FAULT;
    return 0;
}

/* ------------------------------------------------------------------ */
/* SYS_TCP_SEND (41) — send data on TCP connection.                    */
/* a0=id, a1=buf, a2=len                                               */
/* ------------------------------------------------------------------ */
static uint64_t sys_tcp_send(uint64_t id, uint64_t buf_u, uint64_t len) {
    if (len == 0) return 0;
    if (!validate_user_ptr((void *)(uintptr_t)buf_u, (size_t)len)) return ESYS_FAULT;
    static uint8_t kbuf[4096];
    size_t done = 0;
    while (done < (size_t)len) {
        size_t chunk = (size_t)len - done;
        if (chunk > sizeof(kbuf)) chunk = sizeof(kbuf);
        if (copy_from_user(kbuf, (const uint8_t *)(uintptr_t)buf_u + done, chunk) != 0)
            return ESYS_FAULT;
        int r = net_tcp_send((int)id, kbuf, (uint32_t)chunk);
        if (r < 0) return (uint64_t)(int64_t)(done > 0 ? (long)done : -1LL);
        done += chunk;
    }
    return (uint64_t)done;
}

/* ------------------------------------------------------------------ */
/* SYS_TCP_RECV (42) — receive data from TCP connection.               */
/* a0=id, a1=buf, a2=cap, a3=blocking                                  */
/* ------------------------------------------------------------------ */
static uint64_t sys_tcp_recv(uint64_t id, uint64_t buf_u,
                              uint64_t cap, uint64_t blocking) {
    if (cap == 0) return 0;
    if (!validate_user_ptr((void *)(uintptr_t)buf_u, (size_t)cap)) return ESYS_FAULT;
    static uint8_t kbuf[4096];
    size_t total = 0;
    while (total < (size_t)cap) {
        size_t chunk = (size_t)cap - total;
        if (chunk > sizeof(kbuf)) chunk = sizeof(kbuf);
        int r = net_tcp_recv((int)id, kbuf, (uint32_t)chunk, (int)blocking);
        if (r < 0) return (uint64_t)(int64_t)(total > 0 ? (long)total : -1LL);
        if (r == 0) break; /* EOF */
        if (copy_to_user((uint8_t *)(uintptr_t)buf_u + total, kbuf, (size_t)r) != 0)
            return ESYS_FAULT;
        total += (size_t)r;
        /* Non-blocking: return whatever we got immediately */
        if (!blocking) break;
    }
    return (uint64_t)total;
}

/* ------------------------------------------------------------------ */
/* SYS_TCP_CLOSE (43)                                                  */
/* ------------------------------------------------------------------ */
static uint64_t sys_tcp_close(uint64_t id) {
    net_tcp_close((int)id);
    return 0;
}

/* ------------------------------------------------------------------ */
/* SYS_DNS_RESOLVE (44) — hostname → IPv4 (host byte order).          */
/* a0=hostname ptr, a1=ip_out ptr                                       */
/* ------------------------------------------------------------------ */
static uint64_t sys_dns_resolve(uint64_t host_u, uint64_t ip_u) {
    char hostname[256];
    if (copy_user_cstr(hostname, sizeof(hostname),
                       (const char *)(uintptr_t)host_u, 256) != 0)
        return ESYS_FAULT;
    if (!validate_user_ptr((void *)(uintptr_t)ip_u, sizeof(uint32_t)))
        return ESYS_FAULT;
    uint32_t ip = 0;
    int r = net_dns_resolve(hostname, &ip);
    if (r != 0) return (uint64_t)(int64_t)-1LL;
    if (copy_to_user((void *)(uintptr_t)ip_u, &ip, sizeof(ip)) != 0) return ESYS_FAULT;
    return 0;
}

/* ------------------------------------------------------------------ */
/* SYS_KILL (35) — send signal to process.                             */
/* ------------------------------------------------------------------ */
static uint64_t sys_kill(uint64_t pid_u, uint64_t sig_u) {
    pid_t  target_pid = (pid_t)pid_u;
    uint32_t sig      = (uint32_t)sig_u;
    if (sig > 31) return ESYS_INVAL;

    pcb_t *target = sched_find(target_pid);
    if (!target || target->state == PROC_DEAD) return ESYS_INVAL;

    __atomic_fetch_or(&target->sig_pending, 1u << sig, __ATOMIC_RELEASE);
    return 0;
}

/* ------------------------------------------------------------------ */
/* SYS_PIPE (38) — create anonymous pipe, return [read_fd, write_fd].  */
/* ------------------------------------------------------------------ */
static uint64_t sys_pipe(uint64_t fds_u) {
    if (!validate_user_ptr((void *)(uintptr_t)fds_u, 8)) return ESYS_FAULT;
    int rfd = -1, wfd = -1;
    int r = vfs_pipe(&rfd, &wfd);
    if (r != 0) return ESYS_INVAL;
    int32_t result[2] = { (int32_t)rfd, (int32_t)wfd };
    if (copy_to_user((void *)(uintptr_t)fds_u, result, 8) != 0) return ESYS_FAULT;
    return 0;
}

/* ------------------------------------------------------------------ */
/* SYS_DUP2 (39) — duplicate file descriptor (also 0/1/2 for stdio).  */
/* ------------------------------------------------------------------ */
static uint64_t sys_dup2(uint64_t old_u, uint64_t new_u) {
    int r = vfs_dup2((fd_t)old_u, (fd_t)new_u);
    if (r < 0) return ESYS_INVAL;
    return (uint64_t)(int64_t)r;
}

/* ------------------------------------------------------------------ */
/* SYS_CHDIR (33) / SYS_GETCWD (34) — per-process working directory.    */
/* ------------------------------------------------------------------ */
static uint64_t sys_chdir(uint64_t path_u) {
    char path[VFS_PATH_MAX];
    if (copy_user_cstr(path, sizeof(path), (const char *)(uintptr_t)path_u,
                       VFS_PATH_MAX) != 0)
        return ESYS_FAULT;
    resolve_with_cwd(path, sizeof(path));

    char canon[VFS_PATH_MAX];
    if (vfs_canonicalize(path, canon, sizeof(canon)) != 0)
        return ESYS_INVAL;

    /* "/" is always a valid directory — some backends don't expose it via stat */
    if (!(canon[0] == '/' && canon[1] == '\0')) {
        stat_info_t st;
        if (vfs_stat(canon, &st) != 0) return (uint64_t)(int64_t)VFS_ENOENT;
        if (st.kind != VFS_NODE_DIR) return (uint64_t)(int64_t)VFS_ENOTDIR;
    }

    pcb_t *cur = sched_current();
    if (!cur) return ESYS_FAULT;
    size_t i = 0;
    while (i < sizeof(cur->cwd) - 1 && canon[i]) { cur->cwd[i] = canon[i]; i++; }
    cur->cwd[i] = '\0';
    return 0;
}

static uint64_t sys_getcwd_impl(uint64_t buf_u, uint64_t size) {
    if (size == 0) return ESYS_INVAL;
    if (!validate_user_ptr((void *)(uintptr_t)buf_u, (size_t)size)) return ESYS_FAULT;
    pcb_t *cur = sched_current();
    const char *cwd = (cur && cur->cwd[0] == '/') ? cur->cwd : "/";
    size_t len = 0;
    while (cwd[len]) len++;
    size_t n = len < size - 1 ? len : size - 1;
    if (copy_to_user((void *)(uintptr_t)buf_u, cwd, n) != 0) return ESYS_FAULT;
    uint8_t zero = 0;
    if (copy_to_user((uint8_t *)(uintptr_t)buf_u + n, &zero, 1) != 0) return ESYS_FAULT;
    return (uint64_t)n;
}

/* ------------------------------------------------------------------ */
/* Dispatch table                                                       */
/* ------------------------------------------------------------------ */

uint64_t syscall_dispatch(uint64_t nr,
                          uint64_t a0, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4) {
    switch (nr) {
    case SYS_EXIT:     return sys_exit(a0);
    case SYS_OPEN:     return sys_open(a0, a1, a2, a3, a4, 0);
    case SYS_CLOSE:    return sys_close(a0);
    case SYS_READ:     return sys_read(a0, a1, a2);
    case SYS_WRITE:    return sys_write(a0, a1, a2);
    case SYS_STAT:     return sys_stat(a0, a1);
    case SYS_SEEK:     return sys_seek(a0, a1, a2);
    case SYS_MKDIR:    return sys_mkdir(a0);
    case SYS_UNLINK:   return sys_unlink(a0);
    case SYS_READDIR:  return sys_readdir(a0, a1, a2, a3);
    case SYS_SPAWN:    return sys_spawn(a0, a1, a2);
    case SYS_WAIT:     return sys_wait(a0, a1);
    case SYS_GETPID:   return sys_getpid();
    case SYS_YIELD:    return sys_yield();
    case SYS_TIME:     return sys_time();
    case SYS_GETUID:   return sys_getuid();
    case SYS_GETGID:   return sys_getgid();
    /* New ABI syscalls */
    case SYS_GETEUID:  return sys_geteuid();
    case SYS_GETEGID:  return sys_getegid();
    case SYS_GETPPID:  return sys_getppid();
    case SYS_UNAME:    return sys_uname(a0);
    case SYS_MMAP:     return sys_mmap(a0, a1, a2, a3, a4, 0);
    case SYS_MUNMAP:   return sys_munmap(a0, a1);
    case SYS_BRK:      return sys_brk(a0);
    case SYS_IOCTL:    return sys_ioctl(a0, a1, a2);
    case SYS_WRITEV:     return sys_writev(a0, a1, a2);
    case SYS_GETTIME_NS: return sys_gettime_ns();
    case SYS_SETUID:     return sys_setuid(a0);
    case SYS_SETGID:     return sys_setgid(a0);
    case SYS_NET_SEND:   return sys_net_send(a0, a1, a2, a3, a4);
    case SYS_NET_RECV:   return sys_net_recv(a0, a1, a2, a3);
    case SYS_NET_PING: {
        /* a0 = dst_ip (host byte order uint32), a1 = ptr to uint32 rtt_ms */
        uint32_t rtt = 0;
        int r = net_icmp_ping((uint32_t)a0, &rtt);
        if (r == 0 && a1 && validate_user_ptr((void *)(uintptr_t)a1, sizeof(uint32_t)))
            copy_to_user((void *)(uintptr_t)a1, &rtt, sizeof(rtt));
        return (uint64_t)(int64_t)r;
    }
    case SYS_CHDIR:        return sys_chdir(a0);
    case SYS_GETCWD:       return sys_getcwd_impl(a0, a1);
    case SYS_KILL:         return sys_kill(a0, a1);
    case SYS_PIPE:         return sys_pipe(a0);
    case SYS_DUP2:         return sys_dup2(a0, a1);
    case SYS_TCP_CONNECT:  return sys_tcp_connect(a0, a1, a2);
    case SYS_TCP_SEND:     return sys_tcp_send(a0, a1, a2);
    case SYS_TCP_RECV:     return sys_tcp_recv(a0, a1, a2, a3);
    case SYS_TCP_CLOSE:    return sys_tcp_close(a0);
    case SYS_DNS_RESOLVE:  return sys_dns_resolve(a0, a1);
    case SYS_TLS_CONNECT: {
        char host[256];
        if(copy_user_cstr(host,sizeof(host),(const char*)(uintptr_t)a1,256)!=0) return ESYS_FAULT;
        int r=net_tls_connect((int)a0,host);
        return (uint64_t)(int64_t)r;
    }
    case SYS_TLS_SEND: {
        if(a2==0) return 0;
        if(!validate_user_ptr((void*)(uintptr_t)a1,(size_t)a2)) return ESYS_FAULT;
        static uint8_t kbuf[4096];
        size_t done=0;
        while(done<(size_t)a2){
            size_t chunk=(size_t)a2-done;
            if(chunk>sizeof(kbuf))chunk=sizeof(kbuf);
            if(copy_from_user(kbuf,(const uint8_t*)(uintptr_t)a1+done,chunk)!=0) return ESYS_FAULT;
            int r=net_tls_send((int)a0,kbuf,(uint32_t)chunk);
            if(r<0) return (uint64_t)(int64_t)(done>0?(long)done:-1LL);
            done+=chunk;
        }
        return (uint64_t)done;
    }
    case SYS_TLS_RECV: {
        if(a2==0) return 0;
        if(!validate_user_ptr((void*)(uintptr_t)a1,(size_t)a2)) return ESYS_FAULT;
        static uint8_t kbuf[4096];
        size_t chunk=a2<sizeof(kbuf)?a2:sizeof(kbuf);
        int r=net_tls_recv((int)a0,kbuf,(uint32_t)chunk,(int)a3);
        if(r<0) return (uint64_t)(int64_t)-1LL;
        if(r==0) return 0;
        if(copy_to_user((void*)(uintptr_t)a1,kbuf,(size_t)r)!=0) return ESYS_FAULT;
        return (uint64_t)r;
    }
    case SYS_TLS_CLOSE:  net_tls_close((int)a0); return 0;
    case SYS_MEMINFO: {
        if (!validate_user_ptr((void*)(uintptr_t)a0, 8)) return ESYS_FAULT;
        if (!validate_user_ptr((void*)(uintptr_t)a1, 8)) return ESYS_FAULT;
        uint64_t total = 0, free_b = 0;
        mm_get_meminfo(&total, &free_b);
        uint64_t total_kb = total / 1024;
        uint64_t used_kb  = (total - free_b) / 1024;
        if (copy_to_user((void*)(uintptr_t)a0, &total_kb, 8) != 0) return ESYS_FAULT;
        if (copy_to_user((void*)(uintptr_t)a1, &used_kb,  8) != 0) return ESYS_FAULT;
        return 0;
    }
    case SYS_GETUSERNAME: {
        void *ubuf = (void *)(uintptr_t)a0;
        size_t usize = (size_t)a1;
        if (usize == 0) return 0;
        if (!validate_user_ptr(ubuf, usize)) return ESYS_FAULT;
        pcb_t *cur_p = sched_current();
        uint32_t uid = cur_p ? cur_p->uid : 0;
        const char *uname = NULL;
        if (uid == 0) {
            uname = "root";
        } else {
            asd_user_t *u = usr_find_by_uid(uid);
            if (u) uname = u->name;
        }
        if (!uname) uname = "user";
        static char kname[64];
        size_t i = 0;
        while (uname[i] && i < sizeof(kname) - 1) { kname[i] = uname[i]; i++; }
        kname[i] = '\0';
        size_t copy_len = i + 1 < usize ? i + 1 : usize;
        kname[copy_len - 1] = '\0';
        if (copy_to_user(ubuf, kname, copy_len) != 0) return ESYS_FAULT;
        return 0;
    }
    default:               return ESYS_NOSYS;
    }
}

/* Deliver pending signals after each syscall — called by syscall_entry.S
 * trampoline or inline after syscall_dispatch returns. */
void syscall_check_signals(void) {
    pcb_t *cur = sched_current();
    if (!cur || !cur->sig_pending) return;

    uint32_t pending = __atomic_exchange_n(&cur->sig_pending, 0, __ATOMIC_ACQUIRE);

    /* SIGKILL and SIGTERM: terminate the process */
    if (pending & ((1u << SIGKILL) | (1u << SIGTERM))) {
        int code = (pending & (1u << SIGKILL)) ? -9 : -15;
        sched_exit(code);
        /* unreachable */
    }
    /* SIGCHLD: child exited — just clear it (shell polls via wait) */
    /* Other signals: ignored for now */
}

int execve(const char *path, char *const argv[]) {
    (void)path; (void)argv;
    return -1;
}
