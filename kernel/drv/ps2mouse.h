/*
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef ASD_PS2MOUSE_H
#define ASD_PS2MOUSE_H

#include <stdint.h>

void ps2mouse_init(void);
void ps2mouse_isr(void);
void ps2mouse_get_state(int32_t *x, int32_t *y, uint32_t *btn);

#endif
