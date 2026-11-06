/* SPDX-License-Identifier: MIT */
#ifndef MB_DISPLAY_H
#define MB_DISPLAY_H
#include <stdint.h>
#include <stdbool.h>
bool display_init(unsigned board);
void display_rect(unsigned x, unsigned y, unsigned w, unsigned h, uint16_t color);
void display_row(unsigned row, uint16_t fg, uint16_t bg, const char *text, unsigned n);
void display_text(unsigned row, uint16_t fg, const char *text);
void display_progress(unsigned percent, uint16_t color);
/* lilfm1buddy's: the flash as 16 x 16 sectors, one byte each (LMAP_* in the host); only the cells that changed are drawn */
void display_map(const uint8_t *states, unsigned n);
/* fm1buddy's: its theme's colours, n big-endian RGB565 from index at (0 the background, 1.. the map's states 0..6);
 * until then, ours */
void display_palette(unsigned at, const uint8_t *colours, unsigned n);
void display_release(void);
#endif
