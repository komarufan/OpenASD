/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026, ASD Project Contributors
 * All rights reserved.
 */

#include "sched.h"
#include "../arch/syscall.h"
#include "../arch/pit.h"
#include "../console/fbcon.h"
#include "../mm/mm.h"
#include "../usr/usr.h"
#include "../vfs/vfs.h"
#include "../include/string.h"
#include <stddef.h>
#include <stdint.h>

static inline void spin_lock(uint32_t *lock) {
    while (__atomic_exchange_n(lock, 1, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("pause");
    }
}

static inline void spin_unlock(uint32_t *lock) {
    __atomic_store_n(lock, 0, __ATOMIC_RELEASE);
}

static uint64_t g_tsc_hz = 1000000000ULL; /* assume 1 GHz until calibrated */

static uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

uint64_t asd_time_ns(void) {
    return rdtsc() * 1000000000ULL / g_tsc_hz;
}

#define RB_PCB(node) \
    ((pcb_t *)((char *)(node) - __builtin_offsetof(pcb_t, rq_node)))

static void rb_rotate_left(rb_root_t *root, rb_node_t *x) {
    rb_node_t *y = x->right;
    x->right = y->left;
    if (y->left) y->left->parent = x;
    y->parent = x->parent;
    if (!x->parent)              root->root = y;
    else if (x == x->parent->left) x->parent->left  = y;
    else                           x->parent->right = y;
    y->left = x;
    x->parent = y;
}

static void rb_rotate_right(rb_root_t *root, rb_node_t *x) {
    rb_node_t *y = x->left;
    x->left = y->right;
    if (y->right) y->right->parent = x;
    y->parent = x->parent;
    if (!x->parent)               root->root = y;
    else if (x == x->parent->right) x->parent->right = y;
    else                             x->parent->left  = y;
    y->right = x;
    x->parent = y;
}

static void rb_insert_fixup(rb_root_t *root, rb_node_t *z) {
    while (z->parent && z->parent->color == 1) {
        rb_node_t *gp = z->parent->parent;
        if (!gp) break;
        if (z->parent == gp->left) {
            rb_node_t *y = gp->right;
            if (y && y->color == 1) {
                z->parent->color = 0; y->color = 0; gp->color = 1; z = gp;
            } else {
                if (z == z->parent->right) { z = z->parent; rb_rotate_left(root, z); }
                z->parent->color = 0; gp->color = 1; rb_rotate_right(root, gp);
            }
        } else {
            rb_node_t *y = gp->left;
            if (y && y->color == 1) {
                z->parent->color = 0; y->color = 0; gp->color = 1; z = gp;
            } else {
                if (z == z->parent->left) { z = z->parent; rb_rotate_right(root, z); }
                z->parent->color = 0; gp->color = 1; rb_rotate_left(root, gp);
            }
        }
    }
    root->root->color = 0;
}

static void rb_insert(rb_root_t *root, rb_node_t *z, int16_t key) {
    z->left = z->right = NULL;
    z->parent = NULL;
    z->color = 1;

    rb_node_t *parent = NULL;
    rb_node_t *x = root->root;
    while (x) {
        parent = x;
        if (key < RB_PCB(x)->priority) x = x->left;
        else                            x = x->right;
    }
    z->parent = parent;
    z->left = z->right = NULL;
    z->color = 1; /* red */
    if (!parent)           root->root = z;
    else if (key < RB_PCB(parent)->priority) parent->left  = z;
    else                                      parent->right = z;
    rb_insert_fixup(root, z);
}

static rb_node_t *rb_leftmost(rb_root_t *root) {
    rb_node_t *n = root->root;
    if (!n) return NULL;
    while (n->left) n = n->left;
    return n;
}

static void rb_transplant(rb_root_t *root, rb_node_t *u, rb_node_t *v) {
    if (!u->parent)              root->root = v;
    else if (u == u->parent->left) u->parent->left  = v;
    else                            u->parent->right = v;
    if (v) v->parent = u->parent;
}

static void rb_delete_fixup(rb_root_t *root, rb_node_t *x, rb_node_t *x_parent) {
    while (x != root->root && (!x || x->color == 0)) {
        if (x == (x_parent ? x_parent->left : NULL)) {
            rb_node_t *w = x_parent ? x_parent->right : NULL;
            if (w && w->color == 1) {
                w->color = 0; x_parent->color = 1;
                rb_rotate_left(root, x_parent);
                w = x_parent->right;
            }
            if ((!w->left  || w->left->color  == 0) &&
                (!w->right || w->right->color == 0)) {
                if (w) w->color = 1;
                x = x_parent;
                x_parent = x ? x->parent : NULL;
            } else {
                if (!w->right || w->right->color == 0) {
                    if (w->left) w->left->color = 0;
                    if (w) w->color = 1;
                    rb_rotate_right(root, w);
                    w = x_parent ? x_parent->right : NULL;
                }
                if (w) w->color = x_parent ? x_parent->color : 0;
                if (x_parent) x_parent->color = 0;
                if (w && w->right) w->right->color = 0;
                if (x_parent) rb_rotate_left(root, x_parent);
                x = root->root;
                x_parent = NULL;
            }
        } else {
            rb_node_t *w = x_parent ? x_parent->left : NULL;
            if (w && w->color == 1) {
                w->color = 0; x_parent->color = 1;
                rb_rotate_right(root, x_parent);
                w = x_parent->left;
            }
            if ((!w->right || w->right->color == 0) &&
                (!w->left  || w->left->color  == 0)) {
                if (w) w->color = 1;
                x = x_parent;
                x_parent = x ? x->parent : NULL;
            } else {
                if (!w->left || w->left->color == 0) {
                    if (w->right) w->right->color = 0;
                    if (w) w->color = 1;
                    rb_rotate_left(root, w);
                    w = x_parent ? x_parent->left : NULL;
                }
                if (w) w->color = x_parent ? x_parent->color : 0;
                if (x_parent) x_parent->color = 0;
                if (w && w->left) w->left->color = 0;
                if (x_parent) rb_rotate_right(root, x_parent);
                x = root->root;
                x_parent = NULL;
            }
        }
    }
    if (x) x->color = 0;
}

static void rb_remove_node(rb_root_t *root, rb_node_t *z) {
    rb_node_t *x = NULL, *x_parent = NULL;
    int orig_color = z->color;

    if (!z->left) {
        x = z->right; x_parent = z->parent;
        rb_transplant(root, z, z->right);
    } else if (!z->right) {
        x = z->left; x_parent = z->parent;
        rb_transplant(root, z, z->left);
    } else {
        /* Successor: leftmost node in right subtree */
        rb_node_t *y = z->right;
        while (y->left) y = y->left;
        orig_color = y->color;
        x = y->right;
        if (y->parent == z) {
            x_parent = y;
        } else {
            x_parent = y->parent;
            rb_transplant(root, y, y->right);
            y->right = z->right;
            y->right->parent = y;
        }
        rb_transplant(root, z, y);
        y->left = z->left;
        y->left->parent = y;
        y->color = z->color;
    }

    z->left = z->right = z->parent = NULL;

    if (orig_color == 0)
        rb_delete_fixup(root, x, x_parent);

    if (root->root) root->root->color = 0;
}

static runq_t        g_runq[MAX_CPUS];
static uint32_t      g_cpu_count;
static pcb_t         g_proc_table[MAX_PROCS];
static uint32_t      g_proc_table_lock;
static uint32_t      g_next_pid;
static uint32_t      g_proc_count;

static sched_state_t g_sched_state __attribute__((aligned(4096)));

static pcb_t        *g_current[MAX_CPUS];

/* Thread blocked in sched_reap() — must not be enqueued as READY without parent. */
static pcb_t        *g_reap_waiter;

typedef struct {
    uint64_t rbx, rbp, r12, r13, r14, r15;
    uint64_t rsp;
    uint64_t rip;
} sched_jmpbuf_t;

static sched_jmpbuf_t g_reap_jmp;
static volatile int   g_reap_jmp_active;

extern int  sched_jmp_save(sched_jmpbuf_t *buf);
extern void sched_jmp_restore(sched_jmpbuf_t *buf, int val) __attribute__((noreturn));

static pcb_t         g_idle_pcb[MAX_CPUS];

static pcb_t         g_bootstrap_pcb;
static uint8_t       g_bootstrap_kstack[SCHED_BOOTSTRAP_KSTACK_SIZE]
    __attribute__((aligned(16)));

static int sched_kstack_alloc(pcb_t *pcb) {
    if (!pcb)
        return -1;
    if (pcb->kstack && pcb->kstack_top)
        return 0;
    uint8_t *ks = (uint8_t *)kmalloc(SCHED_KSTACK_SIZE);
    if (!ks)
        return -1;
    pcb->kstack     = ks;
    pcb->kstack_top = (uint64_t)(uintptr_t)(ks + SCHED_KSTACK_SIZE);
    return 0;
}

#define LAPIC_BASE       0xFEE00000ULL
#define LAPIC_TIMER_LVT  0x320
#define LAPIC_TIMER_ICR  0x380
#define LAPIC_TIMER_CCR  0x390
#define LAPIC_TIMER_DIV  0x3E0
#define LAPIC_EOI        0x0B0

static volatile uint32_t *lapic_reg(uint32_t off) {
    return (volatile uint32_t *)(LAPIC_BASE + off);
}

static void lapic_write(uint32_t off, uint32_t val) {
    *lapic_reg(off) = val;
}

static void lapic_program_oneshot(uint64_t ns) {
    (void)ns;
    /* PIT drives preemption; LAPIC MMIO is not mapped in this bring-up. */
}

static uint32_t this_cpu_id(void) {
    /* BSP only until per-CPU runqueues are brought up on APs. */
    return 0;
}

static void idle_fn(void) {
    for (;;) __asm__ volatile("hlt");
}

static void sched_load_cr3(uint64_t cr3) {
    if (!cr3) cr3 = amm_get_kernel_cr3();
    uint64_t cur;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cur));
    if (cr3 != cur)
        __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
}

