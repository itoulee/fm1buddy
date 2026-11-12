// hal.h on the CYD-S3: the SD card (4-bit SD_MMC, at /sdcard for stdio), NVS for the settings, Wi-Fi, and HTTPS
// with ESP-IDF's certificate bundle
#include <Arduino.h>
#include <Preferences.h>
#include <SD_MMC.h>
#ifdef BOARD_TAB5
#include <SD.h>
#include <SPI.h>
#endif
#include <WiFi.h>
#include <dirent.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_ota_ops.h>
#include <ff.h>
#include <mbedtls/platform.h>
#include <mbedtls/sha256.h>
#ifndef BOARD_TAB5
#include <soc/rtc_cntl_reg.h>
#endif
#include <sys/stat.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "board.h"
#include "hal.h"
#include "jobs.h"
#include "proxy_get.h"
#include "settings.h"
#ifdef BOARD_TAB5
#include <esp_freertos_hooks.h>
#include <esp_rom_sys.h>
#include <soc/assist_debug_reg.h>

#include <atomic>

#include "tab5.h"
#define STAGE(s) tab5::stage(s)
#else
#define STAGE(s)
#endif

#ifdef BOARD_TAB5
static bool sdSpi;  // (the card in SPI mode: SD_MMC didn't find it)
#endif
static bool sdOk, logToSd, computer, disk, recovery;
// USB disk mode (hal.h): set, then a restart. Kept across that restart only (a power cycle starts as ever)
#define DISK_MODE 0x4449534B
static RTC_NOINIT_ATTR uint32_t diskFlag;
// recovery mode (hal.h), the same way
#define RECOVERY_MODE 0x52435652
static RTC_NOINIT_ATTR uint32_t recoveryFlag;
// the console's "host": the next start is the USB host even with a computer on it, for a while (testing)
#define HOST_TEST 0x484F5354
static RTC_NOINIT_ATTR uint32_t hostTest;
static RTC_NOINIT_ATTR char hostScript[120];
static uint32_t hostTestUntil;
#define LOG_FILE "/sdcard/fm1buddy/log.txt"
#define LOG_PREV "/sdcard/fm1buddy/log-1.txt"         // the run before
#define LOG_RECOVERY "/sdcard/fm1buddy/recovery.txt"  // recovery runs, added to: restarts don't take them away
static const char* logFile = LOG_FILE;

// a crash, kept through the restart (RTC memory) and logged then: decode its addresses against the build's
// firmware.elf (xtensa-esp32s3-elf-addr2line -pfiaC -e .pio/build/cyd-s3/firmware.elf ADDR...)
#define CRASH_MAGIC 0x43524153
static RTC_NOINIT_ATTR struct {
  uint32_t magic;
  int core;
  char reason[48];
  uint32_t pc, n;
  uint32_t bt[24];
} crash;

#ifdef BOARD_TAB5
#include <hal/wdt_hal.h>
#include <soc/rtc.h>
#endif

static void onPanic(arduino_panic_info_t* info, void*) {
#ifdef BOARD_TAB5
  // a backstop: the RTC watchdog resets the whole chip in 15 s if the panic handler never gets to its restart (a
  // crash once left the Tab5 on a frozen screen until the USB-C port was opened)
  wdt_hal_context_t rwdt = RWDT_HAL_CONTEXT_DEFAULT();
  wdt_hal_write_protect_disable(&rwdt);
  wdt_hal_config_stage(&rwdt, WDT_STAGE0, 15 * rtc_clk_slow_freq_get_hz(), WDT_STAGE_ACTION_RESET_SYSTEM);
  wdt_hal_enable(&rwdt);
  wdt_hal_write_protect_enable(&rwdt);
#endif
  crash.core = info->core;
  strlcpy(crash.reason, info->reason ? info->reason : "?", sizeof crash.reason);
  crash.pc = (uint32_t)info->pc;
  crash.n = info->backtrace_len < 24 ? info->backtrace_len : 24;
  for (uint32_t i = 0; i < crash.n; i++) {
    crash.bt[i] = info->backtrace[i];
  }
  crash.magic = CRASH_MAGIC;
}

static const char* resetName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "power on";
    case ESP_RST_EXT: return "reset pin";
    case ESP_RST_SW: return "restart (software)";
    case ESP_RST_PANIC: return "crash (panic)";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "SDIO";
    case ESP_RST_USB: return "USB";
    case ESP_RST_JTAG: return "JTAG";
    default: return "?";
  }
}

static void logCrash() {
  if (crash.magic != CRASH_MAGIC || crash.n > 24) {
    crash.magic = 0;
    return;
  }
  crash.magic = 0;
  hal::log("crash: %s, core %d, pc %08X", crash.reason, crash.core, (unsigned)crash.pc);
  for (uint32_t i = 0; i < crash.n; i += 12) {
    char s[12 * 9 + 1];
    uint32_t k = 0;
    for (uint32_t j = i; j < crash.n && j < i + 12; j++) {
      k += snprintf(s + k, sizeof s - k, "%08X ", (unsigned)crash.bt[j]);
    }
    hal::log("crash: backtrace %s", s);
  }
}

// a new log for this run, the last one kept
static bool newLog() {
  remove(LOG_PREV);
  rename(LOG_FILE, LOG_PREV);
  FILE* f = fopen(LOG_FILE, "w");
  if (!f) {
    return false;
  }
  fclose(f);
  return true;
}
static volatile bool wifiUp, radio;

