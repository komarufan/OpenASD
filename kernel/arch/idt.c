/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, ASD Project Contributors
 */

#include "idt.h"
#include "gdt.h"
#include "pic.h"
#include "../mm/mm.h"
#include "../sched/sched.h"
#include "syscall.h"
#include <stdint.h>

void irq_user_fault_exit(void) __attribute__((noreturn));

typedef struct {
    uint16_t off0;
    uint16_t sel;
    uint8_t  ist;
    uint8_t  type;    /* 0x8E = P|DPL0|64-bit interrupt gate */
    uint16_t off1;
    uint32_t off2;
    uint32_t _res;
} __attribute__((packed)) idt_gate_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) idtr_t;

static idt_gate_t g_idt[256] __attribute__((aligned(16)));

/* Stubs array defined in isr.S */
extern void *isr_stubs[256];

static void set_gate(uint8_t vec, void *handler) {
    uintptr_t off = (uintptr_t)handler;
    g_idt[vec].off0 = (uint16_t)(off);
    g_idt[vec].sel  = SEG_KERNEL_CODE;
    g_idt[vec].ist  = 0;
    g_idt[vec].type = 0x8E;
    g_idt[vec].off1 = (uint16_t)(off >> 16);
    g_idt[vec].off2 = (uint32_t)(off >> 32);
    g_idt[vec]._res = 0;
}

void idt_init(void) {
    for (int i = 0; i < 256; i++)
        set_gate((uint8_t)i, isr_stubs[i]);

    idtr_t idtr = { sizeof(g_idt) - 1, (uint64_t)(uintptr_t)g_idt };
    __asm__ volatile("lidt %0" :: "m"(idtr) : "memory");
}

/* ------------------------------------------------------------------ */

static inline uint8_t p_in8(uint16_t p) {
    uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v;
}
static inline void p_out8(uint16_t p, uint8_t v) {
    __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));
}
static void kputs(const char *s) {
    while (*s) {
        int n = 100000;
        while (n-- && !(p_in8(0x3F8+5) & 0x20));
        p_out8(0x3F8, (uint8_t)*s++);
    }
}
static void kputx(uint64_t v) {
    static const char h[] = "0123456789ABCDEF";
    char buf[19] = "0x0000000000000000";
    for (int i = 17; i >= 2; i--) { buf[i] = h[v & 0xF]; v >>= 4; }
    kputs(buf);
}

static const char *g_exc[32] = {
    "#DE",  "#DB",  "NMI",  "#BP",  "#OF",  "#BR",  "#UD",  "#NM",
    "#DF",  "FPU",  "#TS",  "#NP",  "#SS",  "#GP",  "#PF",  "---",
    "#MF",  "#AC",  "#MC",  "#XM",  "#VE",  "#CP",  "---",  "---",
    "---",  "---",  "---",  "---",  "#HV",  "#VC",  "#SX",  "---",
};

/* Declared in their respective drivers */
extern void pit_isr(void);
extern void ps2kbd_isr(void);
extern void ps2mouse_isr(void);
extern void virtio_net_rx_poll(void);
extern uint8_t net_get_irq(void);

static int isr_frame_user(const isr_frame_t *f) {
    return f && ((f->cs & 3u) != 0);
}

void irq_user_fault_exit(void) {
    sched_exit(-1);
}

static void kill_user_fault(isr_frame_t *f, const char *name) {
    kputs("\n[user] ");
    kputs(name);
    { pcb_t *c = sched_current(); if (c) { kputs(" proc="); kputs(c->name); } }
    kputs(" rip=");
    kputx(f->rip);
    kputs(" rsp=");
    kputx(f->rsp);
    kputs("\n");
    /* Cannot sched_exit() directly from an ISR — iret to a kernel trampoline. */
    f->rip    = (uint64_t)(uintptr_t)irq_user_fault_exit;
    f->cs     = 0x08;
    f->ss     = 0x10;
    f->rsp    = syscall_kernel_rsp0();
    f->rflags = 0x202;
}

/* FIX: Handle #PF (vector 14) via amm_page_fault instead of panicking.
 * This enables demand-paging for user-space ELF segments and stacks.
 * If the fault address is not in any mapped region, we still panic. */
#define VEC_PAGE_FAULT  14

void isr_dispatch(isr_frame_t *f) {
    uint64_t v = f->vector;

    /* EOI BEFORE pit_isr(): sched_tick() inside pit_isr() may switch context
     * via sched_switch_context (which jumps to the next thread and never
     * returns here), so a trailing pic_eoi(0) would be skipped, leaving IRQ0
     * stuck in-service and the timer permanently dead. Acking first is safe —
     * the switch runs with interrupts disabled (see sched_switch_to). */
    if (v == VEC_PIT_TIMER) { pic_eoi(0); pit_isr(); return; }
    if (v == VEC_KBD)       { ps2kbd_isr(); pic_eoi(1); return; }
    if (v == VEC_MOUSE)     { ps2mouse_isr(); pic_eoi(12); return; }
    if (v == VEC_SPURIOUS)  { return; }   /* no EOI for spurious IRQ7 */

    /* virtio-net IRQ (dynamic, determined at init time) */
    {
        uint8_t net_irq = net_get_irq();
        if (net_irq < 16 && v == (uint64_t)(0x20 + net_irq)) {
            virtio_net_rx_poll();
            pic_eoi((int)net_irq);
            return;
        }
    }

    /* Other slave IRQs */
    if (v >= 0x28 && v <= 0x2F) { pic_eoi((int)(v - 0x20)); return; }

    /* Unhandled master IRQs */
    if (v >= 0x20 && v <= 0x27) { pic_eoi((int)(v - 0x20)); return; }

    /* #PF: demand-paging handler */
    if (v == VEC_PAGE_FAULT) {
        uint64_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        int is_write = (int)(f->error_code & 0x2);
        /* Try the current process's vmap first */
        pcb_t *cur = sched_current();
        if (cur && cur->va_map) {
            if (amm_page_fault(cur->va_map, (vaddr_t)cr2, is_write) == 0)
                return; /* handled — retry the faulting instruction */
        }
        if (isr_frame_user(f)) {
            kill_user_fault(f, "#PF");
            return; /* iret → irq_user_fault_exit → sched_exit */
        }
        kputs("\n*** EXCEPTION #PF err="); kputx(f->error_code);
        kputs(" cr2=");  kputx(cr2);
        kputs(" rip=");  kputx(f->rip);
        kputs(" rsp=");  kputx(f->rsp);
        kputs("\n");
        for (;;) __asm__ volatile("cli; hlt");
    }

    /* CPU exceptions — panic in kernel; terminate process in ring 3 */
    if (v < 32) {
        if (isr_frame_user(f)) {
            kill_user_fault(f, g_exc[v]);
            return;
        }
        kputs("\n*** EXCEPTION ");
        kputs(g_exc[v]);
        kputs(" err="); kputx(f->error_code);
        kputs(" rip="); kputx(f->rip);
        kputs(" rsp="); kputx(f->rsp);
        kputs("\n");
        for (;;) __asm__ volatile("cli; hlt");
    }
}