void sched_init(uint32_t cpu_count) {
    g_cpu_count = (cpu_count > MAX_CPUS) ? MAX_CPUS : cpu_count;
    g_next_pid  = 1;
    g_proc_count = 0;

    for (uint32_t i = 0; i < g_cpu_count; i++) {
        runq_t *rq = &g_runq[i];
        rq->lock       = 0;
        rq->cpu_id     = (uint8_t)i;
        rq->rt_tree.root = rq->intr_tree.root = rq->batch_tree.root = NULL;
        rq->rt_count = rq->intr_count = rq->batch_count = 0;
        rq->steal_ns = rq->total_ns = rq->idle_ns = 0;

        /* Idle PCB */
        pcb_t *idle = &g_idle_pcb[i];
        memset(idle, 0, sizeof(*idle));
        idle->pid         = 0;
        idle->state       = PROC_RUN;
        idle->sched_class = SCHED_BATCH;
        idle->cpu         = (uint8_t)i;
        idle->priority    = 127;
        idle->base_pri    = 127;
        idle->regs.rip    = (uint64_t)(uintptr_t)idle_fn;
        idle->regs.rflags = 0x202;
        idle->regs.cs     = 0x08;   /* SEG_KERNEL_CODE */
        idle->regs.ss     = 0x10;   /* SEG_KERNEL_DATA */
        idle->regs.cr3    = amm_get_kernel_cr3();
        idle->deadline_ns = UINT64_MAX;
        for (int j = 0; j < SCHED_NAME_LEN - 1 && "idle"[j]; j++)
            idle->name[j] = "idle"[j];
        if (sched_kstack_alloc(idle) != 0) {
            idle->kstack     = g_bootstrap_kstack;
            idle->kstack_top = (uint64_t)(uintptr_t)(g_bootstrap_kstack +
                                                    SCHED_BOOTSTRAP_KSTACK_SIZE);
        }
        rq->idle_pcb = idle;
        rq->current  = idle;
        g_current[i] = idle;
    }

    g_sched_state.magic     = SCHED_STATE_MAGIC;
    g_sched_state.cpu_count = g_cpu_count;
}

