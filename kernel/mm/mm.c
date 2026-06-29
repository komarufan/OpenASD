/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026, ASD Project Contributors
 * All rights reserved.
 */

#include "mm.h"
#include "../boot/asdboot.h"   /* for asd_mmap_entry_t, ASD_MEM_FREE */
#include <stddef.h>
#include <stdint.h>

typedef uint32_t spinlock_t;
static void mm_halt(void) __attribute__((noreturn));
static void mm_halt(void) { for (;;) __asm__ volatile("hlt"); }

static inline void spin_lock(spinlock_t *l) {
    while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE))
        __asm__ volatile("pause");
}
static inline void spin_unlock(spinlock_t *l) {
    __atomic_store_n(l, 0, __ATOMIC_RELEASE);
}

#include <string.h>
#define k_memset memset
#define k_memcpy memcpy
#define k_strcmp strcmp
#define k_strncpy strncpy

#define MAX_ORDER       18
#define PAGE_INFO_BASE  0xFFFF880000000000ULL
#define AMM_IDENTITY_LIMIT    (4ULL * 1024 * 1024 * 1024) /* early 1:1 window */

#define PAGE_FREE    0x01
#define PAGE_ALLOC   0x02
#define PAGE_RESRV   0x04

typedef struct {
    uint16_t flags;
    uint8_t  order;
    uint8_t  refcount;
    uint16_t map_count;
    uint16_t _reserved;
} page_info_t;

typedef struct free_block {
    struct free_block *next;
} free_block_t;

typedef struct {
    spinlock_t    lock;
    free_block_t *lists[MAX_ORDER + 1];
    uint64_t      free_pages;
    uint64_t      total_pages;
    paddr_t       phys_base;   /* lowest tracked paddr */
    paddr_t       phys_top;
} phys_state_t;

static phys_state_t g_phys;

#define PHYS_MAP_BASE  0xFFFFC80000000000ULL

/* 1 while the UEFI identity-map is active (va == pa for low memory).
   Set to 0 by amm_kernel_pml4_init() after it switches CR3. */
static int g_early_boot = 1;

static inline void *phys_to_virt(paddr_t p) {
    return g_early_boot
        ? (void *)(uintptr_t)p            /* UEFI: va == pa */
        : (void *)(PHYS_MAP_BASE + p);    /* post-CR3: use PHYS_MAP_BASE */
}
static inline paddr_t virt_to_phys(void *v) {
    return (paddr_t)((uintptr_t)v - PHYS_MAP_BASE);
}

/* ---------- bootstrap bump allocator ------------------------------------ */
/* Used by amm_kernel_pml4_init() before PHYS_MAP_BASE is mapped.
   Carved from the first free UEFI region >= BUMP_SIZE above 1 MiB.
   Under UEFI identity-mapping the pages are accessible at their paddr. */
/* Needs to cover page tables + page_info[] (up to AMM_MAX_TRACKED_PHYS). */
#define BUMP_SIZE  (64ULL * 1024 * 1024)  /* 64 MiB reservation */

static paddr_t g_bump_base;
static paddr_t g_bump_cur;
static paddr_t g_bump_end;

static void bump_init(const void *mmap, uint32_t count, uint32_t entry_sz) {
    const uint8_t *e = (const uint8_t *)mmap;
    for (uint32_t i = 0; i < count; i++, e += entry_sz) {
        const asd_mmap_entry_t *m = (const asd_mmap_entry_t *)e;
        if (m->type != ASD_MEM_FREE) continue;
        paddr_t base = PAGE_ALIGN(m->base);
        paddr_t end  = PAGE_ALIGN_DOWN(m->base + m->pages * PAGE_SIZE);
        if (base < 0x100000ULL) base = 0x100000ULL; /* skip first 1 MiB */
        if (end <= base || (end - base) < BUMP_SIZE) continue;
        g_bump_base = base;
        g_bump_cur  = base;
        g_bump_end  = base + BUMP_SIZE;
        return;
    }
    /* Fallback: use the largest available free region */
    e = (const uint8_t *)mmap;
    for (uint32_t i = 0; i < count; i++, e += entry_sz) {
        const asd_mmap_entry_t *m = (const asd_mmap_entry_t *)e;
        if (m->type != ASD_MEM_FREE) continue;
        paddr_t base = PAGE_ALIGN(m->base);
        paddr_t end  = PAGE_ALIGN_DOWN(m->base + m->pages * PAGE_SIZE);
        if (end > base && (end - base) > (g_bump_end - g_bump_base)) {
            g_bump_base = base;
            g_bump_cur  = base;
            g_bump_end  = end;
        }
    }
}

static paddr_t bump_alloc(uint32_t order) {
    paddr_t size    = (paddr_t)PAGE_SIZE << order;
    paddr_t aligned = (g_bump_cur + size - 1) & ~(size - 1); /* natural align */
    if (aligned + size > g_bump_end) return 0;
    g_bump_cur = aligned + size;
    k_memset((void *)(uintptr_t)aligned, 0, (size_t)size);
    return aligned;
}
/* ------------------------------------------------------------------------ */

