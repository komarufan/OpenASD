/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, ASD Project Contributors
 *
 * Internal declarations shared across init/ sub-files.
 * Not part of the public kernel interface.
 */

#ifndef INIT_INTERNAL_H
#define INIT_INTERNAL_H

#include "asdinit.h"
#include "../kernel/block/block.h"
#include "../kernel/usr/usr.h"
#include "../kernel/vfs/vfs.h"
#include "../kernel/sched/sched.h"
#include "../kernel/arch/pit.h"
#include "../kernel/arch/macho.h"
#include "../kernel/security/security.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* FNV-1a hash constants */
#define FNV_EMPTY_HASH  14695981039346656037ULL
#define FNV_PRIME       1099511628211ULL

/* COM1 serial port */
#define COM1_PORT  0x3F8

static inline void io_out8(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" :: "a"(val), "d"(port));
}
static inline void io_out16(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" :: "a"(val), "d"(port));
}
static inline uint8_t io_in8(uint16_t port) {
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "d"(port));
    return v;
}

/* Little-endian multi-byte reads */
static inline uint16_t rd16le(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static inline uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t rd64le(const uint8_t *p) {
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) |
           ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) |
           ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) |
           ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}
static inline int i_isspace(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

/* ------------------------------------------------------------------ */
/* Shared globals                                                       */
/* ------------------------------------------------------------------ */

extern char  g_hostname[64];    /* asdinit.c */
extern uid_t g_shell_uid;       /* asdinit.c */
extern gid_t g_shell_gid;       /* asdinit.c */
extern char  g_cwd[];           /* shell.c   */

/* ------------------------------------------------------------------ */
/* Framebuffer console (defined in kernel/console/fbcon.c)             */
/* ------------------------------------------------------------------ */

extern void fb_console_putc(char c);
extern void fb_console_puts(const char *s);
extern void fb_console_clear(void);
extern void fb_console_tick(void);

/* ------------------------------------------------------------------ */
/* I/O subsystem (io.c)                                                */
/* ------------------------------------------------------------------ */

void  serial_port_putc(char c);
void  serial_port_puts(const char *s);
void  serial_putc(char c);
void  serial_puts(const char *s);
void  serial_putu(uint64_t n);
void  put_u64(uint64_t v);
void  put_repeat(char c, int n);
void  box_right_pad(size_t used, size_t inner_w);
int   serial_getc_nonblock(char *out);
int   kbd_getc_nonblock(char *out);
int   input_getc_nonblock(char *out);
void  input_unget(char c);
char  read_char(void);
int   read_menu_key(void);
void  log_msg(const char *msg);
void  log_svc(const char *svc, const char *msg);
void  boot_log(const char *status, const char *msg);
void  print_shell_banner(void);
void  flush_input(void);
char *readline_serial(char *buf, uint32_t cap);
char *readline_serial_noecho(char *buf, uint32_t cap);

/* ------------------------------------------------------------------ */
/* Installer (installer.c)                                             */
/* ------------------------------------------------------------------ */

#define PASSWD_LBA  34ULL

int  installer_run(void);
/* FreeBSD-style: root password and users after base system is on disk. */
int  configure_users_interactive(block_dev_t *target);
block_dev_t *install_get_last_target(void);
int  passwd_db_magic_ok(const uint8_t *sector0);
block_dev_t *find_passwd_disk(void);
int  try_load_users_from_disk(block_dev_t *dev);
int  boot_try_load_passwd_from_dev(block_dev_t *dev);
int  boot_reload_passwd_from_disk(void);
void try_load_hostname_from_disk(block_dev_t *dev);
void boot_load_hostname_from_disk(void);
/* Load #ASDPW1 from the sole block device while IRQs are still masked (entry.c). */
int  kernel_cmdline_has(const char *key);

void boot_early_load_passwd_db(void);
void boot_apply_passwd_buffer(const uint8_t *raw, size_t len);
void boot_vfs_load_passwd_db(void);
int  boot_passwd_was_loaded(void);

/* ------------------------------------------------------------------ */
/* Shell (shell.c)                                                     */
/* ------------------------------------------------------------------ */

void asd_shell_loop(void) __attribute__((noreturn));


/* ------------------------------------------------------------------ */
/* Service manager (svcmgr.c)                                          */
/* ------------------------------------------------------------------ */

void    load_services(void);
int     compute_waves(void);
void    launch_wave(int wave);
void    check_restarts(void);
void    svcmgr_stop_all(void);
uint64_t stub_time_ns(void);

/* ------------------------------------------------------------------ */
/* Shutdown / reboot (asdinit.c)                                       */
/* ------------------------------------------------------------------ */

void asdinit_shutdown(int reboot) __attribute__((noreturn));

#endif /* INIT_INTERNAL_H */