__attribute__((visibility("default"))) void sched_bootstrap_init_thread(void) {
    pcb_t *pcb = &g_bootstrap_pcb;
    memset(pcb, 0, sizeof(*pcb));
    pcb->pid         = 1;
    pcb->state       = PROC_RUN;
    pcb->sched_class = SCHED_BATCH;
    pcb->priority    = 0;
    pcb->base_pri    = 0;
    pcb->cpu         = 0;
    pcb->uid         = UID_ROOT;
    pcb->gid         = GID_ROOT;
    pcb->regs.rflags = 0x202;
    pcb->regs.cs     = 0x08;
    pcb->regs.ss     = 0x10;
    pcb->regs.cr3    = amm_get_kernel_cr3();
    pcb->kstack      = g_bootstrap_kstack;
    pcb->kstack_top  = (uint64_t)(uintptr_t)(g_bootstrap_kstack +
                                             SCHED_BOOTSTRAP_KSTACK_SIZE);
    pcb->deadline_ns = UINT64_MAX;  /* must not be preempted with deadline 0 */
    strncpy(pcb->name, "asdinit", SCHED_NAME_LEN - 1);

    g_current[0]      = pcb;
    g_runq[0].current = pcb;
    syscall_on_thread_switch(pcb);
}

__attribute__((visibility("default"))) void sched_bootstrap_enter(void (*fn)(void)) {
    /* SysV ABI: RSP must be 16-byte aligned *before* the CALL, so that on
     * function entry (RSP+8) is a multiple of 16.  The CALL below pushes the
     * 8-byte return address, giving the callee the alignment its prologue
     * assumes.  The old "kstack_top - 8" made RSP%16==8 before the CALL, so
     * the callee saw RSP%16==0, its RBP became %16==8, and any 16-byte-aligned
     * SSE store (movaps) the compiler emits for a stack local raised #GP. */
    uint64_t sp = g_bootstrap_pcb.kstack_top & ~(uint64_t)0xF;  /* 16-aligned */
    __asm__ volatile(
        "mov %[sp], %%rsp\n"
        "call *%[fn]\n"
        :
        : [sp]"r"(sp), [fn]"r"(fn)
        : "memory", "cc"
    );
    __builtin_unreachable();
}