static inline page_info_t *page_info(paddr_t p) {
    uint64_t pfn = p >> PAGE_SHIFT;
    return (page_info_t *)(PAGE_INFO_BASE + pfn * sizeof(page_info_t));
}

static void phys_free_block(paddr_t paddr, uint32_t order) {
    free_block_t *b = phys_to_virt(paddr);
    b->next = g_phys.lists[order];
    g_phys.lists[order] = b;
    page_info_t *pi = page_info(paddr);
    pi->flags = PAGE_FREE;
    pi->order = (uint8_t)order;
    g_phys.free_pages += (1UL << order);
}

static void phys_free_range(paddr_t base, uint64_t pages) {
    uint64_t i = 0;
    while (i < pages) {
        /* Find highest order that fits and is naturally aligned */
        uint32_t order = MAX_ORDER;
        while (order > 0) {
            uint64_t block_pages = 1UL << order;
            if (i + block_pages > pages) { order--; continue; }
            /* Check alignment */
            paddr_t addr = base + i * PAGE_SIZE;
            if (addr & ((block_pages * PAGE_SIZE) - 1)) { order--; continue; }
            break;
        }
        phys_free_block(base + i * PAGE_SIZE, order);
        i += (1UL << order);
    }
}

void amm_phys_init(const void *mmap, uint32_t count, uint32_t entry_sz) {
    k_memset(&g_phys, 0, sizeof(g_phys));
    g_phys.phys_base = (paddr_t)-1ULL;

    const uint8_t *e = (const uint8_t *)mmap;
    for (uint32_t i = 0; i < count; i++, e += entry_sz) {
        const asd_mmap_entry_t *m = (const asd_mmap_entry_t *)e;
        if (m->type != ASD_MEM_FREE) continue;
        paddr_t base = PAGE_ALIGN(m->base);
        paddr_t end  = PAGE_ALIGN_DOWN(m->base + m->pages * PAGE_SIZE);
        if (end <= base) continue;

        if (base < g_phys.phys_base) g_phys.phys_base = base;
        if (end  > g_phys.phys_top)  g_phys.phys_top  = end;

        /* Exclude the bootstrap bump pool — those pages are now page tables. */
        if (g_bump_base && base < g_bump_end && end > g_bump_base) {
            if (base < g_bump_base) {
                uint64_t pg = (g_bump_base - base) >> PAGE_SHIFT;
                phys_free_range(base, pg);
                g_phys.total_pages += pg;
            }
            if (end > g_bump_end) {
                uint64_t pg = (end - g_bump_end) >> PAGE_SHIFT;
                phys_free_range(g_bump_end, pg);
                g_phys.total_pages += pg;
            }
        } else {
            uint64_t pages = (end - base) >> PAGE_SHIFT;
            phys_free_range(base, pages);
            g_phys.total_pages += pages;
        }
    }
}

/* Scan the memory map to record phys_top and initialise the bootstrap
   bump allocator.  Must be called before amm_kernel_pml4_init(). */
void amm_early_phys_scan(const void *mmap, uint32_t count, uint32_t entry_sz) {
    k_memset(&g_phys, 0, sizeof(g_phys));
    g_phys.phys_base = (paddr_t)-1ULL;
    const uint8_t *e = (const uint8_t *)mmap;
    for (uint32_t i = 0; i < count; i++, e += entry_sz) {
        const asd_mmap_entry_t *m = (const asd_mmap_entry_t *)e;
        if (m->type != ASD_MEM_FREE) continue;
        paddr_t base = PAGE_ALIGN(m->base);
        paddr_t end  = PAGE_ALIGN_DOWN(m->base + m->pages * PAGE_SIZE);
        if (end <= base) continue;
        if (base < g_phys.phys_base) g_phys.phys_base = base;
        if (end  > g_phys.phys_top)  g_phys.phys_top  = end;
        g_phys.total_pages += (end - base) >> PAGE_SHIFT;
    }
    bump_init(mmap, count, entry_sz);
}

paddr_t amm_phys_alloc(uint32_t order) {
    if (g_early_boot) return bump_alloc(order);
    if (order > MAX_ORDER) return 0;

    spin_lock(&g_phys.lock);
    for (uint32_t o = order; o <= MAX_ORDER; o++) {
        free_block_t *b = g_phys.lists[o];
        if (!b) continue;
        g_phys.lists[o] = b->next;
        paddr_t paddr = virt_to_phys(b);
        g_phys.free_pages -= (1UL << o);

        /* Split down to requested order */
        while (o > order) {
            o--;
            paddr_t buddy = paddr ^ ((paddr_t)(PAGE_SIZE << o));
            phys_free_block(buddy, o);
        }
        page_info_t *pi = page_info(paddr);
        pi->flags = PAGE_ALLOC;
        pi->order = (uint8_t)order;
        pi->refcount = 1;
        spin_unlock(&g_phys.lock);
        return paddr;
    }
    spin_unlock(&g_phys.lock);
    return 0; /* out of memory */
}

