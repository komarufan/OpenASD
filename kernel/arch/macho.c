/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, ASD Project Contributors
 *
 * Mach-O 64-bit loader — maps LC_SEGMENT_64 segments into a user vmap
 * and returns the entry point from LC_UNIXTHREAD or LC_MAIN.
 *
 * Supported load commands:
 *   LC_SEGMENT_64   — map file data into virtual memory
 *   LC_UNIXTHREAD   — x86_64 thread state entry point (legacy static bins)
 *   LC_MAIN         — entry point offset from __TEXT (modern clang output)
 */

#include "macho.h"
#include "../vfs/vfs.h"
#include "../sched/sched.h"
#include "../include/string.h"
#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Mach-O constants                                                    */
/* ------------------------------------------------------------------ */

#define MH_MAGIC_64     0xFEEDFACFU
#define MH_EXECUTE      0x2

#define LC_SEGMENT_64   0x19U
#define LC_UNIXTHREAD   0x5U
#define LC_MAIN         0x80000028U

#define VM_PROT_READ    0x1
#define VM_PROT_WRITE   0x2
#define VM_PROT_EXECUTE 0x4

/* ------------------------------------------------------------------ */
/* Mach-O structures                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t magic;
    uint32_t cputype;
    uint32_t cpusubtype;
    uint32_t filetype;
    uint32_t ncmds;
    uint32_t sizeofcmds;
    uint32_t flags;
    uint32_t reserved;
} __attribute__((packed)) mach_header_64_t;

typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
} __attribute__((packed)) load_command_t;

typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    char     segname[16];
    uint64_t vmaddr;
    uint64_t vmsize;
    uint64_t fileoff;
    uint64_t filesize;
    uint32_t maxprot;
    uint32_t initprot;
    uint32_t nsects;
    uint32_t flags;
} __attribute__((packed)) segment_command_64_t;

#define x86_THREAD_STATE64 4

typedef struct {
    uint64_t rax, rbx, rcx, rdx, rdi, rsi, rbp, rsp;
    uint64_t r8,  r9,  r10, r11, r12, r13, r14, r15;
    uint64_t rip, rflags, cs, fs, gs;
} __attribute__((packed)) x86_thread_state64_t;

typedef struct {
    uint32_t flavor;
    uint32_t count;
    x86_thread_state64_t state;
} __attribute__((packed)) thread_state_hdr_t;

typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
} __attribute__((packed)) thread_command_t;

typedef struct {
    uint32_t cmd;
    uint32_t cmdsize;
    uint64_t entryoff;
    uint64_t stacksize;
} __attribute__((packed)) entry_point_command_t;

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

#define PAGE_SIZE_M       0x1000ULL
#define PG_DOWN(x)  ((x) & ~(PAGE_SIZE_M - 1ULL))
#define PG_UP(x)    (((x) + PAGE_SIZE_M - 1ULL) & ~(PAGE_SIZE_M - 1ULL))

#define USER_STACK_SIZE_M  (2UL * 1024 * 1024)
#define USER_STACK_TOP_M   0x00007FFF00000000ULL

static int vm_copy_m(vmap_t *vm, vaddr_t dst, const void *src, size_t len) {
    const uint8_t *s = (const uint8_t *)src;
    while (len > 0) {
        size_t off   = (size_t)(dst & (PAGE_SIZE_M - 1ULL));
        size_t chunk = (size_t)(PAGE_SIZE_M - off);
        if (chunk > len) chunk = len;
        void *kva = amm_virt_to_kva(vm, dst & ~(PAGE_SIZE_M - 1ULL));
        if (!kva) return -1;
        memcpy((uint8_t *)kva + off, s, chunk);
        dst += chunk; s += chunk; len -= chunk;
    }
    return 0;
}

static int vm_zero_m(vmap_t *vm, vaddr_t dst, size_t len) {
    while (len > 0) {
        size_t off   = (size_t)(dst & (PAGE_SIZE_M - 1ULL));
        size_t chunk = (size_t)(PAGE_SIZE_M - off);
        if (chunk > len) chunk = len;
        void *kva = amm_virt_to_kva(vm, dst & ~(PAGE_SIZE_M - 1ULL));
        if (!kva) return -1;
        memset((uint8_t *)kva + off, 0, chunk);
        dst += chunk; len -= chunk;
    }
    return 0;
}

static int count_strs_m(const char **a) {
    if (!a) return 0; int n = 0; while (a[n]) n++; return n;
}
static size_t slen_m(const char *s) {
    if (!s) return 0; size_t n = 0; while (s[n]) n++; return n;
}

static int push_bytes_m(vmap_t *vm, uint64_t *sp, const void *src, size_t len) {
    *sp -= (uint64_t)len;
    return vm_copy_m(vm, (vaddr_t)*sp, src, len);
}
static int push_u64_m(vmap_t *vm, uint64_t *sp, uint64_t v) {
    return push_bytes_m(vm, sp, &v, 8);
}

static uint64_t build_stack_macho(const char **argv, const char **envp, vmap_t *vm) {
    int argc = count_strs_m(argv);
    int envc = count_strs_m(envp);
    uint64_t sp = USER_STACK_TOP_M;

    uint64_t ep[64]; int ec = (envc > 63) ? 63 : envc;
    for (int i = ec - 1; i >= 0; i--) {
        if (push_bytes_m(vm, &sp, envp[i], slen_m(envp[i]) + 1) != 0) return 0;
        ep[i] = sp;
    }
    uint64_t ap[64]; int ac = (argc > 63) ? 63 : argc;
    for (int i = ac - 1; i >= 0; i--) {
        if (push_bytes_m(vm, &sp, argv[i], slen_m(argv[i]) + 1) != 0) return 0;
        ap[i] = sp;
    }
    sp &= ~15ULL;
    if ((ac + ec + 3) % 2 != 0)
        if (push_u64_m(vm, &sp, 0) != 0) return 0;
    if (push_u64_m(vm, &sp, 0) != 0) return 0;
    for (int i = ec - 1; i >= 0; i--)
        if (push_u64_m(vm, &sp, ep[i]) != 0) return 0;
    if (push_u64_m(vm, &sp, 0) != 0) return 0;
    for (int i = ac - 1; i >= 0; i--)
        if (push_u64_m(vm, &sp, ap[i]) != 0) return 0;
    if (push_u64_m(vm, &sp, (uint64_t)ac) != 0) return 0;
    return sp;
}

/* ------------------------------------------------------------------ */
/* Loader                                                              */
/* ------------------------------------------------------------------ */