void sched_cpu_init(uint32_t cpu_id) {
    /* APs call this to finish per-CPU init */
    g_current[cpu_id] = &g_idle_pcb[cpu_id];
}

/* Insert pcb into g_runq[pcb->cpu] without locking.
 * Caller MUST already hold that run queue's lock.  Split out of
 * sched_enqueue() because sched_yield()/sched_tick() call this while
 * already holding rq->lock — calling the locking sched_enqueue() there
 * would re-acquire the same non-recursive spinlock and deadlock forever. */
static void sched_enqueue_locked(pcb_t *pcb) {
    pcb->state = PROC_READY;

    runq_t *rq = &g_runq[pcb->cpu];
    switch (pcb->sched_class) {
    case SCHED_RT:
        rb_insert(&rq->rt_tree, &pcb->rq_node, pcb->priority);
        rq->rt_count++;
        break;
    case SCHED_INTR:
        rb_insert(&rq->intr_tree, &pcb->rq_node, pcb->priority);
        rq->intr_count++;
        break;
    default: /* SCHED_BATCH */
        rb_insert(&rq->batch_tree, &pcb->rq_node, pcb->priority);
        rq->batch_count++;
        break;
    }
}

void sched_enqueue(pcb_t *pcb) {
    /* Pick CPU: affinity first, else least-loaded */
    uint32_t cpu = pcb->cpu;
    if (cpu >= g_cpu_count) {
        /* Find least-loaded CPU */
        uint32_t min_load = 0xFFFFFFFFU;
        cpu = 0;
        for (uint32_t i = 0; i < g_cpu_count; i++) {
            uint32_t load = g_runq[i].rt_count +
                            g_runq[i].intr_count +
                            g_runq[i].batch_count;
            if (load < min_load) { min_load = load; cpu = i; }
        }
    }
    pcb->cpu = (uint8_t)cpu;

    runq_t *rq = &g_runq[cpu];
    spin_lock(&rq->lock);
    sched_enqueue_locked(pcb);
    spin_unlock(&rq->lock);
}

/* Remove a READY process from its run queue (caller holds rq->lock). */
static void sched_takeoff_runq_locked(runq_t *rq, pcb_t *pcb) {
    rb_root_t *tree;
    uint32_t  *count;

    if (!pcb || pcb->state != PROC_READY)
        return;

    switch (pcb->sched_class) {
    case SCHED_RT:
        tree = &rq->rt_tree; count = &rq->rt_count; break;
    case SCHED_INTR:
        tree = &rq->intr_tree; count = &rq->intr_count; break;
    default:
        tree = &rq->batch_tree; count = &rq->batch_count; break;
    }

    if (*count == 0 || !tree->root)
        return;

    if (pcb->rq_node.parent || tree->root == &pcb->rq_node) {
        rb_remove_node(tree, &pcb->rq_node);
        (*count)--;
        pcb->rq_node.left = pcb->rq_node.right = pcb->rq_node.parent = NULL;
    }
}

pcb_t *sched_dequeue(runq_t *rq) {
    rb_node_t *n = NULL;

    /* RT > INTR > BATCH */
    if (rq->rt_count > 0) {
        n = rb_leftmost(&rq->rt_tree);
        if (n) { rb_remove_node(&rq->rt_tree, n); rq->rt_count--; }
    } else if (rq->intr_count > 0) {
        n = rb_leftmost(&rq->intr_tree);
        if (n) { rb_remove_node(&rq->intr_tree, n); rq->intr_count--; }
    } else if (rq->batch_count > 0) {
        n = rb_leftmost(&rq->batch_tree);
        if (n) { rb_remove_node(&rq->batch_tree, n); rq->batch_count--; }
    }

    if (!n) return rq->idle_pcb;
    return RB_PCB(n);
}

extern void sched_context_save(cpu_context_t *ctx);
extern void sched_context_restore(cpu_context_t *ctx) __attribute__((noreturn));
/* Atomic save-prev/restore-next switch.  Saves the current context with
 * rip = its own return address, so a thread resumed through here returns
 * normally to sched_switch_to's caller instead of re-running a stale
 * restore (which would ping-pong two cooperative threads forever). */
extern void sched_switch_context(cpu_context_t *prev, cpu_context_t *next);

static void sched_switch_to(pcb_t *next);

