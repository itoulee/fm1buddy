// The Tab5's screen (display.h): a 720x1280 MIPI-DSI panel, landscape through M5GFX (its rotation), 1280x720. LVGL
// draws the app at SCREEN_W x SCREEN_H, 640x360, and each of its pixels goes to the panel as 2x2: the app's layout and
// fonts are made for 320x240-480x320 screens, and at 1280x720 they would be tiny
#include <M5Unified.h>
#include <esp_heap_caps.h>

#include "display.h"
#include "hal.h"
#include "tab5.h"

#define SCALE 2
#define ROWS 60  // LVGL's buffer: 60 rows of the app's (a tenth of the screen... six of them)

static lgfx::rgb565_t* big;  // one area, scaled up (SCREEN_W * SCALE x ROWS * SCALE at most)

static void flush(lv_display_t* d, const lv_area_t* a, uint8_t* px) {
  int w = lv_area_get_width(a), h = lv_area_get_height(a);
  const uint16_t* src = (const uint16_t*)px;  // (RGB565, as LVGL has it: the CPU's byte order)
  int bw = w * SCALE;
  for (int y = 0; y < h; y++) {
    uint16_t* row = (uint16_t*)big + (size_t)y * SCALE * bw;
    for (int x = 0; x < w; x++) {
      uint16_t c = src[y * w + x];
      row[x * 2] = c;
      row[x * 2 + 1] = c;
    }
    memcpy(row + bw, row, bw * 2);
  }
  M5.Display.pushImage(a->x1 * SCALE, a->y1 * SCALE, bw, h * SCALE, big);
  lv_display_flush_ready(d);
}

lv_display_t* display::begin() {
  tab5::begin();  // (touch::begin did already)
  lv_display_t* disp = lv_display_create(SCREEN_W, SCREEN_H);
  const size_t n = SCREEN_W * ROWS * 2;
  void* a = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
  void* b = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
  big = (lgfx::rgb565_t*)heap_caps_malloc(n * SCALE * SCALE, MALLOC_CAP_SPIRAM);
  lv_display_set_buffers(disp, a, b, n, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(disp, flush);
  return disp;
}

void display::sleep() {
  M5.Display.sleep();
}

bool display::rotated() {
  return false;  // (M5GFX turns it: its touch readings come turned too)
}

// a touch reading is the panel's pixels as M5GFX turns them (1280x720): the app's are half
void display::toScreen(int px, int py, int& x, int& y) {
  x = px / SCALE;
  y = py / SCALE;
}

void display::fromScreen(int x, int y, int& px, int& py) {
  px = x * SCALE;
  py = y * SCALE;
}