// lines from other tasks wait here for the main loop: only it writes them (to the SD card or the serial port). The
// Tab5: every line waits here, for the log task (below)
#ifdef BOARD_TAB5
#define LOG_Q 256  // (40 KB, in PSRAM)
#else
#define LOG_Q 32
#endif
static char (*logQ)[160];
static uint32_t logW, logR;
static portMUX_TYPE logMux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t mainTask;

// the USB port: 0 while it's not known yet if a computer is on it (board::loop finds out), 1 known, 2 started
// (board::usbBegin)
static int usbState;
#define USB_WAIT 1600  // ms from power-on: a computer on the port has said so by then
static bool hostTestNow;
// lines said before that's known: then they go where the log goes (the SD card, or the serial port)
static std::vector<std::pair<uint32_t, std::string>>* early;

#ifdef BOARD_TAB5
// The Tab5's log: every line (any task's, the main loop's too) goes into logQ, and a task of its own (logTask) writes
// them out: to the USB-C console (Arduino's HWCDC, which never waits: its tx timeout is 0 and it has an 8 KB ring, so
// a computer on the port that isn't reading costs nothing), and to the SD card's log (opened, added to and closed
// once a batch). So nothing the UI does waits on the console or the card, and the log goes on when the main loop
// doesn't (it says so: the stall watch, below)
static SemaphoreHandle_t logLock;  // writeLine's: the log task's, and earlyLines' (the main loop's, once)
static TaskHandle_t logTask;
static std::atomic<uint32_t> logDropped{0};
static volatile bool serialUp;  // Serial (HWCDC) begun (board::serialReady, from setup)

static void consoleOut(const char* s, size_t n) {
  if (serialUp) {
    Serial.write((const uint8_t*)s, n);  // (drops what doesn't fit: never waits)
  } else {
    esp_rom_printf("%s", s);  // (before Serial.begin: the ROM's, which waits 5 ms at most for a computer not reading)
  }
}

static void writeLine(const char* line, uint32_t ms, std::string* sd = 0) {
  if (logLock) {
    xSemaphoreTakeRecursive(logLock, portMAX_DELAY);
  }
  char out[200];
  int n = snprintf(out, sizeof out, "[%6u ms] %s\n", (unsigned)ms, line);
  consoleOut(out, n < (int)sizeof out ? n : sizeof out - 1);
  if (!usbState) {
    if (!early) {
      early = new std::vector<std::pair<uint32_t, std::string>>();
    }
    early->emplace_back(ms, line);
  } else if (logToSd) {
    char l[200];
    snprintf(l, sizeof l, "%7.1f %s\n", ms / 1000.0, line);
    if (sd) {
      *sd += l;  // (the log task's batch)
    } else if (FILE* f = fopen(logFile, "a")) {
      fputs(l, f);
      fclose(f);
    }
  }
  if (logLock) {
    xSemaphoreGiveRecursive(logLock);
  }
}

static void logLoop(void*) {
  char line[160];
  std::string batch;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
    batch.clear();
    xSemaphoreTakeRecursive(logLock, portMAX_DELAY);
    for (int k = 0; k < LOG_Q; k++) {
      portENTER_CRITICAL(&logMux);
      bool any = logR != logW;
      if (any) {
        memcpy(line, logQ[logR % LOG_Q], sizeof line);
        logR++;
      }
      portEXIT_CRITICAL(&logMux);
      if (!any) {
        break;
      }
      uint32_t ms;
      memcpy(&ms, line + sizeof line - 4, 4);
      line[sizeof line - 5] = 0;
      writeLine(line, ms, &batch);
    }
    if (uint32_t lost = logDropped.exchange(0)) {
      char l[48];
      snprintf(l, sizeof l, "log: %u lines lost (too many at once)", (unsigned)lost);
      writeLine(l, ::millis(), &batch);
    }
    if (!batch.empty() && logToSd) {
      if (FILE* f = fopen(logFile, "a")) {
        fwrite(batch.data(), 1, batch.size(), f);
        fclose(f);
      }
    }
    xSemaphoreGiveRecursive(logLock);
  }
}
#else
static void writeLine(const char* line, uint32_t ms) {
  if (!usbState) {
    if (!early) {
      early = new std::vector<std::pair<uint32_t, std::string>>();
    }
    early->emplace_back(ms, line);
    return;
  }
  if (!logToSd) {
    Serial.println(line);
  } else if (FILE* f = fopen(logFile, "a")) {
    fprintf(f, "%7.1f %s\n", ms / 1000.0, line);
    fclose(f);
  }
}
#endif

static void flushLog() {
#ifdef BOARD_TAB5
  if (logTask) {
    xTaskNotifyGive(logTask);
  }
  return;
#endif
  char line[160];
  for (;;) {
    portENTER_CRITICAL(&logMux);
    bool any = logR != logW;
    if (any) {
      memcpy(line, logQ[logR % LOG_Q], sizeof line);
      logR++;
    }
    portEXIT_CRITICAL(&logMux);
    if (!any) {
      return;
    }
    uint32_t ms;
    memcpy(&ms, line + sizeof line - 4, 4);  // when it was said, in its last 4 bytes
    writeLine(line, ms);
  }
}

