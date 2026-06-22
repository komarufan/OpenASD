/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, ASD Project Contributors
 *
 * ASD syscall numbers and low-level wrappers.
 * Included by userland programs — no kernel headers needed.
 * Must stay in sync with kernel/arch/syscall.h.
 */

#ifndef LIBASD_SYSCALL_H
#define LIBASD_SYSCALL_H

#include <stdint.h>
#include <stddef.h>
#include "types.h"

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

/* Extended (v5) */
#define SYS_GETTIME_NS 27
#define SYS_SETUID    28
#define SYS_SETGID    29

/* Network (v6) */
#define SYS_NET_SEND  30   /* (dst_ip, dst_port, src_port, buf, len) → bytes sent */
#define SYS_NET_RECV  31   /* (buf, buf_sz, src_ip_ptr, src_port_ptr) → bytes recv */
#define SYS_NET_PING  32   /* (dst_ip_host_order, rtt_ms_ptr) → 0 or -1 */

/* CWD (v7) */
#define SYS_CHDIR     33   /* (path) → 0 or -errno */
#define SYS_GETCWD    34   /* (buf, size) → len or -errno */

/* Signals and IPC (v8) */
#define SYS_KILL      35   /* (pid, sig) → 0 or -errno */
#define SYS_PIPE      38   /* (int fds[2]) → 0 or -errno */
#define SYS_DUP2      39   /* (oldfd, newfd) → newfd or -errno */

/* TCP client (v9) */
#define SYS_TCP_CONNECT  40  /* (ip, port, *conn_id) → 0 or -errno */
#define SYS_TCP_SEND     41  /* (id, buf, len) → bytes or -errno */
#define SYS_TCP_RECV     42  /* (id, buf, cap, blocking) → bytes or -errno */
#define SYS_TCP_CLOSE    43  /* (id) → 0 */
#define SYS_DNS_RESOLVE  44  /* (hostname, *ip_out) → 0 or -errno */

/* Signal numbers (minimal set for v1) */
#define SIGTERM        1
#define SIGKILL        9
#define SIGCHLD       17

/* ------------------------------------------------------------------ */
/* Open flags                                                           */
/* ------------------------------------------------------------------ */

#define O_RDONLY    0x01
#define O_WRONLY    0x02
#define O_RDWR      0x03
#define O_CREAT     0x10
#define O_TRUNC     0x20
#define O_APPEND    0x40
#define O_DIRECTORY 0x80

/* Legacy aliases used by older code in this tree */
#define O_READ   O_RDONLY
#define O_WRITE  O_WRONLY
#define O_DIR    O_DIRECTORY

/* ------------------------------------------------------------------ */
/* Seek whence                                                          */
/* ------------------------------------------------------------------ */

#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2

/* ------------------------------------------------------------------ */
/* mmap flags / prot                                                    */
/* ------------------------------------------------------------------ */

#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
#define MAP_ANON      MAP_ANONYMOUS
#define MAP_FAILED    ((void *)-1)

#define PROT_NONE     0x00
#define PROT_READ     0x01
#define PROT_WRITE    0x02
#define PROT_EXEC     0x04

/* ------------------------------------------------------------------ */
/* utsname                                                              */
/* ------------------------------------------------------------------ */

#define ASD_UTSNAME_LEN 65

typedef struct {
    char sysname[ASD_UTSNAME_LEN];
    char nodename[ASD_UTSNAME_LEN];
    char release[ASD_UTSNAME_LEN];
    char version[ASD_UTSNAME_LEN];
    char machine[ASD_UTSNAME_LEN];
    char domainname[ASD_UTSNAME_LEN];
} asd_utsname_t;

/* ------------------------------------------------------------------ */
/* iovec for writev                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    void  *iov_base;
    size_t iov_len;
} asd_iovec_t;

/* ------------------------------------------------------------------ */
/* Raw syscall — defined in syscall_asm.S                              */
/* ------------------------------------------------------------------ */

