/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, ASD Project Contributors
 *
 * ASD syscall numbers and kernel-side declarations.
 *
 * The ABI follows the Linux x86-64 syscall convention so that
 * ported programs need minimal stub work:
 *   rax = syscall number
 *   rdi, rsi, rdx, r10, r8, r9 = up to 6 arguments
 *   Return value in rax (negative = errno negated).
 */

#ifndef ASD_SYSCALL_H
#define ASD_SYSCALL_H

#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Syscall numbers                                                      */
/* ------------------------------------------------------------------ */

/* Core I/O */
#define SYS_EXIT      1
#define SYS_OPEN      2
#define SYS_CLOSE     3
#define SYS_READ      4
#define SYS_WRITE     5
#define SYS_STAT      6
#define SYS_SEEK      7
#define SYS_MKDIR     8
#define SYS_UNLINK    9
#define SYS_READDIR   10

/* Process management */
#define SYS_SPAWN     11
#define SYS_WAIT      12
#define SYS_GETPID    13
#define SYS_YIELD     14
#define SYS_TIME      15

/* Identity */
#define SYS_GETUID    16
#define SYS_GETGID    17
#define SYS_GETEUID   18
#define SYS_GETEGID   19
#define SYS_GETPPID   20

/* System info */
#define SYS_UNAME     21

/* Memory management */
#define SYS_MMAP      22
#define SYS_MUNMAP    23
#define SYS_BRK       24

/* Terminal / misc */
#define SYS_IOCTL     25
#define SYS_WRITEV    26

/* Extended syscalls (v5) */
#define SYS_GETTIME_NS 27  /* monotonic nanoseconds since boot */
#define SYS_SETUID    28   /* set real UID (root-only for arbitrary change) */
#define SYS_SETGID    29   /* set real GID */

/* Network syscalls (v6) */
#define SYS_NET_SEND  30   /* send UDP datagram */
#define SYS_NET_RECV  31   /* receive UDP datagram */
#define SYS_NET_PING  32   /* ICMP echo: (dst_ip_be32, rtt_ms_ptr) → 0 or -1 */

/* CWD management */
#define SYS_CHDIR     33   /* chdir(path) → 0 or -errno */
#define SYS_GETCWD    34   /* getcwd(buf, size) → 0 or -errno */

/* Signals and IPC */
#define SYS_KILL      35   /* kill(pid, sig) → 0 or -errno */
#define SYS_PIPE      38   /* pipe(int fds[2]) → 0 or -errno */
#define SYS_DUP2      39   /* dup2(oldfd, newfd) → newfd or -errno */

/* TCP client (v9) */
#define SYS_TCP_CONNECT  40  /* (ip, port, *conn_id) → 0 or -errno */
#define SYS_TCP_SEND     41  /* (id, buf, len) → bytes or -errno */
#define SYS_TCP_RECV     42  /* (id, buf, cap, blocking) → bytes or -errno */
#define SYS_TCP_CLOSE    43  /* (id) → 0 */
#define SYS_DNS_RESOLVE  44  /* (hostname, *ip_out) → 0 or -errno */

/* TLS 1.3 client (v10) */
#define SYS_TLS_CONNECT  45  /* (tcp_id, hostname) → tls_id or -errno */
#define SYS_TLS_SEND     46  /* (id, buf, len) → bytes or -errno */
#define SYS_TLS_RECV     47  /* (id, buf, cap, blocking) → bytes or -errno */
#define SYS_TLS_CLOSE    48  /* (id) → 0 */
#define SYS_MEMINFO      49  /* (*total_kb_out, *used_kb_out) → 0 */
#define SYS_GETUSERNAME  50  /* (buf, size) → 0 or -errno */
#define SYS_FB_INFO      51  /* (*w, *h, *stride, *format) → 0 */
#define SYS_FB_BLIT      52  /* (*buf, x, y, w, h) → 0 */
#define SYS_GET_MOUSE    53  /* (*x, *y, *btn) → 0 */
#define SYS_PORT_CREATE  54  /* (name) → port_id */
#define SYS_PORT_OPEN    55  /* (name) → port_id */
#define SYS_PORT_CLOSE   56  /* (port_id) → 0 */
#define SYS_PORT_SEND    57  /* (port_id, buf, len) → status */
#define SYS_PORT_RECV    58  /* (port_id, buf, cap) → len_out */
#define SYS_KBD_POLL     59  /* () → char (or 0) */

#define SYSCALL_MAX   59

/* Signal numbers (subset — enough for v1) */
#define SIGTERM        1
#define SIGKILL        9
#define SIGCHLD       17

/* ------------------------------------------------------------------ */
/* Open flags (match VFS flags) */
/* ------------------------------------------------------------------ */
#define O_RDONLY   0x01
#define O_WRONLY   0x02
#define O_RDWR     0x03
#define O_CREAT    0x10
#define O_TRUNC    0x20
#define O_APPEND   0x40
#define O_DIRECTORY 0x80

/* ------------------------------------------------------------------ */
/* Seek whence values */
/* ------------------------------------------------------------------ */
#define SEEK_SET   0
#define SEEK_CUR   1
#define SEEK_END   2

/* ------------------------------------------------------------------ */
/* mmap flags */
/* ------------------------------------------------------------------ */
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
#define MAP_ANON      MAP_ANONYMOUS
#ifndef PROT_NONE
#define PROT_NONE     0x00
#endif
#ifndef PROT_READ
#define PROT_READ     0x01
#endif
#ifndef PROT_WRITE
#define PROT_WRITE    0x02
#endif
#ifndef PROT_EXEC
#define PROT_EXEC     0x04
#endif

/* ------------------------------------------------------------------ */
/* Kernel-side API                                                      */
/* ------------------------------------------------------------------ */

/* Set up SYSCALL/SYSRET MSRs — call after GDT is loaded */
void syscall_init(void);

/* Kernel RSP for fault trampoline (see idt.c irq_user_fault_exit). */
uint64_t syscall_kernel_rsp0(void);

/* Point SYSCALL/IRQ kernel stack at the given thread's kstack. */
struct pcb;
void syscall_on_thread_switch(struct pcb *pcb);

/* Discard looped-back COM1 RX bytes after stdout writes (QEMU -serial stdio). */
void console_drain_rx(void);

/* Entry point called from syscall_entry.S */
uint64_t syscall_dispatch(uint64_t nr,
                          uint64_t a0, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4);

/* Called from syscall_entry.S after each syscall — delivers pending signals */
void syscall_check_signals(void);

#endif /* ASD_SYSCALL_H */