static void sched_switch_to(pcb_t *next) {
    uint32_t cpu = this_cpu_id();
    pcb_t   *cur = g_current[cpu];
    runq_t  *rq  = &g_runq[cpu];

    if (!next || !next->regs.rip)
        return;
    if (cur == next)
        return;

    /* Disable interrupts for the whole switch.  The cooperative path
     * (sched_yield from PID1) runs with IF=1, and this routine updates
     * g_current[cpu]=next BEFORE the actual register switch below; a PIT IRQ
     * landing in that window would see cur=next, decide to preempt, and
     * re-enter sched_switch_to on a half-done switch → corruption / freeze.
     * sched_switch_context saves rflags with IF forced on, so the thread we
     * switch to (and cur, when later resumed) still run interruptible. */
    __asm__ volatile("cli");

    /* Program preemption timer */
    uint64_t quantum = 0;
    switch (next->sched_class) {
    case SCHED_RT:    quantum = 0;                     break;
    case SCHED_INTR:  quantum = SCHED_QUANTUM_INTR_NS; break;
    default:          quantum = SCHED_QUANTUM_BATCH_NS; break;
    }
    if (quantum > 0) {
        next->deadline_ns = asd_time_ns() + quantum;
        lapic_program_oneshot(quantum);
    }

    if (cur != g_reap_waiter && cur->state == PROC_RUN)
        cur->state = PROC_READY;
    next->state = PROC_RUN;
    g_current[cpu] = next;
    rq->current    = next;

    sched_state_update();
    syscall_on_thread_switch(next);

    /* Do NOT switch CR3 here.  sched_switch_context() saves cur's context by
     * reading the live %cr3; if we loaded next's CR3 first, cur->regs.cr3 would
     * be clobbered with next's address space.  cur would then be resumed later
     * with the wrong CR3 (e.g. a user thread restored under the kernel page
     * tables → its user code/stack unmapped → #GP on the sysret back to ring 3).
     * sched_context_restore (the tail of sched_switch_context) loads next's CR3
     * from next->regs.cr3 for us. */

    /* Single atomic switch: save cur (resuming == returning from this call)
     * and restore next.  Must NOT be two separate save/restore calls — that
     * would resume cur mid-function and re-restore a stale 'next'. */
    sched_switch_context(&cur->regs, &next->regs);
}

void sched_tick(uint64_t now_ns) {
    uint32_t cpu = this_cpu_id();
    if (cpu >= g_cpu_count)
        return;
    runq_t  *rq  = &g_runq[cpu];
    pcb_t   *cur = g_current[cpu];
    if (!cur || !rq->idle_pcb)
        return;

    /* Parent is blocked in sched_reap() waiting for a child — do not preempt
     * the child mid-run (otherwise the child sits READY and the parent never
     * resumes from sched_switch_to). */
    if (g_reap_waiter)
        return;

    /* PID 1 init runs outside the run queue; never preempt it from the PIT. */
    if (cur == &g_bootstrap_pcb)
        return;

    /* Decay INTR priority */
    if (cur->sched_class == SCHED_INTR) {
        int16_t delta = cur->base_pri - cur->priority;
        cur->priority += delta / 10;
    }

    /* Check if quantum expired */
    if (cur != rq->idle_pcb && now_ns >= cur->deadline_ns) {
        /* Re-enqueue and switch */
        if (cur->state == PROC_RUN) {
            spin_lock(&rq->lock);
            sched_enqueue_locked(cur);   /* lock already held */
            pcb_t *next = sched_dequeue(rq);
            spin_unlock(&rq->lock);
            sched_switch_to(next);
        }
    }

    /* PIT IRQ is acknowledged via pic_eoi() in idt.c; LAPIC MMIO is not mapped. */
    (void)now_ns;
}

void sched_yield(void) {
    uint32_t cpu = this_cpu_id();
    runq_t  *rq  = &g_runq[cpu];
    pcb_t   *cur = g_current[cpu];

    spin_lock(&rq->lock);
    /* Only re-enqueue a thread that is actually RUNNABLE.  A thread that just
     * set itself PROC_WAIT (blocking in port_recv/net_recv) and then yielded
     * must NOT be put back on the run queue, or it busy-polls forever instead
     * of sleeping — which starves the GUI of CPU and makes rendering crawl.
     * sched_wake() re-enqueues it when its event arrives. */
    if (cur != rq->idle_pcb && cur != g_reap_waiter && cur->state == PROC_RUN)
        sched_enqueue_locked(cur);   /* lock already held */
    pcb_t *next = sched_dequeue(rq);
    spin_unlock(&rq->lock);
    sched_switch_to(next);
}

void sched_wake(pcb_t *pcb) {
    if (pcb->state != PROC_WAIT) return;
    pcb->wake_ns = asd_time_ns();
    /* Apply I/O boost for interactive processes */
    if (pcb->sched_class == SCHED_INTR) {
        pcb->priority = (int16_t)(pcb->base_pri - SCHED_BOOST);
        if (pcb->priority < 0) pcb->priority = 0;
    }
    sched_enqueue(pcb);
}

