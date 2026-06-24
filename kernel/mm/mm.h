/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026, ASD Project Contributors
 * All rights reserved.
 */

#ifndef AMM_MM_H
#define AMM_MM_H

#include <stdint.h>
#include <stddef.h>

typedef uintptr_t vaddr_t;
typedef uintptr_t paddr_t;

#define MAX_PROCS  4096

#define PAGE_SIZE        4096UL
#define PAGE_SHIFT       12
#define PAGE_ALIGN(x)    (((x) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
#define PAGE_ALIGN_DOWN(x) ((x) & ~(PAGE_SIZE - 1))
#define PAGE_OF(x)       ((x) >> PAGE_SHIFT)
#define PHYS_MAP_BASE    0xFFFFC80000000000ULL

#define RGN_CODE    0
#define RGN_STACK   1
#define RGN_HEAP    2
#define RGN_FILE    3
#define RGN_SHARED  4

#ifndef PROT_NONE
#define PROT_NONE   0x0
#endif
#ifndef PROT_READ
#define PROT_READ   0x1
#endif
#ifndef PROT_WRITE
#define PROT_WRITE  0x2
#endif
#ifndef PROT_EXEC
#define PROT_EXEC   0x4
#endif

#define SHM_CREATE          0x01
#define SHM_RDONLY_USERLAND 0x02

typedef struct vregion {
    vaddr_t          base;
    size_t           size;
    int              prot;
    int              kind;
    int              fd;
    struct vregion  *prev;
    struct vregion  *next;
} vregion_t;

typedef struct vmap {
    vregion_t *head;
    uint64_t   cr3;
    uint32_t   region_count;
    uint32_t   _pad;
} vmap_t;

static inline void *amm_phys_access(paddr_t paddr) {
    return (void *)(PHYS_MAP_BASE + paddr);
}

uint64_t amm_get_kernel_cr3(void);

void amm_early_phys_scan(const void *mmap, uint32_t count, uint32_t entry_sz);
void amm_phys_init(const void *mmap, uint32_t count, uint32_t entry_sz);
paddr_t amm_phys_alloc(uint32_t order);
void    amm_phys_free(paddr_t paddr, uint32_t order);

vmap_t *amm_vmap_create(void);

void    amm_vmap_destroy(vmap_t *map);
int     amm_region_add(vmap_t *map, vaddr_t base, size_t size,
                       int prot, int kind, int fd);
int     amm_region_remove(vmap_t *map, vaddr_t base);
int     amm_region_overlaps(vmap_t *map, vaddr_t base, size_t size);
vregion_t *amm_region_find(vmap_t *map, vaddr_t addr);

vaddr_t amm_shmap(const char *name, size_t size, int flags);
int     amm_shunmap(vaddr_t addr);

int amm_page_fault(vmap_t *map, vaddr_t fault_addr, int write);
/* Eagerly populate all pages in [base, base+size) in the given vmap.
 * Returns a kernel-accessible pointer to the start of the region
 * (via PHYS_MAP_BASE), or NULL on OOM. */
void *amm_region_populate(vmap_t *map, vaddr_t base, size_t size, int prot);
/* Translate a user virtual address in a vmap to a kernel-accessible pointer. */
void *amm_virt_to_kva(vmap_t *map, vaddr_t va);

void *kmalloc(size_t size);
void  kfree(void *ptr);

void mm_get_meminfo(uint64_t *total_bytes, uint64_t *free_bytes);

#endif
