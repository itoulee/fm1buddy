#include "mirror.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <atomic>
#include <mutex>

#include "fm1.h"
#include "hal.h"
#include "midi.h"

static std::atomic<bool> showing{false};
static std::atomic<bool> seen{false};  // a row came since the page opened
static uint16_t* screen;  // (PSRAM on the board: a big block)
static std::mutex lock;   // the dirty rows
static uint8_t dirtyRows[MIRROR_SIZE / 8];
static bool anyDirty;
static uint32_t helloAt;
static bool fresh;  // the next hello asks for all of it

void mirror::on(bool on) {
  if (on && !screen) {
    screen = (uint16_t*)calloc(MIRROR_SIZE * MIRROR_SIZE, 2);
  }
  if (on && !showing) {
    fresh = true;
    seen = false;
    helloAt = 0;
  }
  showing = on && screen;
}

bool mirror::active() {
  return showing;
}

const uint16_t* mirror::pixels() {
  return screen;
}

bool mirror::live() {
  return seen;
}

// who may be an FM-1: those fm1.h asks, and the Big Screen firmware on M-VAVE's Open V14 ("FM1 Community")
static bool fm1Like(int d) {
  midi::Dev v;
  if (!midi::dev(d, v)) {
    return false;
  }
  if (fm1::candidate(d)) {
    return true;
  }
  char lower[sizeof v.name];
  uint32_t i = 0;
  for (; v.name[i]; i++) {
    lower[i] = tolower(v.name[i]);
  }
  lower[i] = 0;
  return strstr(lower, "fm1");
}

void mirror::service() {
  if (!showing || fm1::busy) {
    return;
  }
  uint32_t now = hal::millis();
  if (helloAt && now - helloAt < 1000) {
    return;
  }
  helloAt = now;
  // 01 (all of it) when we start, and again until a row comes (it may have missed one). Then 00: it sends only what
  // changes, so a still screen sends nothing at all
  const uint8_t all = fresh || !seen;
  const uint8_t hello[] = {0xF0, 0x7D, 'F', 'S', all, 0xF7};
  for (int d = 0; d < MIDI_USB; d++) {
    if (fm1Like(d)) {
      midi::send(d, hello, sizeof hello);
    }
  }
  fresh = false;
}

bool mirror::sysex(int, const uint8_t* m, uint32_t n) {
  if (n < 6 || m[0] != 0xF0 || m[1] != 0x7D || m[2] != 'F' || m[3] != 'S' || m[n - 1] != 0xF7) {
    return false;
  }
  if (m[4] != 1 || n < 10) {
    return true;  // (the sound's name, a hello from another viewer, or something newer: not for the queue)
  }
  int y = m[5] | m[6] << 7, x = m[7] | m[8] << 7;
  if (!screen || y >= MIRROR_SIZE) {
    return true;
  }
  uint16_t* row = screen + y * MIRROR_SIZE;
  for (uint32_t i = 9; i + 4 < n; i += 4) {
    uint16_t c = m[i + 1] | m[i + 2] << 7 | m[i + 3] << 14;
    for (int k = 0; k < m[i] && x < MIRROR_SIZE; k++) {
      row[x++] = c;
    }
  }
  seen = true;
  std::lock_guard<std::mutex> g(lock);
  dirtyRows[y / 8] |= 1 << (y % 8);
  anyDirty = true;
  return true;
}

bool mirror::dirty(uint8_t rows[MIRROR_SIZE / 8]) {
  std::lock_guard<std::mutex> g(lock);
  if (!anyDirty) {
    return false;
  }
  memcpy(rows, dirtyRows, sizeof dirtyRows);
  memset(dirtyRows, 0, sizeof dirtyRows);
  anyDirty = false;
  return true;
}
