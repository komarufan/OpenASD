/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, ASD Project Contributors
 */

#ifndef ASD_IDT_H
#define ASD_IDT_H

#include <stdint.h>

/* IRQ vectors (after 8259 remap to 0x20-0x2F) */
#define VEC_PIT_TIMER  0x20   /* IRQ0 — PIT */
#define VEC_KBD        0x21   /* IRQ1 - PS/2 keyboard */
#define VEC_MOUSE      0x2C   /* IRQ12 - PS/2 mouse */
#define VEC_SPURIOUS   0x27   /* IRQ7 - spurious */
#define VEC_SLAVE_EOI  0x28   /* IRQ8 — first slave */

/* Interrupt frame pushed by CPU + our ISR stubs (see isr.S) */
typedef struct {
    /* GPRs pushed by stub (high to low as we push them) */
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    /* pushed by stub: vector number, then fake/real error code */
    uint64_t vector;
    uint64_t error_code;
    /* pushed by CPU */
    uint64_t rip, cs, rflags, rsp, ss;
} __attribute__((packed)) isr_frame_t;

void idt_init(void);

/* C-side interrupt dispatcher — called from isr_common in isr.S */
void isr_dispatch(isr_frame_t *f);

#endif /* ASD_IDT_H */
