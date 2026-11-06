/* SPDX-License-Identifier: MIT */
/* The loader's commands, on this computer (loader/test.sh): the ones lilfm1buddy's board sends (esp32/rom.cpp), and
 * lilfm1buddy's own: the screen's rows, the sector map (FC47: all 256 sectors, then only what changed), the reset
 * (FC48: its answer first, then the chip restarts), and fm1buddy's theme (FC49: its colours). */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "loader.h"

static uint8_t shown[256];
static unsigned mapCalls, mapLen, rowCalls, resets;
static unsigned rowAt[15];
static uint8_t inBytes; /* (what receive hands out: a count up, so a map's cells are 0, 1, ... one a sector) */

static void screen_map(const uint8_t *s, unsigned n) {
    memcpy(shown, s, n < 256 ? n : 256);
    mapLen = n;
    mapCalls++;
}
static void screen_row(unsigned r, uint16_t fg, uint16_t bg, const char *text, unsigned n) {
    (void)fg, (void)bg, (void)text, (void)n;
    if (rowCalls < 15)
        rowAt[rowCalls] = r;
    rowCalls++;
}
static void reset_(void) { resets++; }
static uint8_t colours[12];
static unsigned paletteAt, paletteLen;
static void screen_palette(unsigned at, const uint8_t *c, unsigned n) {
    memcpy(colours, c, n * 2 < sizeof colours ? n * 2 : sizeof colours);
    paletteAt = at, paletteLen = n;
}
static uint32_t jedec_(void) { return 0x856014; }
static uint16_t key_(void) { return 0x980F; }
static void uid_(uint8_t out[8]) { memset(out, 0x5A, 8); }
static bool screen_init(unsigned board) { return board == 1 || board == 2; }
static bool erase(void *c, uint32_t a, uint32_t n) { (void)c, (void)a, (void)n; return true; }
static bool program(void *c, uint32_t a, const void *d, uint32_t n) { (void)c, (void)a, (void)d, (void)n; return true; }
static bool readf(void *c, uint32_t a, void *o, uint32_t n) { (void)c, (void)a; memset(o, 0, n); return true; }
static int send_(const void *p, uint32_t n) { (void)p, (void)n; return 0; }
static int receive(void *p, uint32_t n) {
    uint8_t *b = p;
    for (uint32_t i = 0; i < n; ++i)
        b[i] = inBytes++;
    return 0;
}

static struct loader_state st;
static uint8_t cbw[31], ans[16];

/* one command: opcode, args at its 3rd byte, what it moves in (in) or out, and the block's transfer count */
static int command(uint16_t op, const uint8_t *args, unsigned nargs, unsigned count, bool in) {
    memset(cbw, 0, sizeof cbw);
    memset(ans, 0, sizeof ans);
    cbw[8] = count & 0xFF, cbw[9] = count >> 8 & 0xFF, cbw[10] = count >> 16 & 0xFF, cbw[11] = count >> 24;
    cbw[12] = in ? 0x80 : 0x00;
    cbw[14] = 16;
    cbw[15] = op >> 8, cbw[16] = op & 0xFF;
    if (nargs)
        memcpy(cbw + 17, args, nargs);
    return loader_dispatch(&st, cbw, ans);
}

int main(void) {
    struct loader_services io = {
        .nor = {{0, 1024 * 1024, readf}, program, erase},
        .send = send_, .receive = receive, .jedec = jedec_, .key = key_, .uid = uid_,
        .screen_init = screen_init, .screen_row = screen_row, .screen_map = screen_map, .reset = reset_,
        .screen_palette = screen_palette,
    };
    loader_init(&st, &io);

    /* the screen: FC43 (its board), FC44 (a row) */
    uint8_t board = 1;
    assert(command(0xFC43, &board, 1, 0, true) == 1 && ans[2] == 1);
    uint8_t row[7] = {0, 0xFF, 0xFF, 0, 0, 0, 5};
    assert(command(0xFC44, row, 7, 5, false) == 1 && rowCalls == 1 && rowAt[0] == 0);

    /* the map: FC47, 256 bytes, then only what changed (the display does that, the dispatch passes it all) */
    inBytes = 0;
    uint8_t map[7] = {0, 0, 0, 0, 0, 1, 0}; /* (its declared length: 256) */
    assert(command(0xFC47, map, 7, 256, false) == 1 && mapCalls == 1 && mapLen == 256);
    assert(shown[0] == 0 && shown[1] == 1 && shown[255] == 255);
    inBytes = 0;
    assert(command(0xFC47, map, 7, 256, false) == 1 && mapCalls == 2);
    assert(command(0xFC47, map, 7, 128, false) == 1 && mapCalls == 2); /* (not its 256: taken, not drawn) */

    /* fm1buddy's theme: FC49, from an index, 6 colours in the command itself (big-endian RGB565), answered like any
     * other; an index past the palette isn't taken */
    uint8_t theme[13] = {2, 0x21, 0x24, 0xFC, 0x00, 0x07, 0xE0, 0x03, 0x20, 0x07, 0xFF, 0xF8, 0x00};
    assert(command(0xFC49, theme, 13, 16, true) == 1 && ans[0] == 0xFC && ans[1] == 0x49);
    assert(paletteAt == 2 && paletteLen == 6 && !memcmp(colours, theme + 1, 12));
    paletteLen = 0;
    theme[0] = 8;
    assert(command(0xFC49, theme, 13, 16, true) == 1 && paletteLen == 0);

    /* the reset: FC48, its answer, then the chip restarts */
    assert(command(0xFC48, 0, 0, 0, true) == 1 && ans[0] == 0xFC && ans[1] == 0x48 && resets == 1);

    /* what the board asks the loader: key, flash, buffer, the error */
    assert(command(0xFC09, 0, 0, 0, true) == 1 && (ans[6] | ans[7] << 8) != 0); /* (its key, scrambled) */
    assert(command(0xFC0A, 0, 0, 0, true) == 1 && ans[2] == 3 && ans[4] == 0x14);
    assert(command(0xFC14, 0, 0, 0, true) == 1 && ans[4] == 0x10);
    assert(command(0xFC46, 0, 0, 0, true) == 1 && ans[2] != 0); /* (the short map's: 7) */
    assert(command(0xFC46, 0, 0, 0, true) == 1 && ans[2] == 0);  /* (and it's cleared) */

    puts("loader: screen, map (256 and not), theme, reset, key, flash, buffer, error: all answered");
    return 0;
}
