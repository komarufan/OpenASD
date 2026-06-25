/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, ASD Project Contributors
 *
 * PS/2 keyboard driver — IRQ1-driven, ring buffer output.
 */

#include "ps2kbd.h"
#include "../arch/pic.h"
#include <stdint.h>

#define KBD_DATA  0x60
#define KBD_STAT  0x64

#define KBD_BUF   256

static uint8_t g_buf[KBD_BUF];
static volatile uint32_t g_head = 0;
static volatile uint32_t g_tail = 0;

static inline uint8_t io_in8(uint16_t p) {
    uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v;
}

static void buf_push(char c) {
    uint32_t next = (g_head + 1) % KBD_BUF;
    if (next != g_tail) {   /* drop if full */
        g_buf[g_head] = (uint8_t)c;
        g_head = next;
    }
}

/* US layout scancode → ASCII maps */
static const char g_map[128] = {
    [0x01]=0x1b,                                                /* ESC */
    [0x02]='1',[0x03]='2',[0x04]='3',[0x05]='4',[0x06]='5',
    [0x07]='6',[0x08]='7',[0x09]='8',[0x0A]='9',[0x0B]='0',
    [0x0C]='-',[0x0D]='=',[0x0E]='\b',[0x0F]='\t',
    [0x10]='q',[0x11]='w',[0x12]='e',[0x13]='r',[0x14]='t',
    [0x15]='y',[0x16]='u',[0x17]='i',[0x18]='o',[0x19]='p',
    [0x1A]='[',[0x1B]=']',[0x1C]='\n',
    [0x1E]='a',[0x1F]='s',[0x20]='d',[0x21]='f',[0x22]='g',
    [0x23]='h',[0x24]='j',[0x25]='k',[0x26]='l',
    [0x27]=';',[0x28]='\'',[0x29]='`',[0x2B]='\\',
    [0x2C]='z',[0x2D]='x',[0x2E]='c',[0x2F]='v',[0x30]='b',
    [0x31]='n',[0x32]='m',[0x33]=',',[0x34]='.',[0x35]='/',
    [0x39]=' ',
};
static const char g_smap[128] = {
    [0x02]='!',[0x03]='@',[0x04]='#',[0x05]='$',[0x06]='%',
    [0x07]='^',[0x08]='&',[0x09]='*',[0x0A]='(',[0x0B]=')',
    [0x0C]='_',[0x0D]='+',[0x0F]='\t',
    [0x10]='Q',[0x11]='W',[0x12]='E',[0x13]='R',[0x14]='T',
    [0x15]='Y',[0x16]='U',[0x17]='I',[0x18]='O',[0x19]='P',
    [0x1A]='{',[0x1B]='}',[0x1C]='\n',
    [0x1E]='A',[0x1F]='S',[0x20]='D',[0x21]='F',[0x22]='G',
    [0x23]='H',[0x24]='J',[0x25]='K',[0x26]='L',
    [0x27]=':',[0x28]='"',[0x29]='~',[0x2B]='|',
    [0x2C]='Z',[0x2D]='X',[0x2E]='C',[0x2F]='V',[0x30]='B',
    [0x31]='N',[0x32]='M',[0x33]='<',[0x34]='>',[0x35]='?',
    [0x39]=' ',
};

static int g_shift = 0;
static int g_ctrl  = 0;
static int g_ext   = 0;

void ps2kbd_init(void) {
    /* Flush any stale data */
    while (io_in8(KBD_STAT) & 0x01)
        (void)io_in8(KBD_DATA);
    pic_unmask(1);   /* IRQ1 */
}

void ps2kbd_isr(void) {
    if (!(io_in8(KBD_STAT) & 0x01)) return;
    uint8_t sc = io_in8(KBD_DATA);
    ps2kbd_isr_scancode(sc);
}

/* Process a single raw scancode through the state machine.
 * Can be called from a polling path as well as from the IRQ handler. */
void ps2kbd_isr_scancode(uint8_t sc) {
    if (sc == 0xE0) { g_ext = 1; return; }

    if (g_ext) {
        g_ext = 0;
        if (sc == 0x48) { buf_push((char)0x80); return; }   /* ↑ */
        if (sc == 0x50) { buf_push((char)0x81); return; }   /* ↓ */
        if (sc == 0x4B) { buf_push((char)0x82); return; }   /* ← */
        if (sc == 0x4D) { buf_push((char)0x83); return; }   /* → */
        if (sc == 0x53) { buf_push((char)0x84); return; }   /* Delete */
        if (sc == 0x49) { buf_push((char)0x85); return; }   /* PgUp */
        if (sc == 0x51) { buf_push((char)0x86); return; }   /* PgDn */
        if (sc == 0x47) { buf_push((char)0x87); return; }   /* Home */
        if (sc == 0x4F) { buf_push((char)0x88); return; }   /* End */
        return;
    }

    /* Ctrl make/break (scancode 0x1D = Left Ctrl) */
    if (sc == 0x1D) { g_ctrl = 1; return; }
    if (sc == 0x9D) { g_ctrl = 0; return; }

    if (sc == 0x2A || sc == 0x36) { g_shift = 1; return; }
    if (sc == 0xAA || sc == 0xB6) { g_shift = 0; return; }
    if (sc & 0x80) return;   /* key release */

    char ch = g_shift ? g_smap[sc] : g_map[sc];
    if (ch) {
        if (g_ctrl && ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 1);  /* ^a=1..^z=26 */
        else if (g_ctrl && ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 1);
        buf_push(ch);
    }
}

int ps2kbd_getc(char *out) {
    if (g_head == g_tail) return 0;
    *out = (char)g_buf[g_tail];
    g_tail = (g_tail + 1) % KBD_BUF;
    return 1;
}