void amm_phys_free(paddr_t paddr, uint32_t order) {
    spin_lock(&g_phys.lock);
    while (order < MAX_ORDER) {
        paddr_t buddy = paddr ^ ((paddr_t)(PAGE_SIZE << order));
        page_info_t *bi = page_info(buddy);
        if (!(bi->flags & PAGE_FREE) || bi->order != order) break;

        /* Unlink buddy from free list */
        free_block_t **pp = &g_phys.lists[order];
        free_block_t  *fb = phys_to_virt(buddy);
        while (*pp && *pp != fb) pp = &(*pp)->next;
        if (*pp) {
            *pp = fb->next;
            g_phys.free_pages -= (1UL << order);
        }
        paddr = (paddr < buddy) ? paddr : buddy;
        order++;
    }
    phys_free_block(paddr, order);
    spin_unlock(&g_phys.lock);
}

#define PTE_PRESENT  (1ULL << 0)
#define PTE_WRITE    (1ULL << 1)
#define PTE_USER     (1ULL << 2)
#define PTE_NX       (1ULL << 63)
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL

#define PML4_IDX(v)  (((v) >> 39) & 0x1FF)
#define PDPT_IDX(v)  (((v) >> 30) & 0x1FF)
#define PD_IDX(v)    (((v) >> 21) & 0x1FF)
#define PT_IDX(v)    (((v) >> 12) & 0x1FF)

static uint64_t *pt_walk_alloc(uint64_t cr3, vaddr_t va) {
    uint64_t *pml4 = phys_to_virt(cr3 & PTE_ADDR_MASK);
    uint64_t *pdpt, *pd, *pt;
    uint64_t  idx;

    /* PML4 */
    idx = PML4_IDX(va);
    if (!(pml4[idx] & PTE_PRESENT)) {
        paddr_t p = amm_phys_alloc(0);
        if (!p) return NULL;
        k_memset(phys_to_virt(p), 0, PAGE_SIZE);
        pml4[idx] = p | PTE_PRESENT | PTE_WRITE | PTE_USER;
    }
    pdpt = phys_to_virt(pml4[idx] & PTE_ADDR_MASK);

    /* PDPT */
    idx = PDPT_IDX(va);
    if (!(pdpt[idx] & PTE_PRESENT)) {
        paddr_t p = amm_phys_alloc(0);
        if (!p) return NULL;
        k_memset(phys_to_virt(p), 0, PAGE_SIZE);
        pdpt[idx] = p | PTE_PRESENT | PTE_WRITE | PTE_USER;
    }
    pd = phys_to_virt(pdpt[idx] & PTE_ADDR_MASK);

    /* PD */
    idx = PD_IDX(va);
    if (!(pd[idx] & PTE_PRESENT)) {
        paddr_t p = amm_phys_alloc(0);
        if (!p) return NULL;
        k_memset(phys_to_virt(p), 0, PAGE_SIZE);
        pd[idx] = p | PTE_PRESENT | PTE_WRITE | PTE_USER;
    }
    pt = phys_to_virt(pd[idx] & PTE_ADDR_MASK);

    return &pt[PT_IDX(va)];
}

static void map_page(uint64_t cr3, vaddr_t va, paddr_t pa, int prot) {
    uint64_t *pte = pt_walk_alloc(cr3, va);
    if (!pte) return;
    uint64_t flags = PTE_PRESENT | PTE_USER;
    if (prot & PROT_WRITE) flags |= PTE_WRITE;
    if (!(prot & PROT_EXEC)) flags |= PTE_NX;
    *pte = (pa & PTE_ADDR_MASK) | flags;
    __asm__ volatile("invlpg (%0)" :: "r"(va) : "memory");
}

static uint64_t g_kernel_cr3;

uint64_t amm_get_kernel_cr3(void) { return g_kernel_cr3; }

/* Diagnostics: current free / total physical page counts. */
uint64_t amm_free_pages(void)  { return g_phys.free_pages; }
uint64_t amm_total_pages(void) { return g_phys.total_pages; }

