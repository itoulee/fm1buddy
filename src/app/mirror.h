// The FM-1's screen, shown here: firmware with "Big Screen" (pfkellogg's screen_mirror, MIT; mutant.cloud's
// "bigscreen" tag) sends the 240x240 screen over USB MIDI while someone asks for it. We say hello
// (F0 7D 'F' 'S' 00 F7, 01: send it all again) about once a second; it sends each row it paints as
// F0 7D 'F' 'S' 01 row_lo row_hi col_lo col_hi {count c0-6 c7-13 c14-15}... F7. It stops 3 s after
// the last hello
#pragma once

#include <stdint.h>

#define MIRROR_SIZE 240

namespace mirror {
// the screen's page shows (true): hellos go out (from service), and the rows aren't routed to other devices
void on(bool on);
bool active();
// call often (the UI's timer): a hello to each FM-1 on the hub, once a second
void service();
// a complete SysEx from device d (midi.cpp, its lock held): true if it was the screen's (then it's used up)
bool sysex(int d, const uint8_t* m, uint32_t n);
// the screen: MIRROR_SIZE x MIRROR_SIZE RGB565, row by row (0 until a row has come)
const uint16_t* pixels();
// the rows painted since the last look (false if none): first and last
bool dirty(int& y0, int& y1);
// a row came in the last 3 s
bool live();
}  // namespace mirror
