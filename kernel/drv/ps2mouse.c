/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, ASD Project Contributors
 *
 * PS/2 mouse driver — IRQ12-driven.
 */

#include "ps2mouse.h"
#include "../arch/pic.h"
#include <stdint.h>

#define KBD_DATA  0x60
#define KBD_STAT  0x64
#define KBD_CMD   0x64

static volatile int32_t g_mouse_x = 0;
static volatile int32_t g_mouse_y = 0;
static volatile uint32_t g_mouse_btn = 0;

static uint8_t mouse_cycle = 0;
static uint8_t mouse_packet[3];

static inline uint8_t io_in8(uint16_t p) {
    uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v;
}

static inline void io_out8(uint16_t p, uint8_t v) {
    __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));
}

static void mouse_wait(uint8_t type) {
    uint32_t timeout = 100000;
    if (type == 0) { /* Wait for data */
        while (timeout--) {
            if ((io_in8(KBD_STAT) & 1) == 1) return;
        }
    } else { /* Wait for signal */
        while (timeout--) {
            if ((io_in8(KBD_STAT) & 2) == 0) return;
        }
    }
}

static void mouse_write(uint8_t write) {
    mouse_wait(1);
    io_out8(KBD_CMD, 0xD4);
    mouse_wait(1);
    io_out8(KBD_DATA, write);
    mouse_wait(0);
    io_in8(KBD_DATA); /* Acknowledge */
}

void ps2mouse_init(void) {
    /* Enable auxiliary mouse device */
    mouse_wait(1);
    io_out8(KBD_CMD, 0xA8);

    /* Enable interrupts */
    mouse_wait(1);
    io_out8(KBD_CMD, 0x20);
    mouse_wait(0);
    uint8_t status = io_in8(KBD_DATA) | 2;
    mouse_wait(1);
    io_out8(KBD_CMD, 0x60);
    mouse_wait(1);
    io_out8(KBD_DATA, status);

    /* Enable defaults and packet streaming */
    mouse_write(0xF6);
    mouse_write(0xF4);

    pic_unmask(2);    /* IRQ2 — slave-PIC cascade; without this, IRQ12 (on the
                       * slave) never reaches the CPU and the mouse appears dead
                       * while the keyboard (IRQ1, master) works fine. */
    pic_unmask(12);   /* IRQ12 — PS/2 mouse */
}

void ps2mouse_isr(void) {
    uint8_t status = io_in8(KBD_STAT);
    if (!(status & 0x20)) return; /* Not from mouse */
    
    uint8_t data = io_in8(KBD_DATA);

    switch (mouse_cycle) {
        case 0:
            if ((data & 0x08) == 0) return; /* Sync bit should be 1 */
            mouse_packet[0] = data;
            mouse_cycle++;
            break;
        case 1:
            mouse_packet[1] = data;
            mouse_cycle++;
            break;
        case 2:
            mouse_packet[2] = data;
            mouse_cycle = 0;

            uint32_t btn = mouse_packet[0] & 0x07; /* Left, Right, Middle */
            int32_t dx = mouse_packet[1];
            int32_t dy = mouse_packet[2];

            if (mouse_packet[0] & 0x10) dx -= 256; /* X sign bit */
            if (mouse_packet[0] & 0x20) dy -= 256; /* Y sign bit */

            g_mouse_btn = btn;
            g_mouse_x += dx;
            g_mouse_y -= dy; /* PS/2 Y is bottom-to-top, screen is top-to-bottom */
            /* Free-running accumulator: do NOT clamp here.  The window server
             * tracks the cursor from the *delta* of this value and clamps to the
             * real screen resolution, so clamping here would lose motion at the
             * edges and make the cursor stick. */
            break;
    }
}

void ps2mouse_get_state(int32_t *x, int32_t *y, uint32_t *btn) {
    *x = g_mouse_x;
    *y = g_mouse_y;
    *btn = g_mouse_btn;
}