void amm_kernel_pml4_init(void) {
    paddr_t p = amm_phys_alloc(0);
    if (!p) mm_halt();
    k_memset(phys_to_virt(p), 0, PAGE_SIZE);
    g_kernel_cr3 = p;

    /* Map 1: Identity map a fixed low window (1:1 virtual = physical). */
    for (paddr_t pa = 0; pa < AMM_IDENTITY_LIMIT; pa += (2ULL << 20)) {
        uint64_t *pml4 = phys_to_virt(g_kernel_cr3);
        uint64_t *pdpt, *pd;
        uint32_t pml4_i = PML4_IDX(pa);
        uint32_t pdpt_i = PDPT_IDX(pa);
        uint32_t pd_i   = PD_IDX(pa);

        if (!(pml4[pml4_i] & PTE_PRESENT)) {
            paddr_t pdpt_phys = amm_phys_alloc(0);
            if (!pdpt_phys) mm_halt();
            k_memset(phys_to_virt(pdpt_phys), 0, PAGE_SIZE);
            pml4[pml4_i] = pdpt_phys | PTE_PRESENT | PTE_WRITE | PTE_USER;
        }
        pdpt = phys_to_virt(pml4[pml4_i] & PTE_ADDR_MASK);

        if (!(pdpt[pdpt_i] & PTE_PRESENT)) {
            paddr_t pd_phys = amm_phys_alloc(0);
            if (!pd_phys) mm_halt();
            k_memset(phys_to_virt(pd_phys), 0, PAGE_SIZE);
            pdpt[pdpt_i] = pd_phys | PTE_PRESENT | PTE_WRITE | PTE_USER;
        }
        pd = phys_to_virt(pdpt[pdpt_i] & PTE_ADDR_MASK);

        uint64_t pde = pa | PTE_PRESENT | PTE_WRITE | PTE_USER | (1ULL << 7);
        pd[pd_i] = pde;
    }

    /* Map 2: Page info array at PAGE_INFO_BASE (0xFFFF880000000000) */
    {
        uint64_t n_entries = (g_phys.phys_top >> PAGE_SHIFT) + 1;
        uint64_t pi_bytes  = n_entries * sizeof(page_info_t);
        uint64_t pi_pages  = (pi_bytes + PAGE_SIZE - 1) >> PAGE_SHIFT;

        /*
         * Allocate and map page_info page-by-page. This avoids requiring a
         * large contiguous early allocation on machines with high RAM.
         */
        for (uint64_t i = 0; i < pi_pages; i++) {
            paddr_t pi_phys = amm_phys_alloc(0);
            if (!pi_phys) mm_halt();
            k_memset(phys_to_virt(pi_phys), 0, PAGE_SIZE);
            map_page(g_kernel_cr3,
                     PAGE_INFO_BASE + i * PAGE_SIZE,
                     pi_phys,
                     PROT_READ | PROT_WRITE);
        }
    }

    /* Map 3: PHYS_MAP_BASE identity for phys_to_virt translation. */
    {
        uint64_t top = g_phys.phys_top;
        if (top == 0) top = AMM_IDENTITY_LIMIT;
        uint64_t *pml4 = phys_to_virt(g_kernel_cr3);

        for (paddr_t pa = 0; pa < top; pa += (2ULL << 20)) {
            uint64_t va = PHYS_MAP_BASE + pa;
            uint32_t pml4_i = PML4_IDX(va);
            uint32_t pdpt_i = PDPT_IDX(va);
            uint32_t pd_i   = PD_IDX(va);

            if (!(pml4[pml4_i] & PTE_PRESENT)) {
                paddr_t pdpt_phys = amm_phys_alloc(0);
                if (!pdpt_phys) mm_halt();
                k_memset(phys_to_virt(pdpt_phys), 0, PAGE_SIZE);
                pml4[pml4_i] = pdpt_phys | PTE_PRESENT | PTE_WRITE | PTE_USER;
            }
            uint64_t *pdpt = phys_to_virt(pml4[pml4_i] & PTE_ADDR_MASK);

            if (!(pdpt[pdpt_i] & PTE_PRESENT)) {
                paddr_t pd_phys = amm_phys_alloc(0);
                if (!pd_phys) mm_halt();
                k_memset(phys_to_virt(pd_phys), 0, PAGE_SIZE);
                pdpt[pdpt_i] = pd_phys | PTE_PRESENT | PTE_WRITE | PTE_USER;
            }
            uint64_t *pd = phys_to_virt(pdpt[pdpt_i] & PTE_ADDR_MASK);

            uint64_t pde = pa | PTE_PRESENT | PTE_WRITE | PTE_USER | (1ULL << 7);
            pd[pd_i] = pde;
        }
    }

    /* Switch to kernel page tables; after this PHYS_MAP_BASE is live. */
    __asm__ volatile("mov %0, %%cr3" :: "r"(g_kernel_cr3) : "memory");
    g_early_boot = 0;
}

#define MAX_REGIONS  (1024 * 16)
static vregion_t g_region_pool[MAX_REGIONS];
static uint32_t  g_region_next;
static spinlock_t g_region_lock;

static vregion_t *region_alloc(void) {
    spin_lock(&g_region_lock);
    if (g_region_next >= MAX_REGIONS) { spin_unlock(&g_region_lock); return NULL; }
    vregion_t *r = &g_region_pool[g_region_next++];
    spin_unlock(&g_region_lock);
    k_memset(r, 0, sizeof(*r));
    r->fd = -1;
    return r;
}

#define MAX_VMAPS  MAX_PROCS
static vmap_t    g_vmap_pool[MAX_VMAPS];
static uint32_t  g_vmap_next;
static spinlock_t g_vmap_lock;