long __syscall(long nr, long a0, long a1, long a2, long a3, long a4);

/* ------------------------------------------------------------------ */
/* Convenience wrappers                                                 */
/* ------------------------------------------------------------------ */

__attribute__((noreturn))
static inline void asd_exit(int code) {
    __syscall(SYS_EXIT, code, 0, 0, 0, 0);
    __builtin_unreachable();
}

static inline int asd_open(const char *path, int flags) {
    return (int)__syscall(SYS_OPEN, (long)path, flags, 0, 0, 0);
}

static inline int asd_close(int fd) {
    return (int)__syscall(SYS_CLOSE, fd, 0, 0, 0, 0);
}

static inline long asd_read(int fd, void *buf, size_t n) {
    return __syscall(SYS_READ, fd, (long)buf, (long)n, 0, 0);
}

static inline long asd_write(int fd, const void *buf, size_t n) {
    return __syscall(SYS_WRITE, fd, (long)buf, (long)n, 0, 0);
}

static inline int asd_stat(const char *path, asd_stat_t *st) {
    return (int)__syscall(SYS_STAT, (long)path, (long)st, 0, 0, 0);
}

static inline long asd_seek(int fd, long off, int whence) {
    return __syscall(SYS_SEEK, fd, off, whence, 0, 0);
}

static inline int asd_mkdir(const char *path) {
    return (int)__syscall(SYS_MKDIR, (long)path, 0, 0, 0, 0);
}

static inline int asd_chdir(const char *path) {
    return (int)__syscall(SYS_CHDIR, (long)path, 0, 0, 0, 0);
}

static inline int asd_getcwd(char *buf, size_t size) {
    return (int)__syscall(SYS_GETCWD, (long)buf, (long)size, 0, 0, 0);
}

static inline int asd_unlink(const char *path) {
    return (int)__syscall(SYS_UNLINK, (long)path, 0, 0, 0, 0);
}

static inline int asd_readdir(const char *path, asd_dirent_t *buf,
                               uint32_t cap, uint32_t *n_out) {
    return (int)__syscall(SYS_READDIR, (long)path, (long)buf,
                          (long)cap, (long)n_out, 0);
}

static inline int asd_spawn(const char *path, const char **argv,
                             const char **envp) {
    return (int)__syscall(SYS_SPAWN, (long)path, (long)argv, (long)envp, 0, 0);
}

static inline int asd_wait(int pid, asd_exit_info_t *info) {
    return (int)__syscall(SYS_WAIT, pid, (long)info, 0, 0, 0);
}

static inline int asd_getpid(void) {
    return (int)__syscall(SYS_GETPID, 0, 0, 0, 0, 0);
}

static inline void asd_yield(void) {
    __syscall(SYS_YIELD, 0, 0, 0, 0, 0);
}

static inline uint64_t asd_time(void) {
    return (uint64_t)__syscall(SYS_TIME, 0, 0, 0, 0, 0);
}

static inline unsigned int asd_getuid(void) {
    return (unsigned int)__syscall(SYS_GETUID, 0, 0, 0, 0, 0);
}

static inline unsigned int asd_getgid(void) {
    return (unsigned int)__syscall(SYS_GETGID, 0, 0, 0, 0, 0);
}

static inline unsigned int asd_geteuid(void) {
    return (unsigned int)__syscall(SYS_GETEUID, 0, 0, 0, 0, 0);
}

static inline unsigned int asd_getegid(void) {
    return (unsigned int)__syscall(SYS_GETEGID, 0, 0, 0, 0, 0);
}

static inline int asd_getppid(void) {
    return (int)__syscall(SYS_GETPPID, 0, 0, 0, 0, 0);
}

static inline int asd_uname(asd_utsname_t *u) {
    return (int)__syscall(SYS_UNAME, (long)u, 0, 0, 0, 0);
}

static inline void *asd_mmap(void *addr, size_t len, int prot, int flags,
                              int fd, long off) {
    (void)off;
    return (void *)__syscall(SYS_MMAP, (long)addr, (long)len,
                             prot, flags, (long)fd);
}

