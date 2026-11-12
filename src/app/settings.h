// The settings, kept across restarts
#pragma once

#include <stdint.h>

struct Settings {
  uint32_t version;
  char ssid[33];
  char pass[65];
  uint8_t theme;
  uint8_t brightness;  // percent
  bool hubAuto;        // update the Hub list the first time it's opened after power-on
};

extern Settings settings;

void settingsLoad();
void settingsSave();

// a proxy for the internet: the Hub's list, firmwares and fm1buddy's own update go through it when it's on (joining
// Wi-Fi doesn't). Settings > Proxy; kept on its own ("proxy"), so the settings above stay as they are
enum ProxyKind : uint8_t { PROXY_HTTP,  // an HTTP proxy: CONNECT for https (Clash's mixed port, 7890)
                           PROXY_SOCKS5,
                           PROXY_KINDS };
struct Proxy {
  uint32_t version;
  bool on;
  uint8_t kind;  // ProxyKind
  uint16_t port;
  char host[64];
  char user[33];  // (none: no user name or password)
  char pass[65];
};

extern Proxy proxy;

void proxySave();
// on, and with somewhere to go
inline bool proxyUsed() { return proxy.on && proxy.host[0] && proxy.port; }