vmap_t *amm_vmap_create(void) {
    spin_lock(&g_vmap_lock);
    if (g_vmap_next >= MAX_VMAPS) { spin_unlock(&g_vmap_lock); return NULL; }
    vmap_t *m = &g_vmap_pool[g_vmap_next++];
    spin_unlock(&g_vmap_lock);
    k_memset(m, 0, sizeof(*m));

    /* Allocate PML4 */
    paddr_t pml4_phys = amm_phys_alloc(0);
    if (!pml4_phys) return NULL;
    k_memset(phys_to_virt(pml4_phys), 0, PAGE_SIZE);

    /* Copy kernel upper-half entries */
    uint64_t *new_pml4    = phys_to_virt(pml4_phys);
    uint64_t *kernel_pml4 = phys_to_virt(g_kernel_cr3);
    for (int i = 256; i < 512; i++)
        new_pml4[i] = kernel_pml4[i];

    /* Allocate user-space PDPT and PD0 to map the low kernel code */
    paddr_t pdpt_phys = amm_phys_alloc(0);
    paddr_t pd0_phys  = amm_phys_alloc(0);
    if (!pdpt_phys || !pd0_phys) return NULL;

    k_memset(phys_to_virt(pdpt_phys), 0, PAGE_SIZE);
    k_memset(phys_to_virt(pd0_phys), 0, PAGE_SIZE);

    uint64_t *new_pdpt = phys_to_virt(pdpt_phys);
    uint64_t *new_pd0  = phys_to_virt(pd0_phys);

    new_pml4[0] = pdpt_phys | PTE_PRESENT | PTE_WRITE | PTE_USER;
    new_pdpt[0] = pd0_phys | PTE_PRESENT | PTE_WRITE | PTE_USER;

    if (kernel_pml4[0] & PTE_PRESENT) {
        uint64_t *kernel_pdpt = phys_to_virt(kernel_pml4[0] & PTE_ADDR_MASK);
        /* Copy PDPT entries 1, 2, 3 (1 GB to 4 GB) */
        new_pdpt[1] = kernel_pdpt[1];
        new_pdpt[2] = kernel_pdpt[2];
        new_pdpt[3] = kernel_pdpt[3];

        if (kernel_pdpt[0] & PTE_PRESENT) {
            uint64_t *kernel_pd0 = phys_to_virt(kernel_pdpt[0] & PTE_ADDR_MASK);
            /* Copy kernel mappings (from 64 MB onwards, i.e. index 32 to 511) */
            for (int i = 32; i < 512; i++) {
                new_pd0[i] = kernel_pd0[i];
            }
        }
    }

    m->cr3 = pml4_phys;
    return m;
}

void amm_vmap_destroy(vmap_t *map) {
    if (!map || !map->cr3) return;

    uint64_t *pml4 = phys_to_virt(map->cr3 & PTE_ADDR_MASK);

    /* Walk PML4 user half (entries 0..255) and free all page-table pages
     * plus the leaf data pages they cover. */
    for (int pml4i = 0; pml4i < 256; pml4i++) {
        if (!(pml4[pml4i] & PTE_PRESENT)) continue;
        paddr_t pdpt_pa = pml4[pml4i] & PTE_ADDR_MASK;
        uint64_t *pdpt = phys_to_virt(pdpt_pa);

        /* How many PDPT entries to scan: entries 0..511 for user PDPT.
         * For PML4[0] we only allocated entries 0..3 from kernel and
         * entries 4..511 for user; scan all and skip kernel-shared ones. */
        for (int pdpti = 0; pdpti < 512; pdpti++) {
            if (!(pdpt[pdpti] & PTE_PRESENT)) continue;

            /* Skip entries 1-3 of PML4[0] PDPT which are shared with kernel */
            if (pml4i == 0 && pdpti >= 1 && pdpti <= 3) continue;

            paddr_t pd_pa = pdpt[pdpti] & PTE_ADDR_MASK;
            uint64_t *pd = phys_to_virt(pd_pa);

            for (int pdi = 0; pdi < 512; pdi++) {
                if (!(pd[pdi] & PTE_PRESENT)) continue;
                /* Bit 7 = PS (2 MB huge page). All huge-page PDEs in user
                 * vmaps are kernel-shared identity-map entries (copied from
                 * kernel pd0 entries 32-511 in amm_vmap_create). User pages
                 * are always 4 KB mapped via map_page(); never free these. */
                if (pd[pdi] & (1ULL << 7)) continue;
                paddr_t pt_pa = pd[pdi] & PTE_ADDR_MASK;
                uint64_t *pt = phys_to_virt(pt_pa);

                for (int pti = 0; pti < 512; pti++) {
                    if (pt[pti] & PTE_PRESENT)
                        amm_phys_free(pt[pti] & PTE_ADDR_MASK, 0);
                }
                amm_phys_free(pt_pa, 0);  /* free PT page */
            }
            amm_phys_free(pd_pa, 0);      /* free PD page */
        }
        amm_phys_free(pdpt_pa, 0);        /* free PDPT page */
    }

    /* Free PML4 itself */
    amm_phys_free(map->cr3 & PTE_ADDR_MASK, 0);

    map->head = NULL;
    map->region_count = 0;
    map->cr3 = 0;
}

