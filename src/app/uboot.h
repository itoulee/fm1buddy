// The FM-1's boot loader: it's in the chip's ROM (JieLi WL82), so no firmware can break it. A running FM-1 goes there
// when it's sent F0 22 24 35 7D F7 (stock V15, and the firmwares on Felucca's platform: fomajam ...), and it's then a
// USB storage device (4C4A:8057, "WL80UBOOT1.00") answering JieLi's own commands, until it's told to start its
// firmware again. With our loader in its RAM (loader/: m-bootloader's, MIT, in the board's program as mflr_loader.h,
// so there's nothing to download) it reads the whole flash: the firmware, and what the firmwares keep (settings,
// projects, samples, voices). The loader also draws on the FM-1's own screen as it goes: two rows of text, and the
// flash as 16 x 16 sectors, in the theme's colors.
//
// An FM-1 whose firmware doesn't start gets there by the key its chip listens for when it's switched on (hal.h
// recoveryMode), and then has a firmware or a backup written back (recovery: writeSector); backups only read.
//
// The protocol is as FM-1-transporter (kurogedelic, MIT) and jl-uboot-tool (kagaimiq, MIT) speak it: their
// docs/PROTOCOL.md says how.
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

#include "catalog.h"

#define BACKUPS_DIR "/backups"

namespace uboot {
struct Chip {
  uint16_t key;      // its chip key (0x980F)
  uint32_t flashId;  // its flash's JEDEC id (0x856014, 0xB36014 or 0xEB6014: 1 MiB)
  uint32_t size;     // the flash's size
  uint32_t io;       // the most a read moves at once: its loader's USB buffer (512 if it doesn't say)
};
// device d (an FM-1, alone on the hub) into its boot loader, and the loader running in it; 0 or an error
const char* enter(int d, Chip& chip);
// the same with the FM-1 already in its boot loader (recovery: hal.h keyState), or on its way there
const char* attach(Chip& chip);
// the whole flash; 0 or an error
const char* read(const Chip& chip, std::vector<uint8_t>& out, void (*progress)(uint32_t done, uint32_t total));
// a command, to keep the loader there while the SD card is slow (JieLi's restarted the chip after about 3 s without one)
void keepAlive();

// The loader's own screen on the FM-1 (after enter or attach): 240 x 240, two rows of 20 characters (A-Z, 0-9 and
// " -./:<>+?%"), and the flash's 256 sectors under them, each in one of the palette's colors. False if it didn't take it
// the panel: the FM-1's (smk false) or the SMK-37's; colors: 0xRRGGBB, the background then a sector state's (up to 7)
bool screenInit(bool smk, const uint32_t* colors, int n);
bool screenRow(int row, uint32_t fg, uint32_t bg, const char* text);
bool screenMap(const uint8_t states[256]);

// Writing (recovery only): whole 4 KiB sectors, only in [WRITE_FROM, WRITE_TO). Below is the flash's head (its header,
// the SPL, isd_config: what the chip starts from), above is key_mac; no firmware install writes either
#define SECTOR 0x1000u
#define WRITE_FROM 0x4000u
#define WRITE_TO 0xFF000u
#define FM1_KEY 0x980F
// its flash: 1 MiB SPI NOR, one of three makers' parts (the same type and size, written the same way)
#define FM1_FLASH 0x856014u    // the FM-1's up to 2026
#define FM1_FLASH_2 0xB36014u  // in newer FM-1s
#define FM1_FLASH_3 0xEB6014u  // in some SMK-37s
static inline bool fm1Flash(uint32_t id) {
  return id == FM1_FLASH || id == FM1_FLASH_2 || id == FM1_FLASH_3;
}
// The SMK-37 (the FM-1's chip in a keyboard) looks the same here, but its flash has its factory calibration (keys,
// pads, wheels) in the sector at SMK_CAL, "trimdat" at SMK_MARK: unique to each keyboard, and never written
#define SMK_CAL 0xC1000u
#define SMK_MARK 0xC1224u
bool smkFlash(const std::vector<uint8_t>& flash);  // (a whole flash, or a backup of one)
// one sector: erased, written, and read back; 0 or an error
const char* writeSector(uint32_t addr, const uint8_t* data);
// back to its firmware (the loader resets the chip)
void leave();
// it's still in its boot loader (after leave(): only switching it off and on gets it out)
bool stuck();
}  // namespace uboot
