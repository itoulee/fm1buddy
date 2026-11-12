// The M5Stack Tab5's own parts (ESP32-P4): M5Unified brings up its IO expanders (the ESP32-C6's power, the panel's and
// the touch panel's resets), the MIPI-DSI panel (ILI9881C, ST7123 or ST7121: M5GFX finds which), its touch panel, the
// backlight (PWM) and the INA226 that measures the battery. Everything on the internal I2C bus (touch, INA226, the IO
// expanders) goes through i2c(): the touch task and the app both use it
#pragma once

#include <stdint.h>

namespace tab5 {
// a line on the USB-C port (USB-Serial-JTAG) at once, through the ROM's printf: from the first moments of setup,
// before hal::log knows where its lines go
void stage(const char* what);
// how much of the P4's SPM (8 KB of on-chip RAM at 0x30100000) was taken at start, so no task's stack lands there
uint32_t spmReserved();
// once, before anything else uses the board (touch::begin calls it: it's the first in setup)
void begin();
// the internal I2C bus, for one transaction (or a few): true if it's yours (give it back with i2cGive)
bool i2cTake(uint32_t ms = 50);
void i2cGive();
// the battery's voltage (INA226), read every few seconds by the touch task, in mV: 0 if there's no battery
uint32_t batteryMv();
void batteryPoll();
// the screen turned 180 degrees (Settings: kept), for the Tab5 held the other way up
bool flipped();
void flip(bool on);
}  // namespace tab5
