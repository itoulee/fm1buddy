# fm1buddy on the M5Stack Tab5 (unofficial port)

`pio run -e tab5` (ESP32-P4 revision v1.x, the "ES" chips most Tab5s have) or `pio run -e tab5-p4r3` (revision v3.x).
Which one: `esptool --chip esp32p4 -p PORT flash-id` prints the chip's revision as it connects. Each makes
`.pio/build/<env>/firmware.factory.bin`, the whole flash from 0x0.

## What's different from the CYD-S3 boards

| | CYD-S3 | Tab5 |
| --- | --- | --- |
| Screen | ILI9341 / ST7796 / ST77922, 320x240 or 480x320 | 5" 1280x720 MIPI-DSI (ILI9881C, ST7123 or ST7121) through M5GFX. The app draws 640x360, shown 2x2 (`src/tab5/display_tab5.cpp`) |
| Touch | FT6336 / ST77922's | GT911 or the ST7123/ST7121's, through M5GFX (`src/tab5/touch_tab5.cpp`) |
| USB host | the one USB-C port (host, or serial for a computer) | the USB-A port: the P4's USB-OTG HS, TinyUSB's port 1, UTMI PHY at full speed. Its 5 V is switched on at start (IO expander, M5Unified) |
| Serial console | USB-C, until the host takes it | USB-C (USB-Serial-JTAG), always, with the host running |
| Wi-Fi, Bluetooth | the S3's | the ESP32-C6's, over SDIO (ESP-Hosted). Bluetooth: NimBLE-Arduino's API on the framework's NimBLE (`tab5lib/NimBLE-IDF`) |
| SD card | SD_MMC | SD_MMC 4-bit (slot 0), else 1-bit, else SPI |
| Battery | ADC | INA226 (two cells: the app is told one cell's voltage) |
| Sleep | deep sleep, touch wakes | none (off: double-click the power button) |
| FM-1 Recover (Unbrick) | yes | no: its key is sent on the S3's own USB pins, as GPIOs; the P4's USB-A is its HS PHY's own pins |
| USB disk, USB MIDI for a computer | yes | no: the P4's USB-C is USB-Serial-JTAG only |
| Self-update over Wi-Fi | yes | no: the releases have no `fm1buddy-tab5.bin` |

Settings has "Turn screen 180" on the Tab5 (kept; the board restarts).

`pio run -e sim-tab5` is the simulator at the Tab5's size (640x360).

None of this has run on a Tab5 yet: it builds, and the simulator shows the layout at 640x360.

## Flashing without losing the settings

The merged image (`firmware.factory.bin`, written at 0x0) covers the NVS partition (0x9000-0xDFFF) with 0xFF, so
it clears the settings (Wi-Fi, theme, proxy) each time. To update and keep them, write only otadata and the app:

    esptool --chip esp32p4 -b 1500000 write-flash 0xe000 boot_app0.bin 0x10000 firmware.bin

## The log, and a main loop that stops

Every log line (any task's) goes to a queue that a task of its own writes out: to the USB-C console (HWCDC, tx
timeout 0: a computer that isn't reading never holds anything up; the newest 8 KB wait for it) and, in batches, to
`/sdcard/fm1buddy/log.txt` (the run before: `log-1.txt`). So the log goes on when the main loop doesn't.

The main loop says where it is each time round (`board::beat`). If it doesn't for 8 s, the log has `stall:` lines: where
it was, its task's state and where it was switched out, eight samples of core 1's PC (the P4's PC recorder), and how
often core 1's idle task, the USB host task and USB events ran in the last second. At 30 s it restarts, and the next
start logs `crash: main loop stuck at ...`. Decode the addresses with
`riscv32-esp-elf-addr2line -pfiaC -e firmware.elf ADDR...` against the same build's firmware.elf.
