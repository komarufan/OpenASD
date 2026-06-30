/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, ASD Project Contributors
 *
 * ELF64 loader — maps PT_LOAD segments into a user vmap and
 * returns the entry point so the caller can SYSRET into it.
 */

#include "elf.h"
#include "../vfs/vfs.h"
#include "../sched/sched.h"
#include "../include/string.h"
#include <stdint.h>
#include <stddef.h>

#define ELF_MAGIC   0x464C457F
#define ET_EXEC     2
#define ET_DYN      3
#define EM_X86_64   62
#define PT_LOAD     1
#define PT_INTERP   3
#define PF_X        0x1
#define PF_W        0x2
#define PF_R        0x4
#define ELFCLASS64  2
#define ELFDATA2LSB 1

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed)) elf64_ehdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed)) elf64_phdr_t;

#define USER_STACK_SIZE  (2UL * 1024 * 1024)
#define USER_STACK_TOP   0x00007FFF00000000ULL

static int count_strs(const char **arr) {
    if (!arr) return 0;
    int n = 0;
    while (arr[n]) n++;
    return n;
}

static size_t str_len(const char *s) {
    if (!s) return 0;
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

static int vm_copy_to_user(vmap_t *vm, vaddr_t dst, const void *src, size_t len);

static int push_user_bytes(vmap_t *vm, uint64_t *sp, const void *src, size_t len) {
    *sp -= (uint64_t)len;
    return vm_copy_to_user(vm, (vaddr_t)*sp, src, len);
}

static int push_user_u64(vmap_t *vm, uint64_t *sp, uint64_t val) {
    *sp -= 8;
    return vm_copy_to_user(vm, (vaddr_t)*sp, &val, 8);
}

static uint64_t build_user_stack_vm(const char **argv, const char **envp, vmap_t *vm) {
    int argc = count_strs(argv);
    int envc = count_strs(envp);
    uint64_t user_sp = USER_STACK_TOP;

#define PUSH_BYTES_VM(src, len) do { \
    if (push_user_bytes(vm, &user_sp, (src), (len)) != 0) return 0; \
} while (0)

#define PUSH_U64_VM(val) do { \
    if (push_user_u64(vm, &user_sp, (uint64_t)(val)) != 0) return 0; \
} while (0)

    /* 1. Copy envp strings */
    uint64_t envp_ptrs[64];
    int env_count = (envc > 63) ? 63 : envc;
    for (int i = env_count - 1; i >= 0; i--) {
        size_t slen = str_len(envp[i]) + 1;
        PUSH_BYTES_VM(envp[i], slen);
        envp_ptrs[i] = user_sp;
    }

    /* 2. Copy argv strings */
    uint64_t argv_ptrs[64];
    int arg_count = (argc > 63) ? 63 : argc;
    for (int i = arg_count - 1; i >= 0; i--) {
        size_t slen = str_len(argv[i]) + 1;
        PUSH_BYTES_VM(argv[i], slen);
        argv_ptrs[i] = user_sp;
    }

    /* 3. Align RSP to 16 bytes for the pointers section */
    user_sp &= ~15ULL;

    /* 4. System V ABI requires (argc + argv + NULL + envp + NULL) to be 16-byte aligned.
     * Total slots = 1 (argc) + (arg_count + 1) + (env_count + 1) = arg_count + env_count + 3.
     * If this is odd, we need an extra 8-byte padding slot.
     * 
     * NOTE: The ABI actually says RSP must be 16-byte aligned *before* the call.
     * Since we are at the entry point, RSP should be 16-byte aligned.
     */
    if ((arg_count + env_count + 3) % 2 != 0) {
        PUSH_U64_VM(0);
    }

    /* 5. Push envp pointers */
    PUSH_U64_VM(0);
    for (int i = env_count - 1; i >= 0; i--) PUSH_U64_VM(envp_ptrs[i]);

    /* 6. Push argv pointers */
    PUSH_U64_VM(0);
    for (int i = arg_count - 1; i >= 0; i--) PUSH_U64_VM(argv_ptrs[i]);

    /* 7. Push argc */
    PUSH_U64_VM((uint64_t)arg_count);

#undef PUSH_BYTES_VM
#undef PUSH_U64_VM
    return user_sp;
}

static int vm_copy_to_user(vmap_t *vm, vaddr_t dst, const void *src, size_t len) {
    const uint8_t *s = (const uint8_t *)src;
    while (len > 0) {
        size_t page_off = (size_t)(dst & (PAGE_SIZE - 1));
        size_t chunk = PAGE_SIZE - page_off;
        if (chunk > len) chunk = len;
        void *kva = amm_virt_to_kva(vm, dst);
        if (!kva) return -1;
        memcpy(kva, s, chunk);
        dst += chunk; s += chunk; len -= chunk;
    }
    return 0;
}

static int vm_zero_user(vmap_t *vm, vaddr_t dst, size_t len) {
    while (len > 0) {
        size_t page_off = (size_t)(dst & (PAGE_SIZE - 1));
        size_t chunk = PAGE_SIZE - page_off;
        if (chunk > len) chunk = len;
        void *kva = amm_virt_to_kva(vm, dst);
        if (!kva) return -1;
        memset(kva, 0, chunk);
        dst += chunk; len -= chunk;
    }
    return 0;
}

