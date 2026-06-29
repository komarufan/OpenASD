/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026, ASD Project Contributors
 * All rights reserved.
 *
 * sched_asm.S — context save/restore for asdhed
 *
 * cpu_context_t layout (offsets in bytes, 8 bytes each):
 *   0   rax      8   rbx     16  rcx     24  rdx
 *  32   rsi     40   rdi     48  rbp     56  rsp
 *  64   r8      72   r9      80  r10     88  r11
 *  96   r12    104   r13    112  r14    120  r15
 * 128   rip    136   rflags 144  cr3    152  fs_base
 * 160   cs     168   ss
 */

.text
.code64

/* -------------------------------------------------------------------------
 * sched_context_save(cpu_context_t *ctx)   [rdi = ctx]
 * ---------------------------------------------------------------------- */
.global sched_context_save
.type   sched_context_save, @function
sched_context_save:
    pushq   %rbx
    movq    %rdi, %rbx          /* rbx = ctx */
    movq    %rax,   0(%rbx)
    movq    (%rsp), %rax
    movq    %rax,   8(%rbx)
    movq    %rcx,  16(%rbx)
    movq    %rdx,  24(%rbx)
    movq    %rsi,  32(%rbx)
    xorq    %rax, %rax
    movq    %rax,  40(%rbx)     /* rdi (ctx arg) not preserved */
    movq    %rbp,  48(%rbx)
    leaq    8(%rsp), %rax
    movq    %rax,  56(%rbx)
    movq    %r8,   64(%rbx)
    movq    %r9,   72(%rbx)
    movq    %r10,  80(%rbx)
    movq    %r11,  88(%rbx)
    movq    %r12,  96(%rbx)
    movq    %r13, 104(%rbx)
    movq    %r14, 112(%rbx)
    movq    %r15, 120(%rbx)

    movq    8(%rsp), %rax       /* return address sits above the pushed old_rbx */
    movq    %rax,  128(%rbx)

    pushfq
    popq    %rax
    movq    %rax,  136(%rbx)

    movq    %cr3,  %rax
    movq    %rax,  144(%rbx)

    movl    $0xC0000100, %ecx
    rdmsr
    shlq    $32, %rdx
    orq     %rdx, %rax
    movq    %rax, 152(%rbx)

    xorq    %rax, %rax
    movw    %cs, %ax
    movq    %rax,  160(%rbx)

    xorq    %rax, %rax
    movw    %ss, %ax
    movq    %rax,  168(%rbx)

    popq    %rbx
    retq
.size sched_context_save, . - sched_context_save


/* -------------------------------------------------------------------------
 * sched_context_restore(cpu_context_t *ctx)   [rdi = ctx]   __noreturn__
 * ---------------------------------------------------------------------- */
.global sched_context_restore
.type   sched_context_restore, @function
sched_context_restore:
    movq    144(%rdi), %rax
    movq    %cr3, %rcx
    cmpq    %rcx, %rax
    je      .Lno_cr3_switch
    movq    %rax, %cr3
.Lno_cr3_switch:

    movq    152(%rdi), %rax
    movl    $0xC0000100, %ecx
    movq    %rax, %rdx
    shrq    $32, %rdx
    wrmsr

    movq    56(%rdi), %rsp
    subq    $40, %rsp

    movq    168(%rdi), %rax
    movq    %rax, 32(%rsp)
    movq    56(%rdi), %rax
    movq    %rax, 24(%rsp)
    movq    136(%rdi), %rax
    movq    %rax, 16(%rsp)
    movq    160(%rdi), %rax
    movq    %rax, 8(%rsp)
    movq    128(%rdi), %rax
    movq    %rax, 0(%rsp)

    movq    0(%rdi),  %rax
    movq    8(%rdi),  %rbx
    movq   16(%rdi),  %rcx
    movq   24(%rdi),  %rdx
    movq   32(%rdi),  %rsi
    movq   48(%rdi),  %rbp
    movq   64(%rdi),  %r8
    movq   72(%rdi),  %r9
    movq   80(%rdi),  %r10
    movq   88(%rdi),  %r11
    movq   96(%rdi),  %r12
    movq  104(%rdi),  %r13
    movq  112(%rdi),  %r14
    movq  120(%rdi),  %r15
    movq   40(%rdi),  %rdi

    iretq
.size sched_context_restore, . - sched_context_restore