void board::logNow() {
  flushLog();
#ifdef BOARD_TAB5
  for (uint32_t t = ::millis(); logR != logW && ::millis() - t < 500;) {
    delay(5);
  }
#endif
}

static volatile bool psramOn;  // the USB host doesn't start meanwhile: its buffers must be in the internal RAM

void hal::psramFirst(bool on) {
  psramOn = on;
  heap_caps_malloc_extmem_enable(on ? 0 : CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);  // (as the Arduino core has it)
}

void hal::idle() {
  delay(1);  // (a tick: the idle task's, which the watchdog watches)
}

void hal::logRam(const char* what) {
  hal::log("ram: %s: %u KB internal free (biggest %u KB), %u KB PSRAM", what,
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

void hal::log(const char* fmt, ...) {
  char buf[160];
  va_list a;
  va_start(a, fmt);
  vsnprintf(buf, sizeof buf - 4, fmt, a);
  va_end(a);
  uint32_t ms = ::millis();
#ifdef BOARD_TAB5
  if (logTask) {
    memcpy(buf + sizeof buf - 4, &ms, 4);
    bool queued = false;
    portENTER_CRITICAL(&logMux);
    if (logW - logR < LOG_Q) {
      memcpy(logQ[logW % LOG_Q], buf, sizeof buf);
      logW++;
      queued = true;
    }
    portEXIT_CRITICAL(&logMux);
    if (!queued) {
      logDropped++;
    }
    xTaskNotifyGive(logTask);
    return;
  }
#endif
  if (!mainTask || xTaskGetCurrentTaskHandle() == mainTask) {
    if (logQ) {
      flushLog();
    }
    writeLine(buf, ms);
    return;
  }
  if (!logQ) {
    return;
  }
  memcpy(buf + sizeof buf - 4, &ms, 4);
  portENTER_CRITICAL(&logMux);
  if (logW - logR < LOG_Q) {
    memcpy(logQ[logW % LOG_Q], buf, sizeof buf);
    logW++;
  }
  portEXIT_CRITICAL(&logMux);
}

#ifdef BOARD_TAB5
// ------------------------------------------------------------------------------------------ stall watch ---
// The main loop (LVGL, the screen, the pages) says where it is (board::beat) each time round. If it doesn't for 8 s,
// the log says where it was, what its task is doing (its state; where it was switched out, if it was), and where
// core 1 (its core: the USB host task's too) is running, sampled with the P4's PC recorder (ASSIST_DEBUG); then
// every 5 s again, and at 30 s the board restarts, the same kept for the next start's log ("crash: main loop stuck")
// as a crash is. Decode the addresses against this build's firmware.elf:
//   riscv32-esp-elf-addr2line -pfiaC -e firmware.elf ADDR...
#define STALL_LOG_MS 8000
#define STALL_RESTART_MS 30000
static std::atomic<uint32_t> loopBeat{0};
static const char* volatile loopAt = "start";
static std::atomic<uint32_t> idle1{0};

void board::beat(const char* at) {
  loopAt = at;
  loopBeat++;
}

void board::serialReady() {
  serialUp = true;
}

static bool idleHook1() {
  idle1++;
  return true;
}

// where core 1 is now (and its stack pointer): the recorder stopped for a moment, read, started again
static uint32_t core1Pc(uint32_t* sp) {
  uint32_t on = ASSIST_DEBUG_CORE_1_RCD_PDEBUGEN | ASSIST_DEBUG_CORE_1_RCD_RECORDEN;
  REG_WRITE(ASSIST_DEBUG_CORE_1_RCD_EN_REG, on);
  esp_rom_delay_us(5);
  REG_WRITE(ASSIST_DEBUG_CORE_1_RCD_EN_REG, ASSIST_DEBUG_CORE_1_RCD_PDEBUGEN);  // (holds the last one)
  uint32_t pc = REG_READ(ASSIST_DEBUG_CORE_1_RCD_PDEBUGPC_REG);
  if (sp) {
    *sp = REG_READ(ASSIST_DEBUG_CORE_1_RCD_PDEBUGSP_REG);
  }
  REG_WRITE(ASSIST_DEBUG_CORE_1_RCD_EN_REG, on);
  return pc;
}

static const char* taskState(eTaskState s) {
  switch (s) {
    case eRunning: return "running";
    case eReady: return "ready (not given the CPU)";
    case eBlocked: return "blocked (waiting)";
    case eSuspended: return "suspended";
    case eDeleted: return "deleted";
    default: return "?";
  }
}

static void stallWatch(void*) {
  uint32_t beat = loopBeat, since = ::millis(), told = 0;
  uint32_t idle = idle1, usbLoops = board::usbHostLoops(), usbEv = board::usbEvents();
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    uint32_t now = ::millis();
    uint32_t dIdle = idle1 - idle, dLoops = board::usbHostLoops() - usbLoops, dEv = board::usbEvents() - usbEv;
    idle += dIdle, usbLoops += dLoops, usbEv += dEv;
    if (loopBeat != beat) {
      if (told) {
        hal::log("stall: the main loop runs again (it was stuck %u ms at \"%s\")", (unsigned)(now - since),
                 (const char*)loopAt);
      }
      beat = loopBeat;
      since = now;
      told = 0;
      continue;
    }
    uint32_t stuck = now - since;
    if (stuck < STALL_LOG_MS) {
      continue;
    }
    eTaskState st = mainTask ? eTaskGetState(mainTask) : eInvalid;
    uint32_t spc = 0, sra = 0;
    if (mainTask && st != eRunning && st != eDeleted) {  // (its context, saved where it was switched out)
      const uint32_t* top = *(const uint32_t* const*)mainTask;
      spc = top[0];  // (RvExcFrame: mepc, then ra)
      sra = top[1];
    }
    uint32_t pcs[8], sp = 0;
    for (int i = 0; i < 8; i++) {
      pcs[i] = core1Pc(i ? 0 : &sp);
      esp_rom_delay_us(997);
    }
    if (!told || now - told >= 5000) {
      told = now;
      hal::log("stall: main loop stuck %u ms at \"%s\", its task %s, switched out at pc %08X ra %08X",
               (unsigned)stuck, (const char*)loopAt, taskState(st), (unsigned)spc, (unsigned)sra);
      hal::log("stall: core 1 at %08X %08X %08X %08X %08X %08X %08X %08X (sp %08X)", (unsigned)pcs[0], (unsigned)pcs[1],
               (unsigned)pcs[2], (unsigned)pcs[3], (unsigned)pcs[4], (unsigned)pcs[5], (unsigned)pcs[6],
               (unsigned)pcs[7], (unsigned)sp);
      hal::log("stall: in the last second, core 1 idle %u times, the USB host task round %u times, %u USB events",
               (unsigned)dIdle, (unsigned)dLoops, (unsigned)dEv);
    }
    if (stuck >= STALL_RESTART_MS) {
      snprintf(crash.reason, sizeof crash.reason, "main loop stuck at %s", (const char*)loopAt);
      crash.core = 1;
      crash.pc = spc ? spc : pcs[0];
      crash.n = 0;
      crash.bt[crash.n++] = sra;
      for (uint32_t p : pcs) {
        crash.bt[crash.n++] = p;
      }
      crash.magic = CRASH_MAGIC;
      hal::log("stall: restarting (the next start logs this as a crash: pc = where it was switched out, then ra, then "
               "core 1's samples)");
      board::logNow();
      delay(200);
      esp_restart();
    }
  }
}

static void stallWatchBegin() {
  esp_register_freertos_idle_hook_for_cpu(idleHook1, 1);
  xTaskCreatePinnedToCore(stallWatch, "stall", 4096, 0, 6, 0, 0);
}
#else
void board::beat(const char*) {}
void board::serialReady() {}
#endif

static void usbKnown();

// the lines said before the log had a place
static void earlyLines() {
#ifdef BOARD_TAB5
  if (logLock) {
    xSemaphoreTakeRecursive(logLock, portMAX_DELAY);
  }
#endif
  if (early) {
#ifdef BOARD_TAB5  // (on the console already, as they were said: to the SD card's log now, in one go)
    if (logToSd) {
      if (FILE* f = fopen(logFile, "a")) {
        for (auto& l : *early) {
          fprintf(f, "%7.1f %s\n", l.first / 1000.0, l.second.c_str());
        }
        fclose(f);
      }
    }
#else
    for (auto& l : *early) {
      writeLine(l.second.c_str(), l.first);
    }
#endif
    delete early;
    early = 0;
  }
#ifdef BOARD_TAB5
  if (logLock) {
    xSemaphoreGiveRecursive(logLock);
  }
#endif
}

void board::begin() {
  mainTask = xTaskGetCurrentTaskHandle();
  set_arduino_panic_handler(onPanic, 0);
  logQ = (char (*)[160])heap_caps_malloc(LOG_Q * 160, MALLOC_CAP_SPIRAM);
#ifdef BOARD_TAB5
  logLock = xSemaphoreCreateRecursiveMutex();
  if (logQ && logLock) {
    xTaskCreatePinnedToCore(logLoop, "log", 6144, 0, 2, &logTask, 0);
  }
  stallWatchBegin();
#endif
  // (not heap_caps_malloc_extmem_enable(): small blocks must stay internal, the USB host's DMA can't reach PSRAM. The
  // big ones go there anyway: LVGL's pool, MIDI's buffers, TLS's)
  // and TLS's buffers (about 40 KB a connection): with Bluetooth on, the internal RAM can't spare them
  mbedtls_platform_set_calloc_free(
      [](size_t n, size_t size) {
        void* p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM);
        return p ? p : heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL);
      },
      free);