pid_t sched_spawn(const spawn_args_t *args) {
    spin_lock(&g_proc_table_lock);

    if (g_proc_count >= MAX_PROCS) {
        spin_unlock(&g_proc_table_lock);
        return 0;
    }

    /* Find a free slot */
    uint32_t pid = g_next_pid;
    while (g_proc_table[pid % MAX_PROCS].state != PROC_DEAD &&
           g_proc_table[pid % MAX_PROCS].pid != 0) {
        pid++;
        if (pid == g_next_pid) { spin_unlock(&g_proc_table_lock); return 0; }
    }
    g_next_pid = (pid + 1) % MAX_PROCS;
    if (g_next_pid == 0) g_next_pid = 1;

    pcb_t *pcb = &g_proc_table[pid % MAX_PROCS];

    /* Zero and fill */
    for (uint64_t i = 0; i < sizeof(pcb_t); i++) ((uint8_t *)pcb)[i] = 0;
    pcb->pid         = (pid_t)pid;
    pcb->state       = PROC_NEW;
    pcb->sched_class = args->sched_class;
    pcb->priority    = (int16_t)args->priority;
    pcb->base_pri    = (int16_t)args->priority;
    pcb->cpu         = (args->cpu_affinity == 0xFF) ?
                        (uint8_t)(g_cpu_count) /* trigger load balance */ :
                        args->cpu_affinity;

    /* Name from binary path (last component) */
    const char *n = args->binary;
    const char *p = n;
    while (*p) { if (*p == '/') n = p + 1; p++; }
    for (int i = 0; i < SCHED_NAME_LEN - 1 && n[i]; i++) pcb->name[i] = n[i];

    /* Inherit CWD, ppid, and fd_table from parent */
    {
        pcb_t *parent = sched_current();
        if (parent) {
            pcb->ppid = parent->pid;
            /* CWD */
            if (parent->cwd[0] == '/') {
                int ci = 0;
                while (ci < 255 && parent->cwd[ci]) { pcb->cwd[ci] = parent->cwd[ci]; ci++; }
                pcb->cwd[ci] = '\0';
            } else {
                pcb->cwd[0] = '/'; pcb->cwd[1] = '\0';
            }
            /* fd_table: inherit parent's open fds (including pipe ends for redirection).
             * Increment refcount on each borrowed vfsnode so close() is safe. */
            extern void vfs_fd_inherit(struct pcb *child, struct pcb *parent);
            vfs_fd_inherit(pcb, parent);
        } else {
            pcb->ppid = 1;
            pcb->cwd[0] = '/'; pcb->cwd[1] = '\0';
        }
    }

    if (sched_kstack_alloc(pcb) != 0) {
        spin_unlock(&g_proc_table_lock);
        return 0;
    }

    g_proc_count++;
    spin_unlock(&g_proc_table_lock);

    /* Use pre-built vmap if provided (from elf_spawn), otherwise allocate new */
    if (args->va_map) {
        pcb->va_map = args->va_map;
    } else {
        pcb->va_map = amm_vmap_create();
    }
    pcb->regs.rflags = 0x202;
    /* Default to kernel selectors; elf_spawn overrides to user selectors
     * (0x1B / 0x23) for ring-3 processes. */
    pcb->regs.cs = 0x08;   /* SEG_KERNEL_CODE */
    pcb->regs.ss = 0x10;   /* SEG_KERNEL_DATA */
    pcb->state = PROC_NEW; /* elf_spawn sets rip/rsp/cr3 then enqueues */
    pcb->deadline_ns = UINT64_MAX;

    return (pid_t)pid;
}