int amm_region_overlaps(vmap_t *map, vaddr_t base, size_t size) {
    if (!map || size == 0) return 0;
    vaddr_t end = base + size;
    if (end <= base) return 1; /* overflow is unsafe and treated as overlap */

    for (vregion_t *r = map->head; r; r = r->next) {
        vaddr_t r_end = r->base + r->size;
        if (r_end <= r->base) return 1;
        if (base < r_end && end > r->base) return 1;
        if (r->base >= end) break; /* sorted list */
    }
    return 0;
}

int amm_region_add(vmap_t *map, vaddr_t base, size_t size,
                   int prot, int kind, int fd) {
    if (!map || size == 0) return -1;
    if (amm_region_overlaps(map, base, size)) return -1;

    vregion_t *nr = region_alloc();
    if (!nr) return -1;
    nr->base = base;
    nr->size = size;
    nr->prot = prot;
    nr->kind = kind;
    nr->fd   = fd;

    /* Insert sorted by base address */
    if (!map->head || map->head->base > base) {
        nr->next = map->head;
        if (map->head) map->head->prev = nr;
        map->head = nr;
    } else {
        vregion_t *cur = map->head;
        while (cur->next && cur->next->base < base) cur = cur->next;
        nr->next = cur->next;
        nr->prev = cur;
        if (cur->next) cur->next->prev = nr;
        cur->next = nr;
    }
    map->region_count++;
    return 0;
}

int amm_region_remove(vmap_t *map, vaddr_t base) {
    vregion_t *r = map->head;
    while (r && r->base != base) r = r->next;
    if (!r) return -1;
    if (r->prev) r->prev->next = r->next;
    else         map->head     = r->next;
    if (r->next) r->next->prev = r->prev;
    map->region_count--;
    return 0;
}

vregion_t *amm_region_find(vmap_t *map, vaddr_t addr) {
    vregion_t *r = map->head;
    while (r) {
        if (addr >= r->base && addr < r->base + r->size) return r;
        if (r->base > addr) break; /* sorted list */
        r = r->next;
    }
    return NULL;
}

#define MAX_SHM_ENTRIES  256
#define SHM_NAME_LEN     64

typedef struct {
    char     name[SHM_NAME_LEN];
    paddr_t  paddr;
    size_t   size;
    vaddr_t  va_base;   /* kernel VA base for this mapping */
    uint32_t refcount;
    uint32_t flags;
} shm_entry_t;

static shm_entry_t g_shm_table[MAX_SHM_ENTRIES];
static spinlock_t  g_shm_lock;

#define SHM_KERNEL_VA_BASE  0xFFFFE80000000000ULL
static vaddr_t g_shm_kernel_va_next = SHM_KERNEL_VA_BASE;

vaddr_t amm_shmap(const char *name, size_t size, int flags) {
    size = PAGE_ALIGN(size);
    spin_lock(&g_shm_lock);

    shm_entry_t *entry = NULL;
    for (int i = 0; i < MAX_SHM_ENTRIES; i++) {
        if (g_shm_table[i].name[0] && k_strcmp(g_shm_table[i].name, name) == 0) {
            entry = &g_shm_table[i];
            break;
        }
    }

    if (!entry && (flags & SHM_CREATE)) {
        /* Allocate a new entry */
        for (int i = 0; i < MAX_SHM_ENTRIES; i++) {
            if (!g_shm_table[i].name[0]) {
                entry = &g_shm_table[i];
                break;
            }
        }
        if (!entry) { spin_unlock(&g_shm_lock); return 0; }

        /* Allocate physical pages */
        uint32_t order = 0;
        while ((1UL << order) * PAGE_SIZE < size) order++;
        paddr_t phys = amm_phys_alloc(order);
        if (!phys) { spin_unlock(&g_shm_lock); return 0; }
        k_memset(phys_to_virt(phys), 0, (size_t)(1UL << order) * PAGE_SIZE);

        k_strncpy(entry->name, name, SHM_NAME_LEN);
        entry->paddr    = phys;
        entry->size     = size;
        entry->refcount = 0;
        entry->flags    = (uint32_t)flags;
        entry->va_base  = 0; /* set at map time below */
    }

    if (!entry) { spin_unlock(&g_shm_lock); return 0; }
    entry->refcount++;

    /* Map into kernel VA */
    vaddr_t va;
    if (entry->va_base == 0) {
        va = g_shm_kernel_va_next;
        entry->va_base = va;
        g_shm_kernel_va_next += size;
    } else {
        va = entry->va_base;
    }

    spin_unlock(&g_shm_lock);

    int prot = PROT_READ;
    if (!(flags & SHM_RDONLY_USERLAND)) prot |= PROT_WRITE;

    paddr_t pa = entry->paddr;
    for (size_t off = 0; off < size; off += PAGE_SIZE) {
        map_page(g_kernel_cr3, va + off, pa + off, prot | PROT_WRITE);
    }

    return va;
}