#ifdef LED_RGB
  rgbLedWrite(LED_RGB, 0, 0, 0);
#endif
  STAGE("sd: start");
#ifdef SD_CLK  // (a board without an SD card: no SD_ pins)
  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0, SD_D1, SD_D2, SD_D3);
  sdOk = SD_MMC.begin("/sdcard", false);
  if (!sdOk) {  // (some boards only wire D0: 1-bit)
    SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
    sdOk = SD_MMC.begin("/sdcard", true);
  }
#ifdef BOARD_TAB5
  if (!sdOk) {  // (the Tab5's slot in SPI mode, as M5Stack's own example has it: CS D3, SCK CLK, MOSI CMD, MISO D0)
    SD_MMC.end();
    SPI.begin(SD_CLK, SD_D0, SD_CMD, SD_D3);
    sdOk = sdSpi = SD.begin(SD_D3, SPI, 25000000, "/sdcard");
  }
#endif
#endif
  STAGE(sdOk ? "sd: done, mounted" : "sd: done, no card");
#ifndef BOARD_TAB5  // (the Tab5's Wi-Fi is the ESP32-C6's: nothing asks it anything until a job needs Wi-Fi)
  WiFi.mode(WIFI_OFF);
#endif
  disk = diskFlag == DISK_MODE && sdOk;
  diskFlag = 0;
  if (disk) {  // the card is the computer's: nothing here touches it, no log on it
    usbState = 1;
    earlyLines();
    hal::log("fm1buddy: USB disk mode (%u MB)", (unsigned)((uint64_t)SD_MMC.numSectors() * SD_MMC.sectorSize() >> 20));
    return;
  }
  // a computer on the USB-C port (it sends start-of-frames): stay its serial port. Else: the USB host
  // (board::usbBegin), and the log goes to the SD card. Up to USB_WAIT to know: board::loop finds out, meanwhile the
  // screen starts. Only for recovery mode it's waited for here (the app starts on the recover page, or doesn't)
  hostTestNow = hostTest == HOST_TEST;
  hostTest = 0;
  if (recoveryFlag == RECOVERY_MODE) {
#ifndef BOARD_TAB5  // (the Tab5's USB-C is its serial port alone: the host is the USB-A port, always)
    for (; !hostTestNow && millis() < USB_WAIT && !computer; delay(10)) {
      computer = HWCDC::isPlugged();
    }
#endif
    usbKnown();
  }
}

