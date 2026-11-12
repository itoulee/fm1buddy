#include "settings.h"

#include <string.h>

#include "hal.h"
#include "themes.h"

#define VERSION 1

#define PROXY_VERSION 1

Settings settings;
Proxy proxy;

static void proxyLoad() {
  if (hal::load("proxy", &proxy, sizeof proxy) && proxy.version == PROXY_VERSION) {
    proxy.host[sizeof proxy.host - 1] = proxy.user[sizeof proxy.user - 1] = proxy.pass[sizeof proxy.pass - 1] = 0;
    if (proxy.kind >= PROXY_KINDS) {
      proxy.kind = PROXY_HTTP;
    }
    return;
  }
  memset(&proxy, 0, sizeof proxy);  // off, no host
  proxy.version = PROXY_VERSION;
  proxy.kind = PROXY_HTTP;
  proxy.port = 7890;
}

void proxySave() {
  hal::save("proxy", &proxy, sizeof proxy);
}

void settingsLoad() {
  proxyLoad();
  if (hal::load("settings", &settings, sizeof settings) && settings.version == VERSION) {
    if (settings.theme >= THEME_COUNT) {
      settings.theme = 0;
    }
    return;
  }
  memset(&settings, 0, sizeof settings);
  settings.version = VERSION;
  settings.theme = 2;  // BLUE
  settings.brightness = 80;
  settings.hubAuto = true;
}

void settingsSave() {
  hal::save("settings", &settings, sizeof settings);
}
