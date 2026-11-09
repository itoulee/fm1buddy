// The board's touch panel: the 2.8"'s and the 4"'s FT6336 (I2C 0x38), and the 3.5"'s touch controller, which is part
// of its ST77922 panel (I2C 0x55, its 16-bit registers). A reading is in the panel's own pixels (portrait on the 3.5")
#pragma once

#include <stdint.h>

namespace touch {
struct Point {
  int16_t x, y;  // the panel's own pixels
  uint8_t id;
  uint8_t weight;
  uint8_t area;
};

// the panel out of reset, and its I2C bus up
void begin();
// the fingers on it now, as many as fit in max
int read(Point* out, int max);
// one of its 8-bit registers (the FT6336's; false on the 3.5"), for the console
bool regGet(uint8_t r, uint8_t& v);
bool regSet(uint8_t r, uint8_t v);
// the panel asleep too, before the chip's deep sleep: its interrupt off (hal::sleep)
void sleep();
}  // namespace touch