int amm_shunmap(vaddr_t addr) {
    if (addr == 0) return -1;

    spin_lock(&g_shm_lock);

    /* Find the shm entry by VA base address */
    shm_entry_t *entry = NULL;
    for (int i = 0; i < MAX_SHM_ENTRIES; i++) {
        if (g_shm_table[i].name[0] &&
            addr >= g_shm_table[i].va_base &&
            addr < g_shm_table[i].va_base + g_shm_table[i].size) {
            entry = &g_shm_table[i];
            break;
        }
    }

    if (!entry) { spin_unlock(&g_shm_lock); return -1; }

    /* Unmap the pages from kernel PML4 */
    for (size_t off = 0; off < entry->size; off += PAGE_SIZE) {
        vaddr_t va = entry->va_base + off;
        uint64_t *pte = pt_walk_alloc(g_kernel_cr3, va);
        if (pte && (*pte & PTE_PRESENT)) {
            *pte = 0; /* clear the entry */
            __asm__ volatile("invlpg (%0)" :: "r"(va) : "memory");
        }
    }

    /* Decrement refcount; free if last reference */
    entry->refcount--;
    if (entry->refcount == 0) {
        /* Free physical pages */
        uint32_t order = 0;
        while ((1UL << order) * PAGE_SIZE < entry->size) order++;
        amm_phys_free(entry->paddr, order);
        /* Clear the entry */
        entry->name[0]  = '\0';
        entry->paddr    = 0;
        entry->size     = 0;
        entry->va_base  = 0;
        entry->flags    = 0;
    }

    spin_unlock(&g_shm_lock);
    return 0;
}

int amm_page_fault(vmap_t *map, vaddr_t fault_addr, int write) {
    vregion_t *r = amm_region_find(map, fault_addr);
    if (!r) return -1; /* segfault */

    if (write && !(r->prot & PROT_WRITE)) return -1; /* protection fault */

    paddr_t pa = amm_phys_alloc(0);
    if (!pa) return -1; /* OOM */

    k_memset(phys_to_virt(pa), 0, PAGE_SIZE);
    map_page(map->cr3, fault_addr & ~(PAGE_SIZE - 1), pa, r->prot);
    return 0;
}

/*
 * amm_region_populate -- eagerly allocate and map all pages in [base, base+size).
 * Returns a kernel virtual pointer (PHYS_MAP_BASE + first_pa) to the start of
 * the contiguous physical allocation, or NULL on OOM.
 *
 * Used by elf_load() to write ELF segment data into a new address space
 * without switching CR3 (which would require the new CR3 to be active).
 */
void *amm_region_populate(vmap_t *map, vaddr_t base, size_t size, int prot) {
    if (!map || size == 0) return NULL;
    vaddr_t aligned_base = base & ~(PAGE_SIZE - 1);
    vaddr_t aligned_end  = PAGE_ALIGN(base + size);
    if (aligned_end <= aligned_base) return NULL;

    for (vaddr_t va = aligned_base; va < aligned_end; va += PAGE_SIZE) {
        if (amm_virt_to_kva(map, va)) {
            continue; /* already backed, e.g. overlapping rounded ELF segment */
        }

        paddr_t pa = amm_phys_alloc(0);
        if (!pa) return NULL; /* OOM */
        k_memset(phys_to_virt(pa), 0, PAGE_SIZE);
        map_page(map->cr3, va, pa, prot);
    }
    return amm_virt_to_kva(map, base);
}

/*
 * amm_virt_to_kva -- translate a virtual address in a vmap to a kernel
 * accessible pointer via PHYS_MAP_BASE.  Returns NULL if the page is not
 * present in the vmap's page tables.
 */
void *amm_virt_to_kva(vmap_t *map, vaddr_t va) {
    if (!map) return NULL;
    uint64_t *pml4 = phys_to_virt(map->cr3 & PTE_ADDR_MASK);
    uint64_t  e;
    e = pml4[PML4_IDX(va)];
    if (!(e & PTE_PRESENT)) return NULL;
    uint64_t *pdpt = phys_to_virt(e & PTE_ADDR_MASK);
    e = pdpt[PDPT_IDX(va)];
    if (!(e & PTE_PRESENT)) return NULL;
    uint64_t *pd = phys_to_virt(e & PTE_ADDR_MASK);
    e = pd[PD_IDX(va)];
    if (!(e & PTE_PRESENT)) return NULL;
    uint64_t *pt = phys_to_virt(e & PTE_ADDR_MASK);
    e = pt[PT_IDX(va)];
    if (!(e & PTE_PRESENT)) return NULL;
    paddr_t pa = (e & PTE_ADDR_MASK) | (va & (PAGE_SIZE - 1));
    return phys_to_virt(pa);
}

#define SLAB_CLASSES  10

static const uint32_t slab_sizes[SLAB_CLASSES] = {
    8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096
};

#define SLAB_HDR_SZ   64

typedef struct slab_hdr {
    uint32_t free_count;
    uint32_t obj_size;
    uint32_t capacity;
    struct slab_hdr *next;
    uint8_t  bitmap[36]; /* up to 288 bits — enough for 8-byte objects in 4K */
} slab_hdr_t;

static slab_hdr_t *g_slab_heads[SLAB_CLASSES];
static spinlock_t  g_slab_lock;

