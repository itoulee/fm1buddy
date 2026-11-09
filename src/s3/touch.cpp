// The board's touch panel (touch.h), over the one I2C bus each board has.
#include "touch.h"

#include <Arduino.h>
#include <Wire.h>
#include <driver/gpio.h>

#include "hal.h"

// the panel takes a moment to wake: its readings are empty until then
static uint32_t readyAt;

#ifdef SCREEN_ST77922

// --------------------------------------------------------------------------------- the 3.5" (its panel) ---
// The FNK0104N's touch controller is part of the panel (a Sitronix TDDI at 0x55): 16-bit registers, big-endian, and
// a touch report per finger (7 bytes: its flags, X, Y, ...) once the touch-info register says there are coordinates.
#define ADDR 0x55
#define REG_TOUCH_INFO 0x0010
#define REG_POINTS 0x0014
#define REG_MAX_TOUCHES 0x0009
#define REPORT 7
#define MAX_POINTS 10  // (which the panel has at most)

static uint8_t maxTouches = 2;
static bool asked;  // (the panel can still be starting when the app is: ask again until it answers)

static bool reg(uint16_t r, uint8_t* d, size_t n) {
  Wire.beginTransmission(ADDR);
  Wire.write(r >> 8);
  Wire.write(r & 0xFF);
  if (Wire.endTransmission(false) != 0 || Wire.requestFrom((uint8_t)ADDR, (uint8_t)n) != n) {
    return false;
  }
  for (size_t i = 0; i < n; i++) {
    d[i] = Wire.read();
  }
  return true;
}

void touch::begin() {
  gpio_hold_dis((gpio_num_t)TOUCH_RST);  // (held through a sleep: hal::sleep)
  gpio_hold_dis((gpio_num_t)TFT_BL);
  pinMode(TOUCH_INT, INPUT);
  pinMode(TOUCH_RST, OUTPUT);
  digitalWrite(TOUCH_RST, 0);
  delay(100);
  digitalWrite(TOUCH_RST, 1);
  readyAt = millis() + 300;
  Wire.begin(TOUCH_SDA, TOUCH_SCL, 400000);
}

int touch::read(Point* out, int max) {
  if ((int32_t)(millis() - readyAt) < 0) {
    return 0;  // (the panel is still waking)
  }
  if (!asked) {
    uint8_t n = 0;
    if (reg(REG_MAX_TOUCHES, &n, 1) && n && n <= MAX_POINTS) {
      maxTouches = n;
      asked = true;
      hal::log("touch: the panel's, %u points", (unsigned)n);
    }
  }
  uint8_t info;
  if (!reg(REG_TOUCH_INFO, &info, 1) || !(info & 0x08)) {
    return 0;  // (no new coordinates: the fingers are as they were)
  }
  uint8_t r[REPORT * MAX_POINTS];
  const int n = max < maxTouches ? max : maxTouches;
  if (!reg(REG_POINTS, r, (size_t)maxTouches * REPORT)) {
    return 0;
  }
  int got = 0;
  for (int i = 0; i < maxTouches && got < n; i++) {
    const uint8_t* p = r + i * REPORT;
    if (!(p[0] & 0x80)) {
      continue;  // (not a finger)
    }
    Point& f = out[got++];
    f.x = (int16_t)((p[0] & 0x3F) << 8 | p[1]);
    f.y = (int16_t)(p[2] << 8 | p[3]);
    f.id = (uint8_t)i;
    f.weight = 32;  // (the panel leaves these out)
    f.area = 2;
  }
  return got;
}

void touch::sleep() {
  // (the touch controller is part of the panel: display::sleep's SLPIN has it too)
}

bool touch::regGet(uint8_t, uint8_t&) {
  return false;
}

bool touch::regSet(uint8_t, uint8_t) {
  return false;
}

#else

// -------------------------------------------------------------------------------------- the 2.8" and 4" ---
// The FT6336: its TD_STATUS, then per finger XH (its event in the top 2 bits) XL YH (its id in the top 4) YL, WEIGHT,
// MISC (area in the top 4)
#define ADDR 0x38

void touch::begin() {
  gpio_hold_dis((gpio_num_t)TOUCH_RST);  // (held through a sleep: hal::sleep)
  gpio_hold_dis((gpio_num_t)TFT_BL);
  pinMode(TOUCH_INT, INPUT);
  pinMode(TOUCH_RST, OUTPUT);
  digitalWrite(TOUCH_RST, 0);
  delay(10);
  digitalWrite(TOUCH_RST, 1);
  readyAt = millis() + 300;
  Wire.begin(TOUCH_SDA, TOUCH_SCL, 400000);
}

int touch::read(Point* out, int max) {
  if ((int32_t)(millis() - readyAt) < 0) {
    return 0;  // (the panel is still waking)
  }
  uint8_t r[13];
  Wire.beginTransmission(ADDR);
  Wire.write(0x02);
  if (Wire.endTransmission(false) || Wire.requestFrom(ADDR, 13) != 13) {
    return 0;
  }
  for (uint8_t& b : r) {
    b = Wire.read();
  }
  const uint8_t n = r[0] & 0x0F;
  int got = 0;
  for (int i = 0; i < n && i < 2 && got < max; i++) {
    const uint8_t* p = r + 1 + i * 6;
    if (p[0] >> 6 == 1) {
      continue;  // lifted
    }
    Point& f = out[got++];
    f.x = (int16_t)((p[0] & 0x0F) << 8 | p[1]);  // the panel's portrait pixels
    f.y = (int16_t)((p[2] & 0x0F) << 8 | p[3]);
    f.id = p[2] >> 4;
    f.weight = p[4];
    f.area = p[5] >> 4;
  }
  return got;
}

bool touch::regGet(uint8_t r, uint8_t& v) {
  Wire.beginTransmission(ADDR);
  Wire.write(r);
  if (Wire.endTransmission(false) || Wire.requestFrom(ADDR, 1) != 1) {
    return false;
  }
  v = Wire.read();
  return true;
}

bool touch::regSet(uint8_t r, uint8_t v) {
  Wire.beginTransmission(ADDR);
  Wire.write(r);
  Wire.write(v);
  return !Wire.endTransmission();
}

void touch::sleep() {
  // its G_MODE (0xA4) to 0: polling, so its interrupt is still there to wake the chip, and no gesture wakes it first
  Wire.beginTransmission(ADDR);
  Wire.write(0xA4);
  Wire.write(0);
  Wire.endTransmission();
}

#endif
