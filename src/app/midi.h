// MIDI between the devices on the hub: whatever any of them sends goes to all the others ("thru"), except while the
// FM-1 is being updated, or as the mapping's rules say (midimap.h). The board's USB host task calls mount, umount, in and service; the rest is safe from any
// thread.
#pragma once

#include <stdint.h>

#include <atomic>
#include <string>
#include <vector>

#define MIDI_USB 4    // devices 0-3: USB, on the hub
#define MIDI_TOUCH 8  // 4-7: Bluetooth
#define MIDI_DEVS 9   // 8: the touchscreen as a controller (page_touch.cpp): it only plays, nothing goes to it
#define SYSEX_MAX 96  // the longest SysEx kept (the FM-1's answers are 19 and 41 bytes on the wire)

namespace midi {
struct Dev {
  bool on;
  uint16_t vid, pid;
  // its name as it says it (base), and where it is on the hub (port: a letter a hub port, "A" for port 1, from the
  // USB-C port out, "CB" for port 2 of a hub on port 3; "" when it isn't on a hub). name is what the screen shows and
  // rules match: base, or with the port after it ("nanoKEY2 B") while another device on the hub has the same base.
  // Rules match "base port" too, so "to \"fm-1 b\"" keeps reaching it alone
  char base[32];
  char port[8];
  char name[40];
  uint32_t serial;     // new on every mount: a new device in the same slot is noticed
  uint32_t lastIn;     // when it last sent something (ms)
  uint8_t last[3];     // the last channel message it sent
  uint32_t count;      // messages it sent
  uint32_t dropped;    // messages to it that didn't fit its queue (it doesn't take them fast enough)
  uint32_t sysexLost;  // SysEx from it that didn't fit (too long, or not read fast enough)
};

extern std::atomic<bool> thru;  // everything to everything (off for a moment: an install, a job in SysEx)
// MIDI routing: the devices play each other (and Bluetooth MIDI runs, if it's on) while it's on. Off: the devices are
// only listened to (installs, Sounds). Wi-Fi and Bluetooth MIDI don't both fit: with Bluetooth on, turning routing on
// turns Wi-Fi off, and Wi-Fi coming on pauses Bluetooth
extern std::atomic<bool> routing;
void route(bool on);
// MIDI mapping (midimap.h): the rules in /midimap.txt on the SD card, for routing (a file that only explains itself is
// written when there's none). How many rules there are, and what's wrong with the file ("" if nothing). From the UI
int loadMap();
int mapRules();
std::string mapProblem();
extern std::atomic<uint32_t> seq;  // goes up when a device comes or goes

// a copy of device d: false if nothing is there
bool dev(int d, Dev& out);

// the USB host task
void mount(int d, uint16_t vid, uint16_t pid, const char* name, const char* port = "");
void umount(int d);
void in(int d, const uint8_t p[4]);  // a USB MIDI packet it sent
// hands what's waiting for devices from..to-1 to them (hal::midiOut): the USB task does the USB ones, the
// Bluetooth loop the others
void service(int from = 0, int to = MIDI_USB);

// MIDI bytes a device sent (Bluetooth)
void bytesIn(int d, const uint8_t* bytes, uint32_t n);

// anywhere
void send(int d, const uint8_t* bytes, uint32_t n);  // MIDI bytes to device d
// the same, all or nothing: waits up to ms for room in its queue. False if it went, or there wasn't room
bool sendWait(int d, const uint8_t* bytes, uint32_t n, uint32_t ms);
uint32_t sysex(int d, uint8_t* out);  // the next complete SysEx (F0..F7) it sent, or 0
void drain(int d);                    // forget the SysEx it sent
uint32_t waiting(int d);              // packets still in its queue, not yet handed to hal::midiOut
// every SysEx device d sends, whole (any length, up to 1 MB), kept from capture(d, true) until capture(d, false)
void capture(int d, bool on);
// what was kept so far (complete messages, F0..F7 each)
void captured(int d, std::vector<uint8_t>& out);

// "note 60 ch 1", "cc 10 = 64 ch 2", ...
void describe(const uint8_t m[3], char* out, int n);

// for play.h: fn runs with the lock held, and gets play's out (play.h Out)
void locked(void (*fn)(void* arg, void (*out)(int d, const uint8_t* msg, uint32_t n)), void* arg);
}  // namespace midi