// a computer on the port or not, known now: where the log goes, recovery mode
static void usbKnown() {
  bool test = hostTestNow;
  usbState = 1;
  recovery = recoveryFlag == RECOVERY_MODE && !computer;  // (not with a computer on the port)
  recoveryFlag = 0;
  if (recovery) {  // the FM-1 is on the port (not a computer): its USB lines are the key's (key_s3.cpp)
    mkdir("/sdcard/fm1buddy", 0777);
    logFile = LOG_RECOVERY;
    if (FILE* f = sdOk ? fopen(logFile, "a") : 0) {
      fputs("--- recovery\n", f);
      fclose(f);
      logToSd = true;
    }
    earlyLines();
    hal::log("fm1buddy: recovery mode, last reset: %d (%s)", (int)esp_reset_reason(), resetName(esp_reset_reason()));
    logCrash();
    return;
  }
  if ((!computer || hal::computerMidi()) && sdOk) {  // (USB MIDI for a computer: no serial port either)
    mkdir("/sdcard/fm1buddy", 0777);
    if (newLog()) {
      logToSd = true;
    }
  }
  if (test) {
    hostTestUntil = millis() + 40000;
    hostScript[sizeof hostScript - 1] = 0;
  }
  earlyLines();
  hal::log("fm1buddy: sd %s, usb %s%s, last reset: %d", sdOk ? "mounted" : "none",
           computer ? (hal::computerMidi() ? "USB MIDI (a computer)" : "serial (a computer)") : "host", test ? " (a test: 40 s)" : "", (int)esp_reset_reason());
  hal::log("fm1buddy: last reset: %s", resetName(esp_reset_reason()));
  logCrash();
}

// the USB port back to the serial port: a restart (esp_restart) keeps the host's choice, and a computer on it
// then wouldn't find the board until it's reset by its button
static void phyToSerial() {
#ifndef BOARD_TAB5
  CLEAR_PERI_REG_MASK(RTC_CNTL_USB_CONF_REG, RTC_CNTL_SW_HW_USB_PHY_SEL_M | RTC_CNTL_SW_USB_PHY_SEL_M);
#endif
}

void board::usbBegin() {
  usbState = 2;
#ifdef BOARD_TAB5
  // two ports: the USB-A one is the host (the P4's USB-OTG HS), the USB-C one stays the serial port (USB-Serial-JTAG)
  if (!disk && !recovery) {
    STAGE("usb host: start");
    usbHostBegin();
    STAGE("usb host: task made");
  }
  return;
#endif
  if (disk) {
    esp_register_shutdown_handler(phyToSerial);
    Serial.end();
    diskBegin();
    return;
  }
  if (computer && hal::computerMidi()) {
    esp_register_shutdown_handler(phyToSerial);
    hal::log("usb: serial off, USB MIDI for the computer");
    Serial.end();
    computerBegin();
    return;
  }
  if (computer) {
    return;
  }
  esp_register_shutdown_handler(phyToSerial);
  hal::log("usb: serial off");
  Serial.end();
  if (!recovery) {
    usbHostBegin();  // (in recovery mode: once the key has worked, keyStart)
  }
}

void board::loop() {
  if (usbState < 2) {  // a computer on the port or not (board::begin), then the port as that wants it
    if (!usbState) {
#ifndef BOARD_TAB5  // (the Tab5: never on the host's port)
      computer = computer || (!hostTestNow && HWCDC::isPlugged());  // (the host test: the host, whatever is on the port)
#endif
      if (computer || hostTestNow || millis() >= USB_WAIT) {
        usbKnown();
      }
    }
    if (usbState == 1 && !psramOn) {  // (not while small blocks go to the PSRAM: hal::psramFirst)
      board::usbBegin();
    }
  }
  flushLog();
  if (hostTestUntil && (int32_t)(millis() - hostTestUntil) >= 0 && !jobs::running) {  // (not in a job's middle)
    hal::log("usb: the test is over: restarting");
    esp_restart();
  }
}