/* -------------------------------------------------------------------------
 * sched_switch_context(cpu_context_t *prev, cpu_context_t *next)
 *      [rdi = prev (save into), rsi = next (restore from)]   __noreturn__
 *
 * Single-routine context switch.  Unlike calling sched_context_save() and
 * sched_context_restore() as two separate C statements, this saves the
 * CURRENT context with rip = this function's RETURN ADDRESS.  When the
 * saved thread is later restored it therefore returns normally to its own
 * caller (sched_switch_to -> sched_yield / sched_tick) and continues its
 * own code — instead of re-executing a stale "restore(next)" and ping-
 * ponging forever between two cooperative threads.
 *
 * Only callee-saved registers, rsp, rip, rflags, cr3, fs_base and the
 * segment selectors must survive; caller-saved GPRs (rax, rcx, rdx, rsi,
 * rdi, r8-r11) are not relied upon across the call (System V ABI) and are
 * zeroed so a later restore is deterministic.
 * ---------------------------------------------------------------------- */
.global sched_switch_context
.type   sched_switch_context, @function
sched_switch_context:
    /* callee-saved */
    movq    %rbx,   8(%rdi)
    movq    %rbp,  48(%rdi)
    movq    %r12,  96(%rdi)
    movq    %r13, 104(%rdi)
    movq    %r14, 112(%rdi)
    movq    %r15, 120(%rdi)

    /* rsp as seen by the caller after this call returns (pop return addr) */
    leaq    8(%rsp), %rax
    movq    %rax,  56(%rdi)

    /* rip = return address -> resume == "return from sched_switch_context" */
    movq    (%rsp), %rax
    movq    %rax, 128(%rdi)

    /* rflags — force IF=1 (0x200) so a thread resumed after a switch that was
     * performed with interrupts disabled (cli, see sched_switch_to) still runs
     * interruptible.  Without this, a cli'd cooperative switch would resume the
     * thread with IF=0 and preemption would silently die. */
    pushfq
    popq    %rax
    orq     $0x200, %rax
    movq    %rax, 136(%rdi)

    /* cr3 */
    movq    %cr3, %rax
    movq    %rax, 144(%rdi)

    /* fs_base (IA32_FS_BASE) */
    movl    $0xC0000100, %ecx
    rdmsr
    shlq    $32, %rdx
    orq     %rdx, %rax
    movq    %rax, 152(%rdi)

    /* cs / ss */
    xorq    %rax, %rax
    movw    %cs, %ax
    movq    %rax, 160(%rdi)
    xorq    %rax, %rax
    movw    %ss, %ax
    movq    %rax, 168(%rdi)

    /* zero caller-saved slots (not preserved across the call) */
    xorq    %rax, %rax
    movq    %rax,   0(%rdi)     /* rax */
    movq    %rax,  16(%rdi)     /* rcx */
    movq    %rax,  24(%rdi)     /* rdx */
    movq    %rax,  32(%rdi)     /* rsi */
    movq    %rax,  40(%rdi)     /* rdi */
    movq    %rax,  64(%rdi)     /* r8  */
    movq    %rax,  72(%rdi)     /* r9  */
    movq    %rax,  80(%rdi)     /* r10 */
    movq    %rax,  88(%rdi)     /* r11 */

    /* restore 'next' via the shared restore path (rdi = next) */
    movq    %rsi, %rdi
    jmp     sched_context_restore
.size sched_switch_context, . - sched_switch_context


/* -------------------------------------------------------------------------
 * sched_jmpbuf: rbx, rbp, r12, r13, r14, r15, rsp, rip
 *
 * sched_jmp_save(buf) -> 0 first call, non-zero after sched_jmp_restore
 * sched_jmp_restore(buf, val) — does not return
 * ---------------------------------------------------------------------- */
.global sched_jmp_save
.type   sched_jmp_save, @function
sched_jmp_save:
    movq    %rbx, 0(%rdi)
    movq    %rbp, 8(%rdi)
    movq    %r12, 16(%rdi)
    movq    %r13, 24(%rdi)
    movq    %r14, 32(%rdi)
    movq    %r15, 40(%rdi)
    leaq    8(%rsp), %rax
    movq    %rax, 48(%rdi)
    movq    (%rsp), %rax
    movq    %rax, 56(%rdi)
    xorl    %eax, %eax
    retq
.size sched_jmp_save, . - sched_jmp_save

.global sched_jmp_restore
.type   sched_jmp_restore, @function
sched_jmp_restore:
    movl    %esi, %eax
    movq    0(%rdi), %rbx
    movq    8(%rdi), %rbp
    movq    16(%rdi), %r12
    movq    24(%rdi), %r13
    movq    32(%rdi), %r14
    movq    40(%rdi), %r15
    movq    48(%rdi), %rsp
    jmp     *56(%rdi)
.size sched_jmp_restore, . - sched_jmp_restore