uint64_t elf_load(const void *image, size_t image_size, vmap_t *vm) {
    if (!image || image_size < sizeof(elf64_ehdr_t)) return 0;
    const elf64_ehdr_t *eh = (const elf64_ehdr_t *)image;
    if (memcmp(eh->e_ident, "\x7f\x45\x4c\x46", 4) != 0) return 0;
    if (eh->e_ident[4] != ELFCLASS64 || eh->e_ident[5] != ELFDATA2LSB) return 0;
    if (eh->e_machine != EM_X86_64) return 0;
    if (eh->e_type != ET_EXEC && eh->e_type != ET_DYN) return 0;
    if (eh->e_phnum == 0 || eh->e_phnum > 48) return 0;
    if (eh->e_phentsize < sizeof(elf64_phdr_t)) return 0;
    if (eh->e_phoff >= image_size) return 0;
    {
        uint64_t ph_tab = (uint64_t)eh->e_phnum * (uint64_t)eh->e_phentsize;
        if (eh->e_phoff + ph_tab > image_size) return 0;
    }

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        uint64_t ph_off = eh->e_phoff + (uint64_t)i * (uint64_t)eh->e_phentsize;
        if (ph_off + sizeof(elf64_phdr_t) > image_size) return 0;
        const elf64_phdr_t *ph = (const elf64_phdr_t *)((const uint8_t *)image + ph_off);
        if (ph->p_type == PT_INTERP) return 0;
        if (ph->p_type != PT_LOAD) continue;
        if (ph->p_vaddr >= 0x0000800000000000ULL) return 0;
        if (ph->p_offset > image_size || ph->p_filesz > image_size - ph->p_offset)
            return 0;

        int prot = PROT_READ;
        if (ph->p_flags & PF_W) prot |= PROT_WRITE;
        if (ph->p_flags & PF_X) prot |= PROT_EXEC;

        uint64_t seg_vaddr = PAGE_ALIGN_DOWN(ph->p_vaddr);
        uint64_t seg_end   = PAGE_ALIGN(ph->p_vaddr + ph->p_memsz);
        size_t   seg_sz    = (size_t)(seg_end - seg_vaddr);
        if (seg_sz == 0 || seg_sz > (64UL * 1024 * 1024)) return 0;

        if (amm_region_add(vm, (vaddr_t)seg_vaddr, seg_sz, prot, RGN_FILE, -1) != 0) return 0;
        if (!amm_region_populate(vm, (vaddr_t)seg_vaddr, seg_sz, prot | PROT_WRITE)) return 0;

        if (ph->p_filesz > 0) {
            if (vm_copy_to_user(vm, (vaddr_t)ph->p_vaddr, (const uint8_t *)image + ph->p_offset, (size_t)ph->p_filesz) != 0) return 0;
        }
        if (ph->p_memsz > ph->p_filesz) {
            if (vm_zero_user(vm, (vaddr_t)(ph->p_vaddr + ph->p_filesz), (size_t)(ph->p_memsz - ph->p_filesz)) != 0) return 0;
        }
    }
    return eh->e_entry;
}

uint32_t elf_spawn(const char *vfs_path, const char **argv, const char **envp) {
    fd_t fd = vfs_open(vfs_path, VFS_O_READ, (void *)0);
    if ((int)fd < 0) return 0;

    static uint8_t g_elf_buf[4 * 1024 * 1024];
    size_t total = 0, got = 0;
    while (total < sizeof(g_elf_buf)) {
        if (vfs_read(fd, g_elf_buf + total, sizeof(g_elf_buf) - total, &got) != 0 || got == 0) break;
        total += got;
    }
    vfs_close(fd);

    vmap_t *vm = amm_vmap_create();
    if (!vm) return 0;

    if (total < sizeof(elf64_ehdr_t)) { amm_vmap_destroy(vm); return 0; }

    uint64_t entry = elf_load(g_elf_buf, total, vm);
    if (!entry) {
        extern void serial_puts(const char *);
        serial_puts("elf_spawn: invalid ELF ");
        serial_puts(vfs_path);
        serial_puts("\n");
        amm_vmap_destroy(vm);
        return 0;
    }

    vaddr_t stack_base = (vaddr_t)(USER_STACK_TOP - USER_STACK_SIZE);
    amm_region_add(vm, stack_base, USER_STACK_SIZE, PROT_READ | PROT_WRITE, RGN_STACK, -1);
    amm_region_populate(vm, stack_base, USER_STACK_SIZE, PROT_READ | PROT_WRITE);

    uint64_t initial_rsp = build_user_stack_vm(argv, envp, vm);
    if (!initial_rsp) { amm_vmap_destroy(vm); return 0; }

    spawn_args_t sa = {
        .binary = vfs_path, .argv = argv, .envp = envp,
        .sched_class = SCHED_BATCH, .priority = 0, .cpu_affinity = 0xFF, .va_map = vm
    };
    pid_t pid = sched_spawn(&sa);
    if (!pid) { amm_vmap_destroy(vm); return 0; }

    pcb_t *pcb = sched_find(pid);
    if (!pcb) {
        amm_vmap_destroy(vm);
        return 0;
    }

    extern uid_t g_shell_uid;
    extern gid_t g_shell_gid;
    extern pcb_t *sched_current(void);
    pcb->regs.rip = entry;
    pcb->regs.rsp = initial_rsp;
    pcb->regs.rflags = 0x202;
    pcb->regs.cr3 = vm->cr3;
    pcb->regs.cs = 0x2B;
    pcb->regs.ss = 0x23;
    /* Inherit uid/gid from the spawner (so `do`'s setuid(0) propagates to its
     * child); processes spawned directly by init (PID 1) take g_shell_uid. */
    {
        pcb_t *parent = sched_current();
        if (parent && parent->pid != 1) {
            pcb->uid = parent->uid;
            pcb->gid = parent->gid;
        } else {
            pcb->uid = g_shell_uid;
            pcb->gid = g_shell_gid;
        }
    }
    pcb->deadline_ns = UINT64_MAX;
    pcb->state = PROC_READY;
    sched_enqueue(pcb);

    return (uint32_t)pid;
}