void board::hostTestStart(const char* script) {
  strlcpy(hostScript, script ? script : "", sizeof hostScript);
  hostTest = HOST_TEST;
  esp_restart();
}

const char* board::hostTestScript() {
  return hostTestUntil ? hostScript : "";
}

void board::printLog() {
  for (const char* p : {LOG_RECOVERY, LOG_PREV, LOG_FILE}) {
    FILE* f = fopen(p, "r");
    if (!f) {
      continue;
    }
    Serial.printf("--- %s\n", p + 8);
    char line[200];
    while (fgets(line, sizeof line, f)) {
      Serial.print(line);
    }
    fclose(f);
  }
}

void board::printFile(const char* path) {
  std::string p = std::string("/sdcard/") + path;
  FILE* f = hal::sd() ? fopen(p.c_str(), "r") : 0;
  if (!f) {
    Serial.printf("no %s\n", path);
    return;
  }
  char line[200];
  while (fgets(line, sizeof line, f)) {
    Serial.print(line);
  }
  fclose(f);
}

void board::printDir(const char* path) {
  std::string p = std::string("/sdcard/") + path;
  DIR* d = hal::sd() ? opendir(p.c_str()) : 0;
  if (!d) {
    Serial.printf("no %s\n", path);
    return;
  }
  while (dirent* e = readdir(d)) {
    Serial.printf("%s%s\n", e->d_name, e->d_type == DT_DIR ? "/" : "");
  }
  closedir(d);
}

uint32_t hal::millis() {
  return ::millis();
}

const char* hal::sd() {
  return sdOk && !disk ? "/sdcard" : 0;
}

bool hal::diskMode() {
  return disk;
}

bool hal::recoveryMode() {
  return recovery;
}

void hal::recoveryModeSet(bool on) {
#ifdef BOARD_TAB5
  if (on) {
    hal::log("usb: no recovery mode on the Tab5");
    return;
  }
#endif
  recoveryFlag = on ? RECOVERY_MODE : 0;
  hal::log("usb: %s recovery mode: restarting", on ? "into" : "out of");
  flushLog();
  esp_restart();
}

bool hal::computerMidi() {
#ifdef BOARD_TAB5
  return false;
#endif
  bool on = false;
  return hal::load("computer", &on, 1) && on;
}

void hal::computerMidiSet(bool on) {
#ifdef BOARD_TAB5
  hal::log("usb: no USB MIDI for a computer on the Tab5");
  return;
#endif
  hal::save("computer", &on, 1);
  if (computer) {
    hal::log("usb: %s for the computer: restarting", on ? "USB MIDI" : "the serial port");
    flushLog();
    esp_restart();
  }
}

bool hal::onComputer() {
  return computer;
}

void hal::diskModeSet(bool on) {
#ifdef BOARD_TAB5
  if (on) {
    hal::log("usb: no USB disk mode on the Tab5");
    return;
  }
#endif
  diskFlag = on ? DISK_MODE : 0;
  hal::log("usb: %s USB disk mode: restarting", on ? "into" : "out of");
  flushLog();
  esp_restart();
}

bool hal::sdSpace(uint64_t& total, uint64_t& used) {
  if (!sdOk) {
    return false;
  }
#ifdef BOARD_TAB5
  if (sdSpi) {
    total = SD.totalBytes();
    used = SD.usedBytes();
    return true;
  }
#endif
  total = SD_MMC.totalBytes();
  used = SD_MMC.usedBytes();
  return true;
}

bool hal::load(const char* key, void* p, size_t n) {
  Preferences prefs;
  prefs.begin("fm1buddy", false);
  // (isKey first: a key never saved is the usual case, e.g. "sleep", read every second, and getBytesLength on it
  // logs an NVS NOT_FOUND error each time)
  bool ok = prefs.isKey(key) && prefs.getBytesLength(key) == n && prefs.getBytes(key, p, n) == n;
  prefs.end();
  return ok;
}

void hal::save(const char* key, const void* p, size_t n) {
  Preferences prefs;
  prefs.begin("fm1buddy", false);
  prefs.putBytes(key, p, n);
  prefs.end();
}

#ifndef BOARD_TAB5  // (the Tab5's: tab5.cpp)
uint32_t hal::batteryMv() {
  return analogReadMilliVolts(BAT_ADC) * 2;  // (the battery, halved by the board's divider)
}

void hal::backlight(uint8_t percent) {
  static bool attached;
  if (!attached) {
    ledcAttach(TFT_BL, 5000, 8);
    attached = true;
  }
  ledcWrite(TFT_BL, percent * 255 / 100);
}
#endif

// --------------------------------------------------------------------------------------------- Wi-Fi ---
bool hal::wifiScan(std::vector<Ap>& out) {
  board::blePause(true);  // both don't fit
  radio = true;
  WiFi.mode(WIFI_STA);
  int n = WiFi.scanNetworks();
  out.clear();
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (!s.length()) {
      continue;  // hidden
    }
    auto a = std::find_if(out.begin(), out.end(), [&](const Ap& x) { return x.ssid == s.c_str(); });
    if (a == out.end()) {
      out.push_back({s.c_str(), (int)WiFi.RSSI(i), WiFi.encryptionType(i) == WIFI_AUTH_OPEN});
    } else if (WiFi.RSSI(i) > a->rssi) {
      a->rssi = WiFi.RSSI(i);
    }
  }
  WiFi.scanDelete();
  std::sort(out.begin(), out.end(), [](const Ap& a, const Ap& b) { return a.rssi > b.rssi; });
  return n >= 0;
}