static int slab_class(size_t size) {
    for (int i = 0; i < SLAB_CLASSES; i++)
        if (size <= slab_sizes[i]) return i;
    return -1;
}

static slab_hdr_t *slab_new(int cls) {
    uint32_t obj_sz   = slab_sizes[cls];
    uint32_t slab_pages = (obj_sz >= 2048) ? 2 : 1;
    paddr_t  pa = amm_phys_alloc(slab_pages == 2 ? 1 : 0);
    if (!pa) return NULL;

    slab_hdr_t *s = phys_to_virt(pa);
    k_memset(s, 0, SLAB_HDR_SZ);
    s->obj_size  = obj_sz;
    s->capacity  = (uint32_t)((slab_pages * PAGE_SIZE - SLAB_HDR_SZ) / obj_sz);
    s->free_count = s->capacity;
    if (s->capacity > 288) s->capacity = 288;
    return s;
}

void *kmalloc(size_t size) {
    if (size == 0) return NULL;
    int cls = slab_class(size);
    if (cls < 0) {
        /* Large allocation: directly from buddy */
        uint32_t order = 0;
        while ((size_t)(PAGE_SIZE << order) < size) order++;
        paddr_t pa = amm_phys_alloc(order);
        return pa ? phys_to_virt(pa) : NULL;
    }

    spin_lock(&g_slab_lock);
    slab_hdr_t *s = g_slab_heads[cls];
    /* Find a slab with free slots */
    while (s && s->free_count == 0) s = s->next;
    if (!s) {
        s = slab_new(cls);
        if (!s) { spin_unlock(&g_slab_lock); return NULL; }
        s->next = g_slab_heads[cls];
        g_slab_heads[cls] = s;
    }

    /* Find first free slot via bitmap */
    uint32_t slot = 0;
    for (uint32_t i = 0; i < s->capacity; i++) {
        uint32_t byte = i / 8, bit = i % 8;
        if (!(s->bitmap[byte] & (1u << bit))) { slot = i; break; }
    }
    s->bitmap[slot / 8] |= (uint8_t)(1u << (slot % 8));
    s->free_count--;
    spin_unlock(&g_slab_lock);

    return (uint8_t *)s + SLAB_HDR_SZ + slot * slab_sizes[cls];
}

void kfree(void *ptr) {
    if (!ptr) return;
    
    paddr_t pa = virt_to_phys(ptr);
    page_info_t *pi = page_info(pa);
    
    /* If it's a large allocation directly from buddy, pi->order will be set and 
       it won't be part of a slab. However, we need a way to distinguish.
       Slabs are always page-aligned and have a slab_hdr_t. */
    
    if ((uintptr_t)ptr % PAGE_SIZE == 0) {
        slab_hdr_t *s = (slab_hdr_t *)ptr;
        /* Simple heuristic: if it looks like a slab header, it might be one.
           But a better way is to check if it's in our slab lists or use page flags. */
        if (s->obj_size >= 8 && s->obj_size <= 4096) {
            /* This is a slab page itself being freed? No, kmalloc returns ptr inside slab.
               Large allocations are page-aligned. */
            amm_phys_free(pa, pi->order);
            return;
        }
    }

    /* Locate slab header: slabs are at most 2 pages, but always start at page boundary */
    slab_hdr_t *s = (slab_hdr_t *)((uintptr_t)ptr & ~(PAGE_SIZE - 1));
    
    /* If the page is not marked as allocated or doesn't look like a slab, it might be a 2-page slab's second page */
    page_info_t *spi = page_info(virt_to_phys(s));
    if (spi->order == 1 && (uintptr_t)ptr % (2 * PAGE_SIZE) >= PAGE_SIZE) {
        s = (slab_hdr_t *)((uintptr_t)s - PAGE_SIZE);
    }

    if (s->obj_size < 8 || s->obj_size > 4096) {
        /* Not a slab, must be a large allocation */
        amm_phys_free(pa, pi->order);
        return;
    }

    uintptr_t off = (uintptr_t)ptr - ((uintptr_t)s + SLAB_HDR_SZ);
    uint32_t slot = (uint32_t)(off / s->obj_size);
    if (slot >= s->capacity) return;

    spin_lock(&g_slab_lock);
    /* FIX (v5): double-free detection — check the bit is actually set before
     * clearing it.  A double-free would silently corrupt the bitmap and
     * allow two callers to receive the same allocation. */
    uint8_t already_free = !(s->bitmap[slot / 8] & (1u << (slot % 8)));
    if (already_free) {
        /* Double-free detected: do NOT corrupt the bitmap. */
        spin_unlock(&g_slab_lock);
        /* In a production kernel this would panic; here we just return. */
        return;
    }
    s->bitmap[slot / 8] &= (uint8_t)~(1u << (slot % 8));
    s->free_count++;
    spin_unlock(&g_slab_lock);
}

void mm_get_meminfo(uint64_t *total_bytes, uint64_t *free_bytes) {
    *total_bytes = g_phys.total_pages * PAGE_SIZE;
    *free_bytes  = g_phys.free_pages  * PAGE_SIZE;
}