void sched_exit(int code) {
    uint32_t cpu = this_cpu_id();
    pcb_t   *cur = g_current[cpu];
    vmap_t  *dead_map = cur->va_map;

    /* Notify parent via SIGCHLD */
    if (cur->ppid > 0) {
        pcb_t *parent = sched_find(cur->ppid);
        if (parent && parent->state != PROC_DEAD)
            __atomic_fetch_or(&parent->sig_pending, 1u << SIGCHLD, __ATOMIC_RELEASE);
    }

    /* Foreground exec from sched_reap(): return to parent via jmp, not full
     * context restore (bootstrap parent was never sched_context_save'd). */
    if (g_reap_jmp_active && g_reap_waiter) {
        cur->exit_code = code;
        cur->state     = PROC_DEAD;
        cur->va_map    = NULL;
        vfs_close_all(cur);
        spin_lock(&g_proc_table_lock);
        g_proc_count--;
        spin_unlock(&g_proc_table_lock);
        sched_load_cr3(g_reap_waiter->regs.cr3);
        if (dead_map)
            amm_vmap_destroy(dead_map);
        if (cur->kstack && cur->kstack != g_bootstrap_kstack && cur->pid != 0) {
            kfree(cur->kstack);
            cur->kstack     = NULL;
            cur->kstack_top = 0;
        }
        g_current[cpu]         = g_reap_waiter;
        g_runq[cpu].current    = g_reap_waiter;
        g_reap_waiter->state   = PROC_RUN;
        g_reap_jmp_active = 0;
        syscall_on_thread_switch(g_reap_waiter);
        sched_jmp_restore(&g_reap_jmp, 1);
    }

    cur->exit_code = code;
    cur->state     = PROC_DEAD;
    cur->va_map    = NULL;
    vfs_close_all(cur);

    spin_lock(&g_proc_table_lock);
    g_proc_count--;
    spin_unlock(&g_proc_table_lock);

    /* Switch to next process */
    runq_t *rq = &g_runq[cpu];
    pcb_t *next = NULL;

    if (g_reap_waiter && g_reap_waiter->state != PROC_DEAD) {
        next = g_reap_waiter;
        g_reap_waiter = NULL;
    } else {
        spin_lock(&rq->lock);
        next = sched_dequeue(rq);
        spin_unlock(&rq->lock);
    }

    /* FIX (v5): sched_dequeue may return NULL when the run queue is empty
     * (e.g. the last user process is exiting).  Fall back to idle_pcb
     * unconditionally in that case to avoid a NULL dereference. */
    if (!next || next->state == PROC_DEAD) next = rq->idle_pcb;
    
    rq->current    = next;
    g_current[cpu] = next;
    next->state    = PROC_RUN;

    sched_state_update();
    syscall_on_thread_switch(next);

    /* Leave the dying address space before freeing its page tables. */
    sched_load_cr3(next->regs.cr3);
    if (dead_map)
        amm_vmap_destroy(dead_map);

    sched_context_restore(&next->regs);
}

int sched_reap(pid_t pid, exit_info_t *info) {
    pcb_t *pcb = sched_find(pid);
    if (!pcb) return -1;

    uint32_t cpu = this_cpu_id();
    runq_t  *rq  = &g_runq[cpu];
    pcb_t   *waiter = sched_current();

    /* Save the outer reap context so foreground reaps can NEST: the shell reaps
     * `do`, then `do` itself reaps `apm`.  With single globals the inner reap
     * would clobber the outer's longjmp target and the shell would never
     * resume — i.e. `do <anything>` hangs the machine. */
    pcb_t          *prev_reap_waiter = g_reap_waiter;
    sched_jmpbuf_t  prev_reap_jmp    = g_reap_jmp;
    int             prev_reap_active = g_reap_jmp_active;

    g_reap_waiter      = waiter;
    g_reap_jmp_active  = 1;

    sched_class_t saved_class = pcb->sched_class;
    int16_t       saved_pri   = pcb->priority;

    if (sched_jmp_save(&g_reap_jmp) == 0) {
        if (pcb->state == PROC_READY && pcb->regs.rip) {
            spin_lock(&rq->lock);
            sched_takeoff_runq_locked(rq, pcb);
            spin_unlock(&rq->lock);
            /* Do NOT boost the foreground child to SCHED_RT.  As the sole RT
             * task it would re-select itself on every sched_yield() (BATCH GUI
             * tasks never run), so a child that yields while waiting on I/O
             * (e.g. `apm` reading an HTTP response, which calls asd_yield()) hangs
             * the whole machine.  Keeping its original BATCH class lets it
             * round-robin cooperatively with ws/dock and make progress. */
            g_current[cpu] = pcb;
            rq->current    = pcb;
            pcb->state     = PROC_RUN;
            syscall_on_thread_switch(pcb);
            sched_load_cr3(pcb->regs.cr3);
            sched_context_restore(&pcb->regs);
        }
        sched_jmp_restore(&g_reap_jmp, 1);
    }

    /* sched_exit() is invoked from SYSCALL context where MSR_FMASK cleared IF.
     * sched_jmp_restore() does not restore RFLAGS, so IF stays 0 on return.
     * The kernel shell loop requires IRQs for PS/2 kbd — re-enable them here. */
    __asm__ volatile("sti" ::: "memory");

    syscall_on_thread_switch(waiter);
    console_drain_rx();

    pcb->sched_class  = saved_class;
    pcb->priority     = saved_pri;

    if (pcb->state != PROC_DEAD) {
        pcb->state     = PROC_DEAD;
        pcb->exit_code = -1;
    }

    /* Restore the outer reap context (re-entrancy — supports nested reaps). */
    g_reap_waiter     = prev_reap_waiter;
    g_reap_jmp        = prev_reap_jmp;
    g_reap_jmp_active = prev_reap_active;

    if (info) {
        info->exit_code = pcb->exit_code;
        info->cpu_ns    = pcb->cpu_ns;
        info->wall_ns   = 0; /* TODO: track wall time */
    }
    if (pcb->kstack && pcb->kstack != g_bootstrap_kstack && pcb->pid != 0) {
        kfree(pcb->kstack);
        pcb->kstack = NULL;
        pcb->kstack_top = 0;
    }
    /* va_map is destroyed in sched_exit() after CR3 switch; do not free it
     * here — the child may still be running on its page tables (timeout). */

    pcb->pid   = 0;
    pcb->state = PROC_DEAD;
    return 0;
}