bool hal::wifiJoin(const char* ssid, const char* pass, std::string& err) {
  hal::log("wifi: %u KB internal RAM free", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
  board::blePause(true);
  radio = true;
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  for (uint32_t t = ::millis(); ::millis() - t < 15000; delay(50)) {
    wl_status_t s = WiFi.status();
    if (s == WL_CONNECTED) {
      wifiUp = true;
      return true;
    }
    if (s == WL_CONNECT_FAILED || s == WL_NO_SSID_AVAIL) {
      break;
    }
  }
  wl_status_t s = WiFi.status();
  err = std::string(s == WL_NO_SSID_AVAIL ? "Can't find " : "Can't join ") + ssid +
        (s == WL_NO_SSID_AVAIL ? "." : ": check the password.");
  wifiOff();
  return false;
}

void hal::wifiOff() {
  wifiUp = false;
  if (!radio) {
    return;
  }
  radio = false;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  board::blePause(false);
}

bool hal::wifiOn() {
  return wifiUp;
}

// ---------------------------------------------------------------------------------------------- HTTPS ---
static esp_err_t onEvent(esp_http_client_event_t* e) {
  if (e->event_id == HTTP_EVENT_ON_HEADER && !strcasecmp(e->header_key, "ETag")) {
    *(std::string*)e->user_data = e->header_value;
  }
  return ESP_OK;
}

int hal::get(const char* url, const char* path, std::string* etag, void (*progress)(uint32_t, uint32_t),
             std::string& err) {
  if (proxyUsed()) {  // Settings > Proxy: ESP-IDF's client has none (proxy_get.cpp)
    hal::log("get: %s via %s proxy %s:%u", url, proxy.kind == PROXY_SOCKS5 ? "SOCKS5" : "HTTP", proxy.host,
             (unsigned)proxy.port);
    int code = proxyGet(proxy, url, path, etag, progress, err);
    if (code != 200 && code != 304) {
      hal::log("get: %d %s", code, err.c_str());
    }
    return code;
  }
  std::string tag;
  esp_http_client_config_t c = {};
  c.url = url;
  c.event_handler = onEvent;
  c.user_data = &tag;
  c.crt_bundle_attach = esp_crt_bundle_attach;
  c.timeout_ms = 15000;
  c.buffer_size = 4096;
  // GitHub's release downloads redirect to a signed URL of ~4 KB, which goes in the request line: the default
  // write buffer (512) can't hold it, so following the redirect fails ("Out of buffer")
  c.buffer_size_tx = 8192;
  c.user_agent = "fm1buddy";
  esp_http_client_handle_t h = esp_http_client_init(&c);
  if (etag && !etag->empty()) {
    esp_http_client_set_header(h, "If-None-Match", etag->c_str());
  }
  int status = -1;
  for (int hops = 0; hops < 5; hops++) {
    esp_err_t r = esp_http_client_open(h, 0);
    if (r != ESP_OK) {
      err = std::string("Can't reach it (") + esp_err_to_name(r) + ").";
      break;
    }
    int64_t len = esp_http_client_fetch_headers(h);
    status = esp_http_client_get_status_code(h);
    if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
      if (esp_http_client_set_redirection(h) != ESP_OK) {
        err = "The server's redirect didn't fit: too much of it to read.";
        status = -1;
        break;
      }
      esp_http_client_close(h);
      status = -1;
      continue;
    }
    if (status != 200) {
      break;
    }
    std::string part = std::string(path) + ".part";
    FILE* f = fopen(part.c_str(), "wb");
    if (!f) {
      err = "Can't write to the SD card.";
      status = -1;
      break;
    }
    static char* buf = (char*)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    uint32_t got = 0;
    bool ok = true;
    for (;;) {
      int n = esp_http_client_read(h, buf, 4096);
      if (n < 0) {
        err = "The download stopped part way.";
        ok = false;
        break;
      }
      if (n == 0) {
        if (!esp_http_client_is_complete_data_received(h)) {
          err = "The download stopped part way.";
          ok = false;
        }
        break;
      }
      if (fwrite(buf, 1, n, f) != (size_t)n) {
        err = "Can't write to the SD card (is it full?).";
        ok = false;
        break;
      }
      got += n;
      if (progress) {
        progress(got, len > 0 ? (uint32_t)len : 0);
      }
    }
    fclose(f);
    remove(path);
    if (!ok || rename(part.c_str(), path)) {
      if (ok) {
        err = "Can't rename it on the SD card.";
      }
      remove(part.c_str());
      status = -1;
    }
    break;
  }
  esp_http_client_close(h);
  esp_http_client_cleanup(h);
  if (status == 200 && etag) {
    *etag = tag;
  }
  return status;
}

static void toHex(const uint8_t sum[32], char hex[65]) {
  for (int i = 0; i < 32; i++) {
    sprintf(hex + i * 2, "%02x", sum[i]);
  }
}