uint64_t macho_load(const void *image, size_t image_size, vmap_t *vm) {
    if (!image || image_size < sizeof(mach_header_64_t)) return 0;
    const mach_header_64_t *mh = (const mach_header_64_t *)image;
    if (mh->magic != MH_MAGIC_64)   return 0;
    if (mh->filetype != MH_EXECUTE) return 0;
    if ((uint64_t)sizeof(mach_header_64_t) + mh->sizeofcmds > image_size) return 0;

    const uint8_t *lc_base = (const uint8_t *)image + sizeof(mach_header_64_t);
    uint64_t entry       = 0;
    uint64_t text_vmaddr = 0;
    int      got_text    = 0;
    uint32_t off         = 0;

    for (uint32_t i = 0; i < mh->ncmds; i++) {
        if (off + sizeof(load_command_t) > mh->sizeofcmds) return 0;
        const load_command_t *lc = (const load_command_t *)(lc_base + off);
        if (lc->cmdsize < sizeof(load_command_t)) return 0;
        if (off + lc->cmdsize > mh->sizeofcmds)  return 0;

        if (lc->cmd == LC_SEGMENT_64) {
            if (lc->cmdsize < sizeof(segment_command_64_t)) { off += lc->cmdsize; continue; }
            const segment_command_64_t *seg = (const segment_command_64_t *)lc;

            if (seg->vmsize == 0)                          { off += lc->cmdsize; continue; }
            if (seg->vmaddr == 0 && seg->filesize == 0)    { off += lc->cmdsize; continue; }
            if (seg->vmaddr >= 0x0000800000000000ULL)        return 0;
            if (seg->fileoff > image_size)                   return 0;
            if (seg->filesize > image_size - seg->fileoff)   return 0;

            int prot = 0;
            if (seg->initprot & VM_PROT_READ)    prot |= PROT_READ;
            if (seg->initprot & VM_PROT_WRITE)   prot |= PROT_WRITE;
            if (seg->initprot & VM_PROT_EXECUTE) prot |= PROT_EXEC;

            uint64_t seg_start = PG_DOWN(seg->vmaddr);
            uint64_t seg_end   = PG_UP(seg->vmaddr + seg->vmsize);
            size_t   seg_sz    = (size_t)(seg_end - seg_start);
            if (seg_sz == 0 || seg_sz > (64UL * 1024 * 1024)) return 0;

            if (amm_region_add(vm, (vaddr_t)seg_start, seg_sz, prot, RGN_FILE, -1) != 0) return 0;

            /* Populate + copy ONLY the file-backed pages.  The zero-fill tail
             * (vmsize > filesize) is left to demand-page via the #PF handler
             * (amm_page_fault zero-fills on touch).  Eagerly populating the
             * whole segment fails for multi-MB zero-fill regions — e.g. hx's
             * 2.6 MB static editor buffer — under memory pressure, which showed
             * up as "macho_spawn: invalid Mach-O". */
            if (seg->filesize > 0) {
                uint64_t file_end     = PG_UP(seg->vmaddr + seg->filesize);
                size_t   file_pages   = (size_t)(file_end - seg_start);
                if (!amm_region_populate(vm, (vaddr_t)seg_start, file_pages, prot | PROT_WRITE)) return 0;
                if (vm_copy_m(vm, (vaddr_t)seg->vmaddr,
                              (const uint8_t *)image + seg->fileoff,
                              (size_t)seg->filesize) != 0) return 0;
            }
            /* BSS / zero-fill tail demand-pages lazily — nothing to do here. */

            if (!got_text && (seg->initprot & VM_PROT_EXECUTE)) {
                text_vmaddr = seg->vmaddr;
                got_text = 1;
            }

        } else if (lc->cmd == LC_UNIXTHREAD) {
            if (lc->cmdsize < sizeof(thread_command_t) + sizeof(thread_state_hdr_t))
                { off += lc->cmdsize; continue; }
            const thread_state_hdr_t *th = (const thread_state_hdr_t *)
                ((const uint8_t *)lc + sizeof(thread_command_t));
            if (th->flavor == x86_THREAD_STATE64)
                entry = th->state.rip;

        } else if (lc->cmd == LC_MAIN) {
            if (lc->cmdsize < sizeof(entry_point_command_t))
                { off += lc->cmdsize; continue; }
            const entry_point_command_t *ep = (const entry_point_command_t *)lc;
            entry = text_vmaddr + ep->entryoff;
        }

        off += lc->cmdsize;
    }

    return entry;
}

