// hal::get through a proxy (Settings > Proxy): ESP-IDF's HTTP client has no proxy, so this is a small HTTP/1.1 GET of
// its own. https goes in a tunnel (HTTP CONNECT, or SOCKS5) and TLS is made inside it, with the certificate checked
// against the site's own name (not the proxy's). The same results as hal::get: the status (200: the body is at path),
// or -1 and err
#pragma once

#include <stdint.h>

#include <string>

#include "settings.h"

int proxyGet(const Proxy& p, const char* url, const char* path, std::string* etag,
             void (*progress)(uint32_t, uint32_t), std::string& err);