bool hal::sha256(const uint8_t* p, size_t n, char hex[65]) {
  uint8_t sum[32];
  if (mbedtls_sha256(p, n, sum, 0)) {
    return false;
  }
  toHex(sum, hex);
  return true;
}

// through FatFs: its directory entries have the size and time, where stat would search the folder for each file (the
// card is its only volume: drive 0)
bool hal::listDir(const char* path, void (*fn)(const char* name, uint32_t size, uint32_t mtime, bool folder, void* arg), void* arg) {
  if (strncmp(path, "/sdcard/", 8)) {
    return false;
  }
  std::string p = std::string("0:") + (path + 7);
  FF_DIR d;
  FRESULT r = f_opendir(&d, p.c_str());
  if (r != FR_OK) {
    hal::log("sd: can't open %s (FatFs %d)", p.c_str(), (int)r);
    return false;
  }
  FILINFO fi;
  while ((r = f_readdir(&d, &fi)) == FR_OK && fi.fname[0]) {
    if (fi.fattrib & AM_DIR) {
      fn(fi.fname, 0, 0, true, arg);
      continue;
    }
    struct tm tm = {};  // (as ESP-IDF's stat makes st_mtime)
    tm.tm_mday = fi.fdate & 0x1F;
    tm.tm_mon = ((fi.fdate >> 5) & 0xF) - 1;
    tm.tm_year = (fi.fdate >> 9) + 80;
    tm.tm_sec = (fi.ftime & 0x1F) * 2;
    tm.tm_min = (fi.ftime >> 5) & 0x3F;
    tm.tm_hour = fi.ftime >> 11;
    tm.tm_isdst = -1;
    fn(fi.fname, fi.fsize, (uint32_t)mktime(&tm), false, arg);
  }
  f_closedir(&d);
  if (r != FR_OK) {
    hal::log("sd: reading %s stopped (FatFs %d)", p.c_str(), (int)r);
    return false;
  }
  return true;
}

// where the app starts in an installer .bin (the partition table's first app slot)
#define IMAGE_APP_AT 0x10000

const char* hal::selfUpdate(const char* path, void (*progress)(uint32_t, uint32_t)) {
  FILE* f = fopen(path, "rb");
  if (!f) {
    return "The download isn't on the SD card.";
  }
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  uint8_t magic = 0;
  if (size <= IMAGE_APP_AT || fseek(f, IMAGE_APP_AT, SEEK_SET) || fread(&magic, 1, 1, f) != 1 || magic != 0xE9) {
    fclose(f);
    return "That isn't an fm1buddy firmware.";
  }
  uint32_t n = size - IMAGE_APP_AT;
  const esp_partition_t* to = esp_ota_get_next_update_partition(0);
  if (!to || n > to->size) {
    fclose(f);
    return to ? "It's too big for this board's app slot." : "This board has no second app slot.";
  }
  hal::log("update: %u bytes into %s (at %06X)", (unsigned)n, to->label, (unsigned)to->address);
  esp_ota_handle_t h;
  esp_err_t r = esp_ota_begin(to, OTA_WITH_SEQUENTIAL_WRITES, &h);
  static uint8_t* buf = (uint8_t*)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
  fseek(f, IMAGE_APP_AT, SEEK_SET);
  for (uint32_t at = 0; r == ESP_OK && at < n;) {
    size_t k = fread(buf, 1, std::min<uint32_t>(4096, n - at), f);
    if (!k) {
      r = ESP_FAIL;
      break;
    }
    r = esp_ota_write(h, buf, k);
    at += k;
    if (progress) {
      progress(at, n);
    }
  }
  fclose(f);
  if (r != ESP_OK) {
    hal::log("update: writing: %s", esp_err_to_name(r));
    esp_ota_abort(h);
    return "Writing it failed: the firmware it runs is unchanged.";
  }
  if ((r = esp_ota_end(h)) != ESP_OK || (r = esp_ota_set_boot_partition(to)) != ESP_OK) {
    hal::log("update: %s", esp_err_to_name(r));
    return "It didn't check out: the firmware it runs is unchanged.";
  }
  hal::log("update: done, %s starts next", to->label);
  return 0;
}

void hal::reboot() {
  hal::log("restarting");
  esp_restart();
}

bool hal::sha256(const char* path, char hex[65]) {
  FILE* f = fopen(path, "rb");
  if (!f) {
    return false;
  }
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  static uint8_t* buf = (uint8_t*)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
  for (size_t n; (n = fread(buf, 1, 4096, f));) {
    mbedtls_sha256_update(&ctx, buf, n);
  }
  fclose(f);
  uint8_t sum[32];
  mbedtls_sha256_finish(&ctx, sum);
  mbedtls_sha256_free(&ctx);
  toHex(sum, hex);
  return true;
}

// ----------------------------------------------------------------------------------------------- jobs ---
struct Spawn {
  void (*fn)(void*);
  void* arg;
};

static void trampoline(void* p) {
  Spawn s = *(Spawn*)p;
  delete (Spawn*)p;
  s.fn(s.arg);
  vTaskDelete(0);
}

void hal::spawn(void (*fn)(void*), void* arg, uint32_t stack, int priority) {
  xTaskCreatePinnedToCore(trampoline, "job", stack, new Spawn{fn, arg}, priority, 0, 0);
}