int sched_set(pid_t pid, sched_class_t cls, int priority) {
    pcb_t *pcb = sched_find(pid);
    if (!pcb) return -1;
    pcb->sched_class = cls;
    pcb->base_pri    = (int16_t)priority;
    pcb->priority    = (int16_t)priority;
    return 0;
}

void sched_steal(uint32_t this_cpu) {
    uint32_t victim = 0;
    uint32_t max_load = 0;

    for (uint32_t i = 0; i < g_cpu_count; i++) {
        if (i == this_cpu) continue;
        uint32_t load = g_runq[i].intr_count + g_runq[i].batch_count;
        if (load > max_load) { max_load = load; victim = i; }
    }

    uint32_t my_load = g_runq[this_cpu].intr_count +
                       g_runq[this_cpu].batch_count;
    if (max_load <= my_load + SCHED_STEAL_THRESHOLD) return;

    /* Lock both queues in CPU-ID order */
    uint32_t first  = (this_cpu < victim) ? this_cpu : victim;
    uint32_t second = (this_cpu < victim) ? victim   : this_cpu;
    spin_lock(&g_runq[first].lock);
    spin_lock(&g_runq[second].lock);

    /* Move half of victim's non-RT processes */
    uint32_t to_move = (g_runq[victim].intr_count +
                        g_runq[victim].batch_count) / 2;
    for (uint32_t i = 0; i < to_move; i++) {
        rb_node_t *n = NULL;
        if (g_runq[victim].batch_count > 0)
            n = rb_leftmost(&g_runq[victim].batch_tree);
        else if (g_runq[victim].intr_count > 0)
            n = rb_leftmost(&g_runq[victim].intr_tree);
        if (!n) break;

        pcb_t *p = RB_PCB(n);
        /* Remove from victim */
        if (p->sched_class == SCHED_BATCH) {
            rb_remove_node(&g_runq[victim].batch_tree, n);
            g_runq[victim].batch_count--;
        } else {
            rb_remove_node(&g_runq[victim].intr_tree, n);
            g_runq[victim].intr_count--;
        }
        /* Insert into this CPU */
        p->cpu = (uint8_t)this_cpu;
        if (p->sched_class == SCHED_BATCH) {
            rb_insert(&g_runq[this_cpu].batch_tree, n, p->priority);
            g_runq[this_cpu].batch_count++;
        } else {
            rb_insert(&g_runq[this_cpu].intr_tree, n, p->priority);
            g_runq[this_cpu].intr_count++;
        }
    }

    spin_unlock(&g_runq[second].lock);
    spin_unlock(&g_runq[first].lock);
}

void sched_state_update(void) {
    /* Seqlock write: increment to odd */
    __atomic_fetch_add(&g_sched_state.seq, 1, __ATOMIC_RELEASE);

    g_sched_state.total_procs = g_proc_count;
    for (uint32_t i = 0; i < g_cpu_count; i++) {
        g_sched_state.cpus[i].rt_count    = g_runq[i].rt_count;
        g_sched_state.cpus[i].intr_count  = g_runq[i].intr_count;
        g_sched_state.cpus[i].batch_count = g_runq[i].batch_count;
        g_sched_state.cpus[i].current_pid = g_current[i] ?
                                            g_current[i]->pid : 0;
        g_sched_state.cpus[i].idle_ns     = g_runq[i].idle_ns;
        g_sched_state.cpus[i].busy_ns     = g_runq[i].total_ns;
    }

    /* Increment to even */
    __atomic_fetch_add(&g_sched_state.seq, 1, __ATOMIC_RELEASE);
}

runq_t *sched_runq(uint32_t cpu_id) {
    return (cpu_id < g_cpu_count) ? &g_runq[cpu_id] : NULL;
}

pcb_t *sched_find(pid_t pid) {
    if (pid == 0 || pid >= MAX_PROCS) return NULL;
    pcb_t *p = &g_proc_table[pid % MAX_PROCS];
    return (p->pid == pid && p->state != PROC_DEAD) ? p : NULL;
}

pcb_t *sched_current(void) {
    return g_current[this_cpu_id()];
}
