// TinyUSB (the USB host, usb_s3.cpp), built from source: the framework's own TinyUSB config (its core's USB device
// code is compiled against it too), with the host's settings ours. TinyUSB's DWC2 host takes one of the S3's 8 USB
// channels only while a transfer is going, so more devices fit than with ESP-IDF's host (a channel for every open
// endpoint, all the time)
#pragma once

#ifdef BOARD_TAB5
// the P4 (the framework's config doesn't say, and its default is the S2): its USB-OTG FS is port 0, HS port 1
#ifndef CFG_TUSB_MCU
#define CFG_TUSB_MCU OPT_MCU_ESP32P4
#endif
#endif

#include "tusb_config.h"  // the framework's (arduino_tinyusb/include)

#undef BOARD_TUH_RHPORT
#ifdef BOARD_TAB5
#define BOARD_TUH_RHPORT 1  // the Tab5's USB-A port: the P4's USB-OTG HS (TinyUSB's port 1)
// the host's DMA goes through the P4's data cache: each buffer it reads or writes on a cache line of its own (64
// bytes), and whole lines (TinyUSB writes them back and drops them around a transfer)
#define USB_DMA_ALIGN 64
#else
#define BOARD_TUH_RHPORT 0  // the S3 has one USB controller
#define USB_DMA_ALIGN 4
#endif
#define CFG_TUH_MAX_SPEED OPT_MODE_FULL_SPEED

#undef CFG_TUH_ENUMERATION_BUFSIZE
#define CFG_TUH_ENUMERATION_BUFSIZE 512  // config descriptors: a keyboard's can be long
#undef CFG_TUH_DEVICE_MAX
#define CFG_TUH_DEVICE_MAX 8
#undef CFG_TUH_MIDI
#define CFG_TUH_MIDI 4  // midi.h's MIDI_USB
#define CFG_TUH_MIDI_STREAM_API 0
#define CFG_TUH_DWC2_ENDPOINT_MAX 24
#define CFG_TUH_API_EDPT_XFER 1  // the FM-1's boot loader: a storage device spoken to here, without TinyUSB's driver
// DMA: the USB core itself retries a packet a device refuses (NAK: the FM-1's update loader, busy writing its flash).
// Without it (slave mode) TinyUSB restarts the transfer in software, and a send to the loader hung there for good.
// Every transfer's buffer is then in internal RAM and 4-byte aligned (usb_s3.cpp's are)
#define CFG_TUH_DWC2_DMA_ENABLE 1

// not used: their buffers stay out of the internal RAM
#undef CFG_TUH_CDC
#define CFG_TUH_CDC 0
#undef CFG_TUH_HID
#define CFG_TUH_HID 0
#undef CFG_TUH_MSC
#define CFG_TUH_MSC 0
