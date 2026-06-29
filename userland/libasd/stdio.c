/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, ASD Project Contributors
 */

#include "include/asd/stdio.h"
#include "include/asd/syscall.h"
#include <stdint.h>
#include <stdarg.h>

void putc(char c) {
    asd_write(STDOUT_FD, &c, 1);
}

void puts(const char *s) {
    if (!s) return;
    const char *p = s;
    while (*p) p++;
    asd_write(STDOUT_FD, s, (size_t)(p - s));
    putc('\n');
}

/* ------------------------------------------------------------------ */
/* vsnprintf                                                            */
/* ------------------------------------------------------------------ */

static void buf_putc(char *dst, size_t *pos, size_t cap, char c) {
    if (*pos + 1 < cap) dst[(*pos)++] = c;
}

static void buf_puts(char *dst, size_t *pos, size_t cap, const char *s) {
    while (*s) buf_putc(dst, pos, cap, *s++);
}

static void buf_putu(char *dst, size_t *pos, size_t cap,
                     uint64_t v, int base, int width, char pad) {
    char tmp[24];
    static const char dig[] = "0123456789abcdef";
    int i = 0;
    if (v == 0) { tmp[i++] = '0'; }
    while (v) { tmp[i++] = dig[v % (uint64_t)base]; v /= (uint64_t)base; }
    /* padding */
    for (int j = i; j < width; j++) buf_putc(dst, pos, cap, pad);
    for (int j = i - 1; j >= 0; j--) buf_putc(dst, pos, cap, tmp[j]);
}

int vsnprintf(char *buf, size_t cap, const char *fmt, va_list ap) {
    size_t pos = 0;
    if (!buf || cap == 0) return 0;

    while (*fmt) {
        if (*fmt != '%') { buf_putc(buf, &pos, cap, *fmt++); continue; }
        fmt++; /* skip '%' */

        char pad = ' ';
        int  width = 0;
        int  is_long = 0;

        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (*fmt++ - '0'); }
        if (*fmt == 'l') { is_long = 1; fmt++; }
        if (*fmt == 'l') { fmt++; } /* ll */

        switch (*fmt++) {
        case 'd': {
            long v = is_long ? va_arg(ap, long) : va_arg(ap, int);
            if (v < 0) { buf_putc(buf, &pos, cap, '-'); v = -v; }
            buf_putu(buf, &pos, cap, (uint64_t)v, 10, width, pad);
            break;
        }
        case 'u':
            buf_putu(buf, &pos, cap,
                     is_long ? (uint64_t)va_arg(ap, unsigned long)
                             : (uint64_t)va_arg(ap, unsigned int),
                     10, width, pad);
            break;
        case 'x': case 'X':
            buf_putu(buf, &pos, cap,
                     is_long ? (uint64_t)va_arg(ap, unsigned long)
                             : (uint64_t)va_arg(ap, unsigned int),
                     16, width, pad);
            break;
        case 'p': {
            uint64_t v = (uint64_t)(uintptr_t)va_arg(ap, void *);
            buf_puts(buf, &pos, cap, "0x");
            buf_putu(buf, &pos, cap, v, 16, 16, '0');
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            buf_puts(buf, &pos, cap, s ? s : "(null)");
            break;
        }
        case 'c':
            buf_putc(buf, &pos, cap, (char)va_arg(ap, int));
            break;
        case '%':
            buf_putc(buf, &pos, cap, '%');
            break;
        default:
            buf_putc(buf, &pos, cap, '?');
            break;
        }
    }
    buf[pos] = '\0';
    return (int)pos;
}

int snprintf(char *buf, size_t n, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(buf, n, fmt, ap);
    va_end(ap);
    return r;
}

int vprintf(const char *fmt, va_list ap) {
    char buf[512];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (n > 0) asd_write(STDOUT_FD, buf, (size_t)n);
    return n;
}

int printf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}

int readline(char *buf, int cap) {
    if (!buf || cap < 2) return 0;
    int i = 0;
    for (;;) {
        char c;
        long r = asd_read(STDIN_FD, &c, 1);
        if (r <= 0) {
            /* No input available yet.  STDIN may be a pipe (terminal) or the
             * keyboard, both of which read non-blocking and return 0 when
             * empty.  Yield and retry so the shell blocks on a full line
             * instead of busy-spinning and reprinting its prompt forever. */
            asd_yield();
            continue;
        }
        if (c == '\n' || c == '\r') { asd_write(STDOUT_FD, "\n", 1); break; }
        if ((c == '\b' || c == 127) && i > 0) {
            i--;
            asd_write(STDOUT_FD, "\b \b", 3);
            continue;
        }
        if (i < cap - 1) {
            buf[i++] = c;
            asd_write(STDOUT_FD, &c, 1);   /* echo */
        }
    }
    buf[i] = '\0';
    return i;
}
