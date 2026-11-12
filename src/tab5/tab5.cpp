// The Tab5's own parts (tab5.h), and what hal.h has that the Tab5 does differently from the CYD-S3: no deep sleep, no
// FM-1 Recover (its key goes out on the S3's own USB pins), no USB disk or USB MIDI for a computer (the P4's USB-C is
// USB-Serial-JTAG only: a USB device would have to be on the USB-A port, the host's)
#include "tab5.h"

#include <M5Unified.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_private/startup_internal.h>
#include <esp_rom_sys.h>

#include <atomic>
#include <cstring>

#include "board.h"
#include "hal.h"

// ------------------------------------------------------------------------------------- the SPM ---
// ESP-IDF 5.5 (espressif/esp-idf#19020) registers the P4's 8 KB SPM (0x30100000–0x30102000) as generic internal
// heap. Heaps of a matching cap are tried smallest first, so a task stack that fits (the prebuilt Arduino
// CONFIG_ESP_MAIN_TASK_STACK_SIZE is 4096, plus 512: 4608) can land there. spi_flash then asserts because
// esp_ptr_in_dram() does not count SPM as DRAM. pioarduino's prebuilt libfreertos.a / libesp_system.a bake that
// 4096 in; changing sdkconfig would mean Hybrid Compile, which overwrites the machine-wide Arduino libs, and a
// heap-allocated 8 KB+ stack can still fail: the L2MEM "startup stack" heap is only reclaimed after main_task is
// already running. So: (1) intercept xTaskCreatePinnedToCore for "main" and give it a static DRAM stack; (2) at
// ESP_SYSTEM_INIT_FN SECONDARY (after heap_caps_init, before esp_startup_start_app) permanently reserve the SPM
// with MALLOC_CAP_SPM. A C++ constructor cannot be used for (2): it does run before main_task, but
// MALLOC_CAP_INTERNAL|8BIT prefers RETENT DRAM, so the last attempt reserved 0 bytes and left SPM free for the
// stack. Do not use a constructor for this again.
static uint32_t spmTaken;

static bool inSpm(const void* p) {
  return (uintptr_t)p >= 0x30100000 && (uintptr_t)p < 0x30102000;
}

static void takeSpm() {
  if (spmTaken) {
    return;
  }
  // MALLOC_CAP_SPM is the only cap that prefers this 8 KB region (INTERNAL|8BIT is served from DRAM first)
  for (size_t n = 8192; n >= 8; n /= 2) {
    for (;;) {
      void* p = heap_caps_malloc(n, MALLOC_CAP_SPM);
      if (!p) {
        break;
      }
      if (!inSpm(p)) {
        heap_caps_free(p);
        break;
      }
      spmTaken += n;  // (kept for good)
    }
  }
  esp_rom_printf("[tab5] SPM reserved: %u bytes (no task stack there)\n", (unsigned)spmTaken);
}

// After heap init (CORE prio 100) and the other SECONDARY fns (the last stock one is 999), before start_app
ESP_SYSTEM_INIT_FN(tab5_take_spm, SECONDARY, BIT(0), 5000) {
  takeSpm();
  return ESP_OK;
}

// main_task's stack and TCB in DRAM (not the heap, not PSRAM, not SPM). 16 KB: above the 8 KB SPM, and more than
// the prebuilt 4096. The wrap is linked with -Wl,--wrap=xTaskCreatePinnedToCore (platformio.ini, env:tab5)
static constexpr uint32_t kMainStack = 16384;
static DRAM_ATTR StaticTask_t mainTcb;
static DRAM_ATTR StackType_t mainStack[kMainStack / sizeof(StackType_t)] __attribute__((aligned(16)));

extern "C" BaseType_t __real_xTaskCreatePinnedToCore(TaskFunction_t pxTaskCode, const char* const pcName,
                                                     const uint32_t ulStackDepth, void* const pvParameters,
                                                     UBaseType_t uxPriority, TaskHandle_t* const pxCreatedTask,
                                                     const BaseType_t xCoreID);

extern "C" BaseType_t __wrap_xTaskCreatePinnedToCore(TaskFunction_t pxTaskCode, const char* const pcName,
                                                     const uint32_t ulStackDepth, void* const pvParameters,
                                                     UBaseType_t uxPriority, TaskHandle_t* const pxCreatedTask,
                                                     const BaseType_t xCoreID) {
  if (pcName && std::strcmp(pcName, "main") == 0) {
    takeSpm();  // (if the init fn did not run: still do it before the scheduler makes anything else)
    TaskHandle_t h = xTaskCreateStaticPinnedToCore(pxTaskCode, pcName, kMainStack, pvParameters, uxPriority, mainStack,
                                                   &mainTcb, xCoreID);
    if (pxCreatedTask) {
      *pxCreatedTask = h;
    }
    esp_rom_printf("[tab5] main_task static DRAM stack %u B at %p (TCB %p)\n", (unsigned)kMainStack, (void*)mainStack,
                   (void*)&mainTcb);
    return h ? pdTRUE : pdFALSE;
  }
  return __real_xTaskCreatePinnedToCore(pxTaskCode, pcName, ulStackDepth, pvParameters, uxPriority, pxCreatedTask,
                                        xCoreID);
}

