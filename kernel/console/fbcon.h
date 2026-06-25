#ifndef ASD_FBCON_H
#define ASD_FBCON_H

#include "../../boot/asdboot.h"

void fb_console_init(const asd_bib_t *bib);
void fb_console_clear(void);
void fb_console_putc(char c);
void fb_console_puts(const char *s);
void fb_console_tick(void);

/* TUI helpers */
void fb_console_set_colors(uint32_t fg, uint32_t bg);
void fb_console_get_size(uint32_t *cols, uint32_t *rows);
void fb_console_set_cursor(uint32_t col, uint32_t row);
void fb_console_draw_box(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const char *title);
void fb_console_draw_button(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const char *label, int p);

void fb_console_info(uint32_t *w, uint32_t *h, uint32_t *stride, uint32_t *fmt);
void fb_console_blit(const uint32_t *buf, uint32_t x, uint32_t y, uint32_t w, uint32_t h);

#endif