static inline int asd_munmap(void *addr, size_t len) {
    return (int)__syscall(SYS_MUNMAP, (long)addr, (long)len, 0, 0, 0);
}

static inline void *asd_brk(void *new_brk) {
    return (void *)__syscall(SYS_BRK, (long)new_brk, 0, 0, 0, 0);
}

static inline int asd_ioctl(int fd, unsigned long req, void *arg) {
    return (int)__syscall(SYS_IOCTL, fd, (long)req, (long)arg, 0, 0);
}

static inline long asd_writev(int fd, const asd_iovec_t *iov, int iovcnt) {
    return __syscall(SYS_WRITEV, fd, (long)iov, iovcnt, 0, 0);
}

static inline uint64_t asd_gettime_ns(void) {
    return (uint64_t)__syscall(SYS_GETTIME_NS, 0, 0, 0, 0, 0);
}

static inline int asd_setuid(unsigned int uid) {
    return (int)__syscall(SYS_SETUID, (long)uid, 0, 0, 0, 0);
}

static inline int asd_setgid(unsigned int gid) {
    return (int)__syscall(SYS_SETGID, (long)gid, 0, 0, 0, 0);
}

/* TCP and DNS wrappers */
static inline int asd_tcp_connect(uint32_t ip, uint16_t port, int *conn_id) {
    return (int)__syscall(SYS_TCP_CONNECT, (long)ip, (long)port, (long)conn_id, 0, 0);
}
static inline long asd_tcp_send(int id, const void *buf, size_t len) {
    return __syscall(SYS_TCP_SEND, (long)id, (long)buf, (long)len, 0, 0);
}
static inline long asd_tcp_recv(int id, void *buf, size_t cap, int blocking) {
    return __syscall(SYS_TCP_RECV, (long)id, (long)buf, (long)cap, (long)blocking, 0);
}
static inline void asd_tcp_close(int id) {
    __syscall(SYS_TCP_CLOSE, (long)id, 0, 0, 0, 0);
}
static inline int asd_dns_resolve(const char *host, uint32_t *ip_out) {
    return (int)__syscall(SYS_DNS_RESOLVE, (long)host, (long)ip_out, 0, 0, 0);
}

/* Signal and IPC wrappers */
static inline int asd_kill(int pid, int sig) {
    return (int)__syscall(SYS_KILL, (long)pid, (long)sig, 0, 0, 0);
}

static inline int asd_pipe(int fds[2]) {
    return (int)__syscall(SYS_PIPE, (long)fds, 0, 0, 0, 0);
}

static inline int asd_dup2(int oldfd, int newfd) {
    return (int)__syscall(SYS_DUP2, (long)oldfd, (long)newfd, 0, 0, 0);
}

/* Network wrappers */
static inline int asd_net_send(uint32_t dst_ip, uint16_t dst_port, uint16_t src_port,
                               const void *buf, uint16_t len) {
    return (int)__syscall(SYS_NET_SEND, (long)dst_ip, (long)dst_port,
                          (long)src_port, (long)(uintptr_t)buf, (long)len);
}

static inline int asd_net_recv(void *buf, uint16_t buf_sz,
                               uint32_t *src_ip, uint16_t *src_port) {
    return (int)__syscall(SYS_NET_RECV, (long)(uintptr_t)buf, (long)buf_sz,
                          (long)(uintptr_t)src_ip, (long)(uintptr_t)src_port, 0);
}

/* Send ICMP echo to dst_ip (host byte order), fill *rtt_ms on success.
 * Returns 0 on reply received, -1 on timeout. */
static inline int asd_ping(uint32_t dst_ip, uint32_t *rtt_ms) {
    return (int)__syscall(SYS_NET_PING, (long)dst_ip,
                          (long)(uintptr_t)rtt_ms, 0, 0, 0);
}

#endif /* LIBASD_SYSCALL_H */