/* ------------------------------------------------------------------ */
/* Spawn                                                               */
/* ------------------------------------------------------------------ */

uint32_t macho_spawn(const char *vfs_path, const char **argv, const char **envp) {
    extern void serial_puts(const char *);
    fd_t fd = vfs_open(vfs_path, VFS_O_READ, (void *)0);
    if ((int)fd < 0) return 0;

    static uint8_t g_macho_buf[4 * 1024 * 1024];
    size_t total = 0, got = 0;
    while (total < sizeof(g_macho_buf)) {
        if (vfs_read(fd, g_macho_buf + total,
                     sizeof(g_macho_buf) - total, &got) != 0 || got == 0) break;
        total += got;
    }
    vfs_close(fd);

    vmap_t *vm = amm_vmap_create();
    if (!vm) return 0;

    if (total < sizeof(mach_header_64_t)) { amm_vmap_destroy(vm); return 0; }

    uint64_t entry = macho_load(g_macho_buf, total, vm);
    if (!entry) {
        serial_puts("macho_spawn: invalid Mach-O ");
        serial_puts(vfs_path);
        serial_puts("\n");
        amm_vmap_destroy(vm);
        return 0;
    }

    vaddr_t stack_base = (vaddr_t)(USER_STACK_TOP_M - USER_STACK_SIZE_M);
    amm_region_add(vm, stack_base, USER_STACK_SIZE_M,
                   PROT_READ | PROT_WRITE, RGN_STACK, -1);
    /* Populate ONLY the top 64 KiB of the stack — enough for build_stack_macho
     * (argv/envp/auxv) and the initial frames.  The rest grows on demand via the
     * #PF handler (RGN_STACK is demand-pageable).  Eagerly populating the full
     * 2 MiB per process otherwise fails once several processes are already up
     * (ws + syslog + netd), so e.g. dock would fail to get a stack and never
     * start — the cause of "installed desktop draws one frame then no dock". */
    #define STACK_PREPOP_M  (64 * 1024)
    if (!amm_region_populate(vm, USER_STACK_TOP_M - STACK_PREPOP_M,
                             STACK_PREPOP_M, PROT_READ | PROT_WRITE)) {
        amm_vmap_destroy(vm); return 0;
    }

    uint64_t initial_rsp = build_stack_macho(argv, envp, vm);
    if (!initial_rsp) { amm_vmap_destroy(vm); return 0; }

    spawn_args_t sa = {
        .binary      = vfs_path,
        .argv        = argv,
        .envp        = envp,
        .sched_class = SCHED_BATCH,
        .priority    = 0,
        .cpu_affinity = 0xFF,
        .va_map      = vm
    };
    pid_t pid = sched_spawn(&sa);
    if (!pid) { amm_vmap_destroy(vm); return 0; }

    pcb_t *pcb = sched_find(pid);
    if (!pcb) { amm_vmap_destroy(vm); return 0; }

    extern uid_t g_shell_uid;
    extern gid_t g_shell_gid;
    pcb->regs.rip    = entry;
    pcb->regs.rsp    = initial_rsp;
    pcb->regs.rflags = 0x202;
    pcb->regs.cr3    = vm->cr3;
    pcb->regs.cs     = 0x2B;
    pcb->regs.ss     = 0x23;
    pcb->uid         = g_shell_uid;
    pcb->gid         = g_shell_gid;
    pcb->deadline_ns = UINT64_MAX;
    pcb->state       = PROC_READY;
    sched_enqueue(pcb);

    return (uint32_t)pid;
}