uint32_t tab5::spmReserved() {
  return spmTaken;
}

void tab5::stage(const char* what) {
  esp_rom_printf("[tab5 %6u ms] %s\n", (unsigned)millis(), what);
}

static SemaphoreHandle_t i2cMux;
static std::atomic<uint32_t> batMv{0};
static bool started, flip180;

void tab5::begin() {
  if (started) {
    return;
  }
  started = true;
  i2cMux = xSemaphoreCreateMutex();
  char line[96];
  snprintf(line, sizeof line, "begin: SPM reserved %u B, main task stack at %p", (unsigned)spmTaken, (void*)&line);
  stage(line);
  auto cfg = M5.config();
  cfg.serial_baudrate = 0;  // (main.cpp's Serial.begin: the USB-C port's USB-Serial-JTAG)
  cfg.clear_display = true;
  cfg.output_power = false;  // (the USB-A port's 5 V: below, alone)
  cfg.internal_imu = false;
  cfg.internal_rtc = false;
  cfg.internal_spk = false;
  cfg.internal_mic = false;
  cfg.external_imu = false;
  cfg.external_rtc = false;
  cfg.led_brightness = 0;
  stage("M5.begin: start (IO expanders, panel, touch, INA226)");
  M5.begin(cfg);
  snprintf(line, sizeof line, "M5.begin: done, board %d, panel %dx%d", (int)M5.getBoard(), (int)M5.Display.width(),
           (int)M5.Display.height());
  stage(line);
  bool f = false;
  flip180 = hal::load("flip", &f, 1) && f;
  stage("nvs: read");
  M5.Display.setRotation(flip180 ? 3 : 1);  // landscape, 1280x720
  // lit at once, with a line on it: the screen works, whatever comes next (ui::frame sets the app's brightness)
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Display.setTextSize(3);
  M5.Display.setCursor(40, 40);
  M5.Display.print("fm1buddy: starting...");
  M5.Display.setBrightness(160);
  stage("screen: lit");
  // the USB-A port's 5 V (PI4IOE5V6408 0x44, USB5V_EN), for the hub and the FM-1 on it
  bool usb5v = M5.Power.setExtOutput(true, m5::ext_USB);
  stage(usb5v ? "usb-a 5V: on" : "usb-a 5V: FAILED");
  hal::log("tab5: board %d, display %dx%d, USB-A 5V %s", (int)M5.getBoard(), (int)M5.Display.width(),
           (int)M5.Display.height(), usb5v ? "on" : "failed");
}

bool tab5::i2cTake(uint32_t ms) {
  return i2cMux && xSemaphoreTake(i2cMux, pdMS_TO_TICKS(ms)) == pdTRUE;
}

void tab5::i2cGive() {
  xSemaphoreGive(i2cMux);
}

void tab5::batteryPoll() {
  static uint32_t at;
  if (at && millis() - at < 5000) {
    return;
  }
  at = millis();
  if (!i2cTake(5)) {
    return;
  }
  int32_t mv = M5.Power.getBatteryVoltage();  // (the INA226's bus voltage: the NP-F550, two cells)
  i2cGive();
  batMv = mv > 5000 ? (uint32_t)mv : 0;  // (no battery: nothing to show)
}

uint32_t tab5::batteryMv() {
  return batMv;
}

bool tab5::flipped() {
  return flip180;
}

void tab5::flip(bool on) {
  hal::save("flip", &on, 1);
  hal::log("tab5: screen %s: restarting", on ? "turned" : "as it was");
  delay(100);
  ESP.restart();  // (LVGL and the touch panel's mapping start again the new way)
}

// ------------------------------------------------------------------------------------- hal.h, the Tab5's ---
void hal::backlight(uint8_t percent) {
  M5.Display.setBrightness(percent * 255 / 100);  // (PWM on GPIO22: not the I2C bus)
}

uint32_t hal::batteryMv() {
  // the app reads one LiPo cell (4.2 V full): the NP-F550 is two in series
  return tab5::batteryMv() / 2;
}

void hal::sleep() {
  hal::log("sleep: not on the Tab5 (power it off with its button: double-click)");
}

// FM-1 Recover needs the S3's USB pins as GPIOs (the key, key_s3.cpp): the P4's USB-A port is its USB-OTG HS PHY's own
// pins, which can't be. The app doesn't offer it here
void hal::keyStart() {
  hal::log("recover: not on the Tab5");
}

int hal::keyState() {
  return hal::KEY_IDLE;
}

// USB disk and USB MIDI for a computer: not on the Tab5 (the app doesn't offer them here)
void board::diskBegin() {}

int hal::diskState() {
  return hal::DISK_WAITING;
}

void board::computerBegin() {}

bool board::computerOn() {
  return false;
}

uint32_t board::computerOut(const uint8_t*, uint32_t) {
  return 0;
}
