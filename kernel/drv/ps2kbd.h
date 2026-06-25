/*
 * SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026, ASD Project Contributors
 */

#ifndef ASD_PS2KBD_H
#define ASD_PS2KBD_H

#include <stdint.h>

/* Called once after IDT + PIC are ready */
void ps2kbd_init(void);

/* Called from IRQ1 handler in isr_dispatch */
void ps2kbd_isr(void);

/* Process a raw scancode — reusable from polled input paths */
void ps2kbd_isr_scancode(uint8_t sc);

/* Non-blocking: returns 1 and fills *out if a key is available */
int  ps2kbd_getc(char *out);

#endif /* ASD_PS2KBD_H */
