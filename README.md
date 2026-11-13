# fm1buddy for the M5Stack Tab5 (ESP32-P4): this fork's `tab5` branch

> An unofficial port of [konsumer/fm1buddy](https://github.com/konsumer/fm1buddy) (all credit for fm1buddy itself
> goes there) to the [M5Stack Tab5](https://docs.m5stack.com/en/core/Tab5). Based on upstream `main` at `ef5512f`
> (2026-11-11), with all of upstream's code. Upstream's own README follows below the line.

## v1.0 (2026-11-13)

The first release of the Tab5 port. The version shows as `v1.0` in Settings > About and in the boot log.

- The Tab5 port: the app at 640x360 (shown 2x2), USB host on USB-A, the console on USB-C, Wi-Fi/Bluetooth through the ESP32-C6.
- Firmware list: search by name (name, author, file, product; any case), with an on-screen keyboard and a clear button.
- FM1 Open / FM1 Community recognised as an FM-1, with Open's updater armed before Install.
- SMK-37 Pro (SMK37 Open, USB "SMK37 Open") recognised as an SMK-37: the list switches to it, and Install works
  (confirmed on the real thing by the user).
- The blue-screen fix: a use-after-free when the firmware list was rebuilt while scrolled (`531b776` / `db75043`).
- Crash restart backstop: the RTC watchdog is armed in the panic handler, plus the stall watch and its log.

## Hardware

- M5Stack Tab5: ESP32-P4 **revision v1.3** (esptool says `revision v1.x`), **16 MB flash**, the ESP32-C6 for
  Wi-Fi/Bluetooth (ESP-Hosted). Build env `tab5`. A `tab5-p4r3` env exists for P4 v3.x chips, but isn't tried.
- **USB-A**: the USB host (the P4's USB-OTG HS port). The FM-1 goes here (a hub works too). Its 5 V is switched on at boot.
- **USB-C**: power, and the serial console (USB-Serial/JTAG, 115200). It is never the host.
- microSD (FAT32), the same as upstream (`/fm1`, `/fm1buddy/log.txt`, ...).
- The app draws at 640x360, and each pixel goes to the 1280x720 panel as 2x2.

## What this fork adds

- **Search the firmware list by name**, on the Hub's list and the SD card's: a search box with an on-screen keyboard
  and a clear button. It matches name, author, file name or product, and ignores case.
- **FM1 Open / FM1 Community** (M-VAVE Open V14, USB `3654:7756`, product "FM1 Community"): recognised as an FM-1
  (its identities `FM1_OFFICIAL_OPEN`, `FM1_COMMUNITY_n`, `ota-FM1C_...`), and its updater is armed (cmd 0x31/0x32)
  before Install.
- **SMK-37 Pro / Elite, including SMK37 Open** (USB product "SMK37 Open", no hyphen, also `3654:7756`): recognised as
  an SMK-37 (identities `SMK-37 Pro_016`, `SMK-37 Pro_8nnnnnn`, `SMK-37 Elite_...`, earlier `SMK37P_OPEN_n`). The
  Firmware list switches to that machine, Install is enabled for SMK packages, and Open's updater is armed the same
  way as FM1 Open's.
- **Logging**: every log line goes to the USB-C console, through a log task of its own. It never waits for a computer
  that isn't reading. It also goes to `/sdcard/fm1buddy/log.txt` in batches (the run before is in `log-1.txt`). Send
  `log` on the console to print both. There are also detailed USB-A attach/enumeration logs, and the reset reason by
  name at boot.
- **Stall watch**: if the main loop stops for 8 s, `stall:` lines say where (its task state, core 1's PC samples). At 30 s
  it restarts, and the next boot logs `crash: main loop stuck at ...`.
- **Crash backstop**: the panic handler arms the RTC watchdog (15 s), so a crash always restarts. The crash is logged at
  the next boot (`crash: ...`, decode with `riscv32-esp-elf-addr2line -pfiaC -e firmware.elf ADDR...`).
- Settings > Proxy (HTTP CONNECT / SOCKS5) for the Hub and downloads.

### The "blue screen" fix

The Tab5 used to freeze on its (blue-themed) screen. The core dump showed a use-after-free. The firmware list was
rebuilt (typing a search, the Hub's list arriving, another machine or kind) while it was scrolled down.
`lv_obj_clean()` scrolls back to the top, and that scroll event filled list rows whose labels had just been deleted
(a Load access fault in `lv_obj_get_screen`). Fixed in `531b776` (pushed here as `db75043`): the rows are
dropped before the list is cleaned.

## Build

    pio run -e tab5        # PlatformIO; output in .pio/build/tab5/ (firmware.bin, firmware.factory.bin, firmware.elf)

(`pio run -e sim-tab5` builds the desktop simulator at the Tab5's size.)

## Flash

**Back up first.** Put the Tab5 in download mode (hold the power button until the green light blinks), then:

    esptool --chip esp32p4 -p PORT -b 1500000 read-flash 0 0x1000000 tab5-backup-full.bin

To recover, write it back with `esptool --chip esp32p4 -p PORT -b 1500000 write-flash 0x0 tab5-backup-full.bin`
(or use M5Stack's factory firmware, through M5Burner).

**Update and keep the settings** (Wi-Fi, theme, proxy are in NVS at 0x9000, which this doesn't touch):

    esptool --chip esp32p4 -p PORT -b 1500000 write-flash 0xe000 boot_app0.bin 0x10000 fm1buddy-tab5-p4v1-app-0x10000.bin

(`boot_app0.bin` at 0xe000 makes it boot app0. The app is `.pio/build/tab5/firmware.bin`.)

**Fresh install** (the merged image covers 0x0 up to the end of the app, and **erases the settings**, since NVS in it is
blank):

    esptool --chip esp32p4 -p PORT -b 1500000 write-flash 0x0 fm1buddy-tab5-p4v1-merged-0x0.bin

The current p4v1 build (2026-11-13) is:

- `fm1buddy-tab5-p4v1-app-0x10000.bin`: 2,323,680 bytes, sha256 `5eaf776dec42c8dfeea47f9b7fd66e4e8755f291709870797058e3dd93e08a7e`
- `fm1buddy-tab5-p4v1-merged-0x0.bin`: 2,389,216 bytes, sha256 `c6766c00b9fc96137a3cf118015231709c3f8b166d9e9ce964728bc69cec2b1e`

## Not on the Tab5, and not tried yet

- Not on the Tab5: FM-1 Recover/Unbrick (it drives the S3's own USB pins), USB disk mode, USB MIDI for a computer,
  and sleep.
- Not tried on the real thing:
  - **Install under FM1 Open** (its updater's arming: the tag is the `.fwsc`'s sha256, which Open may not take).
    Install on the SMK-37 Pro under SMK37 Open is now confirmed working by the user ("好了").
  - **Touch and orientation**: the touch mapping across the whole panel, and the 180-degree flip setting.
  - The RTC-watchdog crash backstop, and the stall watch's restart.
  - The `tab5-p4r3` build (P4 v3.x).
- More notes are in [docs/TAB5.md](docs/TAB5.md).

---

# fm1buddy

[![video](https://img.youtube.com/vi/mT8rAArBcnE/hqdefault.jpg)](https://www.youtube.com/watch?v=mT8rAArBcnE)

All without a computer/phone/etc:

- Switch the M-VAVE FM-1's firmware, on the fly (from the SD card, or over Wi-Fi from [mutant.cloud](https://mutant.cloud/))
- Switch the M-VAVE SMK-37's firmware too (Pro, Elite, MKE-P37: the FM-1's chip in a keyboard), from the SD card or over Wi-Fi from [mutant.cloud](https://mutant.cloud/?device=smk37), and back to M-VAVE's stock firmware
- Play it from cheap USB-only (up to 4, on a hub) and Bluetooth MIDI devices, even if FM-1 firmware only supports USB MIDI
- touchscreen
- nice themed menus
- Backup flash/settings/presets/etc (from stock V15, or any firmware)
- Send/record sounds (.syx, like DX7 banks)
- MIDI-routing: Bluetooth & USB devices pass notes, clock & controls to each other, or map them with rules in `midimap.txt` on the SD card (say, several controllers each onto their own FM-1 channel). Add and delete rules by touch, split a keyboard at any key, transpose, and set or scale velocity. Or build the file on the web page's MIDI map tab, learning each control from the device.
- MIDI clock: lead at a tempo (tap tempo, start/stop) or follow a device's clock, with a light on each beat
- Arpeggiator (up, down, up-down, random, as played; 1/4 to 1/32; up to 4 octaves; hold) and chord memory, for any controller (the FM-1's own notes are left alone)
- Record the FM-1's audio to WAV files on the SD card, over USB, with level meters and a clip warning (firmware that sends USB audio: Felucca, and those built on it like SLOOP; any class-compliant USB mic or sound card works too)
- Touchscreen MIDI controller: a two-finger X/Y pad (each finger sends 2 CCs, and can play notes on a scale) and 16 velocity-sensitive pads. It's a controller like any other, so mapping rules, clock, arp and chords work on it
- Big Screen: show the FM-1's screen big (Manager > Big Screen). Only on firmware with the Big Screen feature (the [bigscreen tag](https://mutant.cloud/?tag=bigscreen) on mutant.cloud), not other firmware
- Choose what it starts on: firmware manager, MIDI hub, mapper, touch controller, or Big Screen
- USB MIDI for a computer: plugged straight into one, it's a class-compliant USB MIDI device (Settings > On a computer), so the touch controller, clock and Bluetooth MIDI devices reach your DAW, and the DAW can play them
- Warns before you install firmware with no backup of the FM-1 on the SD card, with a button to make one
- Sample-management (folders of WAV to its sample-banks, and back)
- SLOOP (2.3+, and Kitten Synths): back it up in SLOOP's own file (they open in its web editor, and its backups open here), restore one, load a single project from one, save its song as a .sloopsong (the loop, sections A-D and their order, as SLOOP's editor exports them) and load those back, and load folders of WAVs into its sample slots. The Manager page says what the firmware on the FM-1 can do, and what to install for more
- Sample slots on Felucca and the firmwares built on it that kept them (Skiff, JIANT, Melodee, ...): load a folder of WAVs (or one WAV) from `/sloop/samples` into USR1-3, for its SAMPLE engine. fomajam's drum kits load from setups
- Act as USB disk (to manage files on SD)
- MIDI-out, turn FM1 into a sequencer (on supported firmware, like [fomajam](https://github.com/konsumer/fomajam))
- FM1 Recover: write a firmware or backup to an FM-1 that won't start, like [FM-1-transporter](https://github.com/kurogedelic/FM-1-transporter) with no wiring needed (when people say "use RP2040")
- Automation options, great for LLM development: switch it into USB MIDI mode from the serial console (`tools/s3.py usbmidi`), and back with a "magic" SysEx message (`tools/s3.py serial`), and switch to USB-disk (`tools/s3.py disk`) or eject disk on host to go back to serial. This means tests can run on the real thing.

It should be useful for most firmware, depending on what it supports, but it has some extra stuff that is specific to [fomajam](https://github.com/konsumer/fomajam):

- load/save song
- load/save sample-banks quickly (for swapping sets at a live show)
- export as MIDI/WAV for use n other programs

No soldering or assembly needed, just get a cheap [Freenove ESP32-S3 2.8"](https://store.freenove.com/products/fnk0104?variant=45040270213318) (FNK0104B). The same board comes in 3.5" (FNK0104N) and 4" (FNK0104S), with bigger screens (both 480x320), and its own firmware here (the 3.5"'s, a different panel with its own pins, isn't tried on one yet). Other sizes and makes won't work: the firmware is made for these boards (their screen, touch and SD pins). You might additionally need a USB power-splitter and/or a small USB hub, depending on what you are doing.

**Install it, and how to set it up:** <https://konsumer.github.io/fm1buddy/>

**Update it:** Settings > Update > Check for update, over Wi-Fi (its size's release, through the SD card). Installing again from the page keeps its settings (Wi-Fi's too), unless you choose to erase it.

<table>
  <tr>
    <td><img src="docs/screens/firmware-hub.png" width="260" alt="The Firmware Hub's list" /></td>
    <td><img src="docs/screens/firmware-details.png" width="260" alt="A firmware's details" /></td>
    <td><img src="docs/screens/firmware-install.png" width="260" alt="Installing it on the FM-1" /></td>
  </tr>
  <tr>
    <td><img src="docs/screens/midi.png" width="260" alt="The MIDI devices on the hub" /></td>
    <td><img src="docs/screens/midi-bluetooth.png" width="260" alt="Bluetooth MIDI" /></td>
    <td><img src="docs/screens/themes.png" width="260" alt="The themes" /></td>
  </tr>
  <tr>
    <td><img src="docs/screens/sounds.png" width="260" alt="Manager: what the FM-1 speaks, recording, .syx files" /></td>
    <td><img src="docs/screens/sounds-bank.png" width="260" alt="A DX7 bank on the SD card" /></td>
    <td><img src="docs/screens/settings.png" width="260" alt="Settings" /></td>
  </tr>
</table>

## Development

With [PlatformIO](https://platformio.org), the board on the computer (it's a USB serial port there; on a hub, the USB
host):

```sh
pio run -e cyd-s3 -t upload     # the 2.8" (ILI9341)
pio run -e cyd-s3-35 -t upload  # the 3.5" (ST77922 on QSPI, and its own pins)
pio run -e cyd-s3-4 -t upload   # the 4" (ST7796)
```

The app also runs on the desktop (SDL2: `brew install sdl2`), with pretend Wi-Fi, an FM-1 and a keyboard. It runs
headless from a script too, for screenshots:

```sh
pio run -e sim && .pio/build/sim/program --fm1 --keys
pio run -e sim-4 && .pio/build/sim-4/program --fm1 --keys  # at the 4"'s (and the 3.5"'s) screen size
.pio/build/sim/program --headless --script "tap 24 142; wait 300; shot settings.bmp; quit"
```

`tools/unit-test.sh` runs the tests of the MIDI mapping's rules and SLOOP's formats (`test/`: a sample slot made from
.wav files must be the bytes SLOOP's own `tools/sampleio.py` makes). `--sloop` makes the simulator's FM-1 one running
SLOOP 2.8, `--felucca` one running Felucca 1.6.1 (its 3 sample slots), `--smk` an SMK-37 Elite on its stock firmware, `--bigscreen` one whose firmware sends its screen (Manager > Big Screen). `--theme NAME` shows it in one of the themes (WHITE, NEON, ...), to check a page in all of them. `--clock` gives the simulator's keyboard
MIDI clock to follow; `press X Y`, `release`, `finger2 X Y` and `lift2` in a script hold fingers down, for the touch
controller.

`tools/install-test.sh` installs a package on the pretend FM-1, cleanly and with faults (lost and repeated requests,
garbled answers, unplugged while writing), and checks it ends up exactly as the package.

Each install on the board writes what went between it and the FM-1 to `fm1buddy/installs/N.txt` on the SD card (the
last 10 are kept): send one along with a report of an install gone wrong.

`tools/s3.py` (with `~/.platformio/penv/bin/python`) does the same on the board: `shot FILE`, `tap X Y`, `drag X1 Y1 X2 Y2`, `log` (what it
logged on the hub, also in `fm1buddy/log.txt` on its SD card), `host` (the USB host for 40 s, running an optional script of `tap X Y` and `wait MS` steps), and `usbmidi` / `serial`
(it restarts as a USB MIDI device for the computer, and back: `serial` sends it a SysEx of its own,
`F0 7D 66 6D 31 62 01 F7`, and needs `pip install python-rtmidi`), to test USB MIDI from a script. `disk` restarts it as a USB disk and prints
where its SD card is mounted; `eject` ejects it, and it comes back as the serial port by itself, so a script can copy
packages to it.

```
src/app/   the app (LVGL 9): pages, the Hub's list, MIDI between devices, the FM-1's update protocol, sounds (SysEx),
           setups and fomajam's kits and MIDI files (sounds, kits, songmidi), backups (uboot);
           hal.h is what it needs from the board
src/s3/    the board: screen, touch, SD, Wi-Fi, USB host (TinyUSB), Bluetooth MIDI (NimBLE)
src/sim/   the simulator
site/      the installer page (GitHub Pages, with the latest release)
case/      the 3D-printed case (OpenSCAD); tools/cases.sh makes its STLs
loader/    the flash loader that runs in the FM-1's RAM for backups and recovery, and draws on its screen
           (m-bootloader's, MIT): loader/build.sh builds it into src/app/mflr_loader.h, loader/test.sh tests it
```

## License

GPL-3.0-only. Credits and third-party parts: [LICENSING.md](LICENSING.md).
