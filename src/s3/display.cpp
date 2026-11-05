// The board's screen (display.h): two ways in, one interface.
#include "display.h"

#include <Arduino.h>
#include <driver/spi_master.h>
#include <esp_heap_caps.h>

#include "hal.h"

#ifdef SCREEN_ST77922

// --------------------------------------------------------------------------------------- the 3.5" (QSPI) ---
// The Freenove FNK0104N's panel: an ST77922, 320x480 portrait, on four data lines (QSPI). Its commands and its
// colour data go to a 24-bit address: an opcode, then the command number in the address's top byte (OP_CMD), or the
// colour's (OP_COLOR). LVGL draws landscape and this turns each area a quarter turn on its way to the panel: the
// panel's own axis swap is not reliable on these panels, so MADCTL stays portrait (0x00).
#define NATIVE_W SCREEN_H  // the panel's own picture: 320x480, portrait
#define NATIVE_H SCREEN_W

#define OP_CMD 0x02    // a command, with its parameters as the data
#define OP_COLOR 0x32  // colour data, into RAMWRC (0x3C)
#define CHUNK 32768    // a QSPI write at most: its bounce buffer, in internal RAM for the DMA

// The panel's settings, as Freenove's own driver (ST77922.cpp) has them: 56 commands, and the vendor's 20 ms after the
// last (TEON). The ones a driver has to add itself — invert, colour format, MADCTL, sleep out, display on — are in
// panelInit.
struct Init {
  uint8_t cmd;
  uint8_t len;
  uint8_t data[16];  // (the longest of them takes 16)
};
static const Init INIT[] = {
    {0xF1, 1, {0x00}},
    {0x60, 3, {0x00, 0x00, 0x00}},
    {0x65, 1, {0x80}},
    {0x79, 1, {0x06}},
    {0x7B, 3, {0x00, 0x08, 0x08}},
    {0x80, 11, {0x55, 0x62, 0x2F, 0x17, 0xF0, 0x52, 0x70, 0xD2, 0x52, 0x62, 0xEA}},
    {0x81, 4, {0x26, 0x52, 0x72, 0x27}},
    {0x84, 2, {0x92, 0x25}},
    {0x87, 6, {0x10, 0x10, 0x58, 0x00, 0x02, 0x3A}},
    {0x88, 15, {0x00, 0x00, 0x2C, 0x10, 0x04, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x06}},
    {0x89, 3, {0x00, 0x00, 0x00}},
    {0x8A, 11, {0x13, 0x00, 0x2C, 0x00, 0x00, 0x2C, 0x10, 0x10, 0x00, 0x3E, 0x19}},
    {0x8B, 9, {0x15, 0xB1, 0xB1, 0x44, 0x96, 0x2C, 0x10, 0x97, 0x8E}},
    {0x8C, 13, {0x1D, 0xB1, 0xB1, 0x44, 0x96, 0x2C, 0x10, 0x50, 0x0F, 0x01, 0xC5, 0x12, 0x09}},
    {0x8D, 1, {0x0C}},
    {0x8E, 6, {0x33, 0x01, 0x0C, 0x13, 0x01, 0x01}},
    {0xB3, 2, {0x00, 0x30}},
    {0xF1, 1, {0x00}},
    {0x71, 1, {0xD0}},
    {0x66, 2, {0x02, 0x3F}},
    {0xBE, 3, {0x26, 0x00, 0x9D}},
    {0x70, 12, {0x01, 0xA0, 0x11, 0x40, 0xE0, 0x00, 0x11, 0x69, 0x11, 0x00, 0x00, 0x1A}},
    {0x90, 9, {0x04, 0x04, 0x55, 0x74, 0x00, 0x40, 0x43, 0x27, 0x27}},
    {0x91, 9, {0x04, 0x04, 0x55, 0x75, 0x00, 0x40, 0x42, 0x27, 0x27}},
    {0x92, 10, {0x04, 0x44, 0x55, 0xC0, 0x06, 0x00, 0x07, 0x05, 0x90, 0x27}},
    {0x93, 10, {0x04, 0x43, 0x11, 0x00, 0x00, 0x00, 0x00, 0x05, 0x90, 0x27}},
    {0x94, 6, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {0x95, 5, {0x96, 0x16, 0x00, 0x00, 0xFF}},
    {0x96, 12, {0x44, 0x53, 0x03, 0x12, 0x23, 0x24, 0x06, 0x05, 0x94, 0x27, 0x00, 0x44}},
    {0x97, 12, {0x44, 0x53, 0x47, 0x56, 0x20, 0x20, 0x02, 0x01, 0x94, 0x27, 0x00, 0x44}},
    {0xBA, 5, {0x55, 0x94, 0x2D, 0x94, 0x27}},
    {0x9A, 7, {0x40, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00}},
    {0x9B, 7, {0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x00}},
    {0x9C, 13, {0x5C, 0x12, 0x00, 0x00, 0x10, 0x12, 0x00, 0x00, 0x10, 0x02, 0x00, 0x00, 0x00}},
    {0x9D, 8, {0x8A, 0x51, 0x00, 0x00, 0x00, 0x80, 0x1E, 0x01}},
    {0x9E, 7, {0x51, 0x00, 0x00, 0x00, 0x80, 0x1E, 0x01}},
    {0xB4, 12, {0x1D, 0x1C, 0x1E, 0x0B, 0x14, 0x02, 0x13, 0x09, 0x1E, 0x00, 0x1E, 0x10}},
    {0xB5, 12, {0x1D, 0x1C, 0x1E, 0x0A, 0x15, 0x03, 0x11, 0x08, 0x1E, 0x01, 0x1E, 0x12}},
    {0xB6, 7, {0x77, 0x77, 0x00, 0x0A, 0xFF, 0x0A, 0xFF}},
    {0x86, 14, {0xCD, 0x04, 0xB1, 0x02, 0x58, 0x12, 0x58, 0x0C, 0x13, 0x01, 0xA5, 0x00, 0xA5, 0xA5}},
    {0xB7, 16, {0x07, 0x0A, 0x0E, 0x06, 0x05, 0x03, 0x2B, 0x03, 0x03, 0x42, 0x07, 0x10, 0x10, 0x2E, 0x3F, 0x0D}},
    {0xB8, 16, {0x07, 0x0A, 0x0D, 0x05, 0x05, 0x02, 0x2B, 0x02, 0x03, 0x42, 0x06, 0x10, 0x0F, 0x2E, 0x3F, 0x0D}},
    {0xB9, 2, {0x23, 0x23}},
    {0xBF, 6, {0x10, 0x14, 0x14, 0x0B, 0x0B, 0x0B}},
    {0xF2, 1, {0x00}},
    {0x73, 5, {0x04, 0xDA, 0x12, 0x54, 0x47}},
    {0x77, 5, {0x6B, 0x5B, 0xFD, 0xC3, 0xC5}},
    {0x7A, 2, {0x15, 0x27}},
    {0x7B, 2, {0x04, 0x57}},
    {0x7E, 2, {0x01, 0x0E}},
    {0xBF, 1, {0x36}},
    {0xE3, 2, {0x40, 0x40}},
    {0xF0, 1, {0x00}},
    {0xD0, 1, {0x00}},
    {0x35, 1, {0x01}},  // TEON
};

static spi_device_handle_t dev;
static uint16_t* bounce;  // a QSPI write's bytes, in the panel's order
alignas(4) static uint8_t param[16];

// a command, its parameters as the data: after the opcode, the command number is the address's top byte
static void cmd(uint8_t c, const uint8_t* d, size_t n) {
  spi_transaction_ext_t t;
  memset(&t, 0, sizeof t);
  t.base.flags = SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR;
  t.command_bits = 8;
  t.address_bits = 24;
  t.base.cmd = OP_CMD;
  t.base.addr = (uint32_t)c << 8;
  t.base.length = n * 8;
  if (n) {  // (from a RAM copy: the table is in flash, where the SPI can't read it)
    memcpy(param, d, n);
    t.base.tx_buffer = param;
  }
  spi_device_polling_transmit(dev, (spi_transaction_t*)&t);
}

// the window the colours go into: its columns in the panel's own (portrait) pixels
static void window(int x1, int y1, int x2, int y2) {
  const uint8_t ca[4] = {(uint8_t)(x1 >> 8), (uint8_t)x1, (uint8_t)(x2 >> 8), (uint8_t)x2};
  const uint8_t ra[4] = {(uint8_t)(y1 >> 8), (uint8_t)y1, (uint8_t)(y2 >> 8), (uint8_t)y2};
  cmd(0x2A, ca, sizeof ca);  // CASET
  cmd(0x2B, ra, sizeof ra);  // RASET
}

// colour data, on the four lines (the command and its address went on one)
static void push(const uint16_t* px, size_t bytes) {
  spi_transaction_ext_t t;
  memset(&t, 0, sizeof t);
  t.base.flags = SPI_TRANS_MODE_QIO | SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR;
  t.command_bits = 8;
  t.address_bits = 24;
  t.base.cmd = OP_COLOR;
  t.base.addr = 0x3C << 8;  // RAMWRC
  t.base.length = bytes * 8;
  t.base.tx_buffer = px;
  spi_device_polling_transmit(dev, (spi_transaction_t*)&t);
}

static void panelInit() {
  delay(200);  // (the panel settles: on a warm restart it otherwise doesn't start)
  for (const Init& c : INIT) {
    cmd(c.cmd, c.data, c.len);
  }
  cmd(0x21, nullptr, 0);        // INVON: without it the colours are inverted
  const uint8_t colmod = 0x01;  // RGB565, as Freenove's driver sets it
  cmd(0x3A, &colmod, 1);
  const uint8_t madctl = 0x00;  // portrait, RGB order: LVGL turns the picture (flush)
  cmd(0x36, &madctl, 1);
  cmd(0x11, nullptr, 0);  // sleep out
  delay(120);
  cmd(0x29, nullptr, 0);  // display on
  delay(20);              // (the vendor waits 20 ms after TEON: INIT's last)
}

// the panel's window starts on a multiple of 4 columns (its dual-gate driving): grow what LVGL draws to 4-px steps
static void rounder(lv_event_t* e) {
  lv_area_t* a = (lv_area_t*)lv_event_get_param(e);
  a->x1 &= ~3, a->y1 &= ~3;
  a->x2 |= 3, a->y2 |= 3;
  if (a->x2 >= SCREEN_W)
    a->x2 = SCREEN_W - 1;  // (both are multiples of 4: still aligned)
  if (a->y2 >= SCREEN_H)
    a->y2 = SCREEN_H - 1;
}

static void flush(lv_display_t* d, const lv_area_t* a, uint8_t* px) {
  const int w = lv_area_get_width(a);
  const uint16_t* src = (const uint16_t*)px;
  lv_area_t n = *a;
  lv_display_rotate_area(d, &n);  // the panel's own pixels: portrait
  const int nw = lv_area_get_width(&n), rows = CHUNK / 2 / nw;
  for (int y = n.y1; y <= n.y2; y += rows) {
    const int last = y + rows - 1 < n.y2 ? y + rows - 1 : n.y2;
    uint16_t* dst = bounce;
    for (int yn = y; yn <= last; yn++) {
      for (int xn = n.x1; xn <= n.x2; xn++) {
        // the panel's (xn, yn) is the screen's (yn, SCREEN_H - 1 - xn): a quarter turn (LV_DISPLAY_ROTATION_270)
        const uint16_t c = src[(SCREEN_H - 1 - xn - a->y1) * w + (yn - a->x1)];
        *dst++ = (uint16_t)(c >> 8 | c << 8);  // the panel takes each pixel's high byte first
      }
    }
    window(n.x1, y, n.x2, last);
    push(bounce, (size_t)(last - y + 1) * nw * 2);
  }
  lv_display_flush_ready(d);
}

lv_display_t* display::begin() {
  hal::backlight(0);  // (dark until the app's frame is drawn: ui::frame)
  bounce = (uint16_t*)heap_caps_malloc(CHUNK, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

  spi_bus_config_t bus = {};
  bus.data0_io_num = LCD_D0;
  bus.data1_io_num = LCD_D1;
  bus.data2_io_num = LCD_D2;
  bus.data3_io_num = LCD_D3;
  bus.sclk_io_num = LCD_SCK;
  bus.max_transfer_sz = CHUNK;
  bus.flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_QUAD | SPICOMMON_BUSFLAG_IOMUX_PINS;
  if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) {
    hal::log("screen: the QSPI bus didn't start");
  }
  spi_device_interface_config_t cfg = {};
  cfg.mode = 0;
  cfg.clock_speed_hz = 40000000;  // (the controller's write clock stops at 62.5 MHz: 40 is the next step under it)
  cfg.spics_io_num = LCD_CS;
  cfg.queue_size = 1;
  cfg.flags = SPI_DEVICE_HALFDUPLEX;
  if (spi_bus_add_device(SPI2_HOST, &cfg, &dev) != ESP_OK) {
    hal::log("screen: the QSPI device didn't start");
  }
  panelInit();

  lv_display_t* disp = lv_display_create(NATIVE_W, NATIVE_H);
  lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_270);
  size_t n = (size_t)NATIVE_W * NATIVE_H * 2;  // the whole picture, in the PSRAM (a flush is never split)
  void* buf = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
  if (!buf) {  // (no PSRAM: a few rows at a time)
    n = (size_t)NATIVE_W * 40 * 2;
    buf = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
  }
  lv_display_set_buffers(disp, buf, NULL, n, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(disp, flush);
  lv_display_add_event_cb(disp, rounder, LV_EVENT_INVALIDATE_AREA, nullptr);
  return disp;
}

void display::sleep() {
  cmd(0x28, nullptr, 0);  // display off
  cmd(0x10, nullptr, 0);  // sleep in
}

bool display::rotated() {
  return true;
}

void display::toScreen(int px, int py, int& x, int& y) {
  x = py;
  y = SCREEN_H - 1 - px;  // (the quarter turn LVGL makes of the panel's picture: LV_DISPLAY_ROTATION_270)
}

void display::fromScreen(int x, int y, int& px, int& py) {
  px = SCREEN_H - 1 - y;
  py = x;
}

#else

// ------------------------------------------------------------------------------ the 2.8" and the 4" (SPI) ---
#include <TFT_eSPI.h>

static TFT_eSPI tft;

static void flush(lv_display_t* d, const lv_area_t* a, uint8_t* px) {
  uint32_t w = lv_area_get_width(a), h = lv_area_get_height(a);
  tft.startWrite();
  tft.setAddrWindow(a->x1, a->y1, w, h);
  tft.pushColors((uint16_t*)px, w * h, true);
  tft.endWrite();
  lv_display_flush_ready(d);
}

lv_display_t* display::begin() {
  tft.init();
  hal::backlight(0);   // (TFT_eSPI lit it: dark until the app's frame is drawn, ui::frame)
  tft.setRotation(1);  // (the panel is portrait: a quarter turn, in the screen's controller)
  lv_display_t* disp = lv_display_create(SCREEN_W, SCREEN_H);
  const size_t n = 320 * 40 * 2;  // a buffer of 12 KB (about 40 rows of the 2.8", 27 of the 4"), as internal RAM has
  void* a = heap_caps_malloc(n, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  void* b = heap_caps_malloc(n, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  lv_display_set_buffers(disp, a, b, n, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(disp, flush);
  return disp;
}

void display::sleep() {
  tft.writecommand(0x28);  // display off
  tft.writecommand(0x10);  // sleep in
}

bool display::rotated() {
  return false;
}

void display::toScreen(int px, int py, int& x, int& y) {
  x = py;
  y = SCREEN_H - 1 - px;  // (the panel is portrait: turned as the screen is, tft.setRotation(1))
}

void display::fromScreen(int x, int y, int& px, int& py) {
  px = SCREEN_H - 1 - y;
  py = x;
}

#endif
