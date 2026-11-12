# NimBLE-IDF (for fm1buddy's Tab5 build)

NimBLE-Arduino 2.5.1's C++ API (`src/*.h`, `src/*.cpp`, by h2zero, Apache-2.0: LICENSE, NOTICE), without its bundled
NimBLE host and controller port: on the ESP32-P4 those don't build (it has no Bluetooth of its own), and the framework
already has ESP-IDF's NimBLE host, talking to the Tab5's ESP32-C6 over ESP-Hosted (VHCI over SDIO).

Changes from NimBLE-Arduino 2.5.1:

- `nimconfig.h`: only the C++ API's own settings, the host's are the framework's sdkconfig; and no
  `USING_NIMBLE_ARDUINO_HEADERS`, so the headers are ESP-IDF's (as with esp-nimble-cpp)
- `NimBLEDevice.cpp`: `hostedInitBLE()` before `nimble_port_init()`, `hostedDeinitBLE()` after `nimble_port_deinit()`
