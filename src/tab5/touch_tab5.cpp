// The Tab5's touch panel (touch.h): GT911 (0x14) on the first units, the ST7123 / ST7121's own (0x55) since; M5GFX
// reads whichever it is, already turned as the screen is (1280x720). Up to two fingers for the app
#include <M5Unified.h>

#include "tab5.h"
#include "touch.h"

void touch::begin() {
  tab5::begin();  // (the first thing setup does: M5Unified, the screen too)
}

int touch::read(Point* out, int max) {
  tab5::batteryPoll();  // (the touch task's: the one other user of the I2C bus, every few seconds)
  if (!tab5::i2cTake(5)) {
    return 0;
  }
  lgfx::touch_point_t tp[2];
  int n = M5.Display.getTouch(tp, max < 2 ? max : 2);
  tab5::i2cGive();
  int k = 0;
  for (int i = 0; i < n; i++) {
    if (tp[i].x < 0 || tp[i].y < 0) {
      continue;
    }
    out[k].x = tp[i].x;
    out[k].y = tp[i].y;
    out[k].id = (uint8_t)tp[i].id;
    out[k].weight = (uint8_t)(tp[i].size > 255 ? 255 : tp[i].size);
    out[k].area = 0;
    k++;
  }
  return k;
}

bool touch::regGet(uint8_t, uint8_t&) {
  return false;
}

bool touch::regSet(uint8_t, uint8_t) {
  return false;
}

void touch::sleep() {}
