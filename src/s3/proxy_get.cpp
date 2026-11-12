#include "proxy_get.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <mbedtls/base64.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/ssl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>

#ifdef ESP_PLATFORM
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#else
#include <mbedtls/x509_crt.h>
#endif

#define TIMEOUT_MS 15000
#ifndef MBEDTLS_ERR_NET_SEND_FAILED  // (mbedtls/net_sockets.h)
#define MBEDTLS_ERR_NET_SEND_FAILED -0x004E
#define MBEDTLS_ERR_NET_RECV_FAILED -0x004C
#endif

namespace {

struct Url {
  bool tls = false;
  std::string host, path;
  int port = 0;
};

bool parseUrl(const std::string& u, Url& o) {
  size_t at;
  if (!strncasecmp(u.c_str(), "https://", 8)) {
    o.tls = true, o.port = 443, at = 8;
  } else if (!strncasecmp(u.c_str(), "http://", 7)) {
    o.tls = false, o.port = 80, at = 7;
  } else {
    return false;
  }
  size_t slash = u.find_first_of("/?#", at);
  std::string auth = u.substr(at, slash == std::string::npos ? std::string::npos : slash - at);
  o.path = slash == std::string::npos ? "/" : u.substr(slash);
  if (o.path[0] != '/') {
    o.path = "/" + o.path;
  }
  size_t hash = o.path.find('#');
  if (hash != std::string::npos) {
    o.path.erase(hash);
  }
  size_t a = auth.rfind('@');  // (user:pass@ isn't sent)
  if (a != std::string::npos) {
    auth.erase(0, a + 1);
  }
  size_t colon = auth.rfind(':');
  if (colon != std::string::npos && auth.find(']') == std::string::npos) {
    o.port = atoi(auth.c_str() + colon + 1);
    auth.erase(colon);
  }
  o.host = auth;
  return !o.host.empty() && o.port > 0 && o.port < 65536;
}

// "host" or "host:port" as Host: and CONNECT want it
std::string hostPort(const Url& u) {
  bool plain = u.port == (u.tls ? 443 : 80);
  return plain ? u.host : u.host + ":" + std::to_string(u.port);
}

// where a redirect goes: Location against the URL it came from
std::string resolve(const Url& from, const std::string& loc) {
  if (!strncasecmp(loc.c_str(), "https://", 8) || !strncasecmp(loc.c_str(), "http://", 7)) {
    return loc;
  }
  std::string base = std::string(from.tls ? "https://" : "http://") + hostPort(from);
  if (loc.compare(0, 2, "//") == 0) {
    return std::string(from.tls ? "https:" : "http:") + loc;
  }
  if (!loc.empty() && loc[0] == '/') {
    return base + loc;
  }
  std::string dir = from.path.substr(0, from.path.find('?'));
  dir.erase(dir.rfind('/') + 1);
  return base + dir + loc;
}

// a connection: TCP to the proxy, maybe a tunnel through it, maybe TLS in that
struct Conn {
  int fd = -1;
  bool tls = false;
  bool tlsUp = false;
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config conf;
  mbedtls_ctr_drbg_context drbg;
  mbedtls_entropy_context ent;
#ifndef ESP_PLATFORM
  mbedtls_x509_crt ca;
#endif
  // what's been read but not taken yet
  char* buf = 0;
  int have = 0, at = 0;

  ~Conn() {
    if (tlsUp) {
      mbedtls_ssl_close_notify(&ssl);
    }
    if (tls) {
      mbedtls_ssl_free(&ssl);
      mbedtls_ssl_config_free(&conf);
      mbedtls_ctr_drbg_free(&drbg);
      mbedtls_entropy_free(&ent);
#ifndef ESP_PLATFORM
      mbedtls_x509_crt_free(&ca);
#endif
    }
    if (fd >= 0) {
      close(fd);
    }
    free(buf);
  }

  bool tcp(const char* host, int port, std::string& err) {
    char ps[8];
    snprintf(ps, sizeof ps, "%d", port);
    struct addrinfo hints = {}, *res = 0;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, ps, &hints, &res) || !res) {
      err = std::string("Can't find the proxy, ") + host + ".";
      return false;
    }
    for (struct addrinfo* a = res; a && fd < 0; a = a->ai_next) {
      int s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
      if (s < 0) {
        continue;
      }
      int fl = fcntl(s, F_GETFL, 0);
      fcntl(s, F_SETFL, fl | O_NONBLOCK);
      int r = connect(s, a->ai_addr, a->ai_addrlen);
      if (r < 0 && errno == EINPROGRESS) {
        fd_set w;
        FD_ZERO(&w);
        FD_SET(s, &w);
        struct timeval tv = {TIMEOUT_MS / 1000, 0};
        r = select(s + 1, 0, &w, 0, &tv) == 1 ? 0 : -1;
        int e = 0;
        socklen_t el = sizeof e;
        if (!r && (getsockopt(s, SOL_SOCKET, SO_ERROR, &e, &el) || e)) {
          r = -1;
        }
      }
      if (r) {
        close(s);
        continue;
      }
      fcntl(s, F_SETFL, fl & ~O_NONBLOCK);
      struct timeval tv = {TIMEOUT_MS / 1000, 0};
      setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
      setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
      fd = s;
    }
    freeaddrinfo(res);
    if (fd < 0) {
      err = std::string("Can't reach the proxy at ") + host + ":" + ps +
            " (is it on, and does it let in connections from the network?).";
      return false;
    }
    return true;
  }

  // the socket itself (before TLS)
  bool sendRaw(const void* p, size_t n) {
    const char* c = (const char*)p;
    while (n) {
      ssize_t k = ::send(fd, c, n, 0);
      if (k <= 0) {
        if (k < 0 && errno == EINTR) {
          continue;
        }
        return false;
      }
      c += k, n -= k;
    }
    return true;
  }

  int recvRaw(void* p, size_t n) {
    for (;;) {
      ssize_t k = ::recv(fd, p, n, 0);
      if (k < 0 && errno == EINTR) {
        continue;
      }
      return (int)k;
    }
  }

  static int bioSend(void* c, const unsigned char* p, size_t n) {
    ssize_t k = ::send(((Conn*)c)->fd, p, n, 0);
    if (k < 0) {
      return errno == EINTR ? MBEDTLS_ERR_SSL_WANT_WRITE : MBEDTLS_ERR_NET_SEND_FAILED;
    }
    return (int)k;
  }

  static int bioRecv(void* c, unsigned char* p, size_t n) {
    ssize_t k = ::recv(((Conn*)c)->fd, p, n, 0);
    if (k < 0) {
      if (errno == EINTR) {
        return MBEDTLS_ERR_SSL_WANT_READ;
      }
      return errno == EAGAIN || errno == EWOULDBLOCK ? MBEDTLS_ERR_SSL_TIMEOUT : MBEDTLS_ERR_NET_RECV_FAILED;
    }
    return (int)k;
  }

  // TLS to host, in whatever the socket reaches (the tunnel): the certificate is checked for host, the site
  bool startTls(const std::string& host, std::string& err) {
    tls = true;
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_entropy_init(&ent);
#ifndef ESP_PLATFORM
    mbedtls_x509_crt_init(&ca);
#endif
    int r = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &ent, (const unsigned char*)"fm1buddy", 8);
    if (!r) {
      r = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    }
    if (r) {
      err = "Can't start TLS (" + std::to_string(r) + ").";
      return false;
    }
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
#ifdef ESP_PLATFORM
    if (esp_crt_bundle_attach(&conf)) {
      err = "Can't load the certificates.";
      return false;
    }
#else
    const char* caf = getenv("CA_FILE");
    mbedtls_x509_crt_parse_file(&ca, caf ? caf : "/etc/ssl/certs/ca-certificates.crt");
    mbedtls_ssl_conf_ca_chain(&conf, &ca, 0);
#endif
    if ((r = mbedtls_ssl_setup(&ssl, &conf)) || (r = mbedtls_ssl_set_hostname(&ssl, host.c_str()))) {
      err = "Can't start TLS (" + std::to_string(r) + ").";
      return false;
    }
    mbedtls_ssl_set_bio(&ssl, this, bioSend, bioRecv, 0);
    while ((r = mbedtls_ssl_handshake(&ssl))) {
      if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
        continue;
      }
      uint32_t v = mbedtls_ssl_get_verify_result(&ssl);
      if (r == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED || (v && v != (uint32_t)-1)) {
        err = "The certificate isn't " + host + "'s (or isn't trusted): is the proxy looking inside?";
      } else {
        char e[80];
        mbedtls_strerror(r, e, sizeof e);
        err = "TLS to " + host + " failed through the proxy (" + e + ").";
      }
      return false;
    }
    tlsUp = true;
    return true;
  }

  bool write(const std::string& s) {
    if (!tls) {
      return sendRaw(s.data(), s.size());
    }
    const unsigned char* p = (const unsigned char*)s.data();
    size_t n = s.size();
    while (n) {
      int k = mbedtls_ssl_write(&ssl, p, n);
      if (k == MBEDTLS_ERR_SSL_WANT_READ || k == MBEDTLS_ERR_SSL_WANT_WRITE) {
        continue;
      }
      if (k <= 0) {
        return false;
      }
      p += k, n -= k;
    }
    return true;
  }

  // < 0: failed; 0: the end
  int readSome(char* p, int n) {
    if (at < have) {
      int k = std::min(n, have - at);
      memcpy(p, buf + at, k);
      at += k;
      return k;
    }
    if (!tls) {
      return recvRaw(p, n);
    }
    for (;;) {
      int k = mbedtls_ssl_read(&ssl, (unsigned char*)p, n);
      if (k == MBEDTLS_ERR_SSL_WANT_READ || k == MBEDTLS_ERR_SSL_WANT_WRITE) {
        continue;
      }
      if (k == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
        return 0;
      }
      return k;
    }
  }

  // a byte at a time is slow for a body: lines go through buf
  bool fill() {
    if (!buf) {
      buf = (char*)malloc(2048);
      if (!buf) {
        return false;
      }
    }
    at = have = 0;
    int k = readSome(buf, 2048);
    at = 0;
    if (k <= 0) {
      return false;
    }
    have = k;
    return true;
  }

  // a line, without its CRLF (false: the connection ended, or one too long)
  bool readLine(std::string& l, size_t max = 16384) {
    l.clear();
    for (;;) {
      if (at >= have && !fill()) {
        return false;
      }
      char c = buf[at++];
      if (c == '\n') {
        if (!l.empty() && l.back() == '\r') {
          l.pop_back();
        }
        return true;
      }
      l += c;
      if (l.size() > max) {
        return false;
      }
    }
  }

  // exactly n bytes from the socket (SOCKS5's replies)
  bool readExact(void* p, int n) {
    char* c = (char*)p;
    while (n > 0) {
      int k = readSome(c, n);
      if (k <= 0) {
        return false;
      }
      c += k, n -= k;
    }
    return true;
  }
};

std::string basicAuth(const Proxy& p) {
  std::string up = std::string(p.user) + ":" + p.pass;
  unsigned char out[160];
  size_t n = 0;
  if (mbedtls_base64_encode(out, sizeof out, &n, (const unsigned char*)up.data(), up.size())) {
    return "";
  }
  return "Proxy-Authorization: Basic " + std::string((char*)out, n) + "\r\n";
}

std::string lower(std::string s) {
  for (char& ch : s) {
    ch = (char)tolower((unsigned char)ch);
  }
  return s;
}

// the status line's code, and the headers to the empty line
bool readHead(Conn& c, int& status, std::string* etag, std::string* loc, int64_t* len, bool* chunked) {
  std::string l;
  if (!c.readLine(l) || l.compare(0, 5, "HTTP/")) {
    return false;
  }
  size_t sp = l.find(' ');
  status = sp == std::string::npos ? -1 : atoi(l.c_str() + sp + 1);
  if (len) {
    *len = -1;
  }
  if (chunked) {
    *chunked = false;
  }
  for (;;) {
    if (!c.readLine(l)) {
      return false;
    }
    if (l.empty()) {
      return status > 0;
    }
    size_t colon = l.find(':');
    if (colon == std::string::npos) {
      continue;
    }
    std::string k = l.substr(0, colon);
    size_t v0 = l.find_first_not_of(" \t", colon + 1);
    std::string v = v0 == std::string::npos ? "" : l.substr(v0);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) {
      v.pop_back();
    }
    if (etag && !strcasecmp(k.c_str(), "ETag")) {
      *etag = v;
    } else if (loc && !strcasecmp(k.c_str(), "Location")) {
      *loc = v;
    } else if (len && !strcasecmp(k.c_str(), "Content-Length")) {
      *len = strtoll(v.c_str(), 0, 10);
    } else if (chunked && !strcasecmp(k.c_str(), "Transfer-Encoding") && lower(v).find("chunked") != std::string::npos) {
      *chunked = true;
    }
  }
}

// through the proxy to the site: a tunnel (CONNECT, or SOCKS5)
bool tunnel(Conn& c, const Proxy& p, const Url& u, std::string& err) {
  if (p.kind == PROXY_HTTP) {
    std::string hp = u.host + ":" + std::to_string(u.port);
    std::string req = "CONNECT " + hp + " HTTP/1.1\r\nHost: " + hp + "\r\n" + (p.user[0] ? basicAuth(p) : "") +
                      "User-Agent: fm1buddy\r\n\r\n";
    int status = -1;
    if (!c.write(req) || !readHead(c, status, 0, 0, 0, 0)) {
      err = "The proxy didn't answer (is it an HTTP proxy?).";
      return false;
    }
    if (status == 407) {
      err = p.user[0] ? "The proxy didn't take the user name and password (407)."
                      : "The proxy wants a user name and password (407).";
      return false;
    }
    if (status / 100 != 2) {
      err = "The proxy couldn't reach " + u.host + " (" + std::to_string(status) + ").";
      return false;
    }
    return true;
  }
  // SOCKS5 (RFC 1928, and 1929 for a user name and password): the site by its name, which the proxy looks up
  bool login = p.user[0];
  unsigned char hi[4] = {5, (unsigned char)(login ? 2 : 1), 0, 2};
  unsigned char r[4];
  if (!c.sendRaw(hi, login ? 4 : 3) || !c.readExact(r, 2) || r[0] != 5) {
    err = "The proxy didn't answer (is it a SOCKS5 proxy?).";
    return false;
  }
  if (r[1] == 2) {
    if (!login) {
      err = "The proxy wants a user name and password.";
      return false;
    }
    std::string a;
    a += (char)1;
    a += (char)strlen(p.user);
    a += p.user;
    a += (char)strlen(p.pass);
    a += p.pass;
    if (!c.sendRaw(a.data(), a.size()) || !c.readExact(r, 2) || r[1] != 0) {
      err = "The proxy didn't take the user name and password.";
      return false;
    }
  } else if (r[1] != 0) {
    err = "The proxy takes none of the ways in fm1buddy has.";
    return false;
  }
  if (u.host.size() > 255) {
    err = "The site's name is too long.";
    return false;
  }
  std::string q;
  q += (char)5, q += (char)1, q += (char)0, q += (char)3;
  q += (char)u.host.size();
  q += u.host;
  q += (char)(u.port >> 8), q += (char)(u.port & 0xFF);
  if (!c.sendRaw(q.data(), q.size()) || !c.readExact(r, 4) || r[0] != 5) {
    err = "The proxy didn't answer.";
    return false;
  }
  if (r[1] != 0) {
    static const char* const WHY[] = {"", "it failed", "not allowed", "no network", "no route to it", "refused",
                                      "timed out", "not supported", "not supported"};
    err = "The proxy couldn't reach " + u.host + " (" + (r[1] < 9 ? WHY[r[1]] : "error") + ").";
    return false;
  }
  int skip = r[3] == 1 ? 4 : r[3] == 4 ? 16 : -1;
  unsigned char tmp[260];
  if (skip < 0) {
    if (r[3] != 3 || !c.readExact(tmp, 1)) {
      err = "The proxy's answer didn't make sense.";
      return false;
    }
    skip = tmp[0];
  }
  if (!c.readExact(tmp, skip + 2)) {
    err = "The proxy didn't answer.";
    return false;
  }
  return true;
}

// the body: to f, as long as it is (or chunked, or to the end)
bool readBody(Conn& c, FILE* f, int64_t len, bool chunked, void (*progress)(uint32_t, uint32_t), std::string& err) {
#ifdef ESP_PLATFORM
  static char* b = (char*)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
#else
  static char* b = (char*)malloc(4096);
#endif
  uint32_t got = 0;
  uint32_t total = len > 0 ? (uint32_t)len : 0;
  auto take = [&](int64_t n) -> bool {  // n bytes (< 0: to the end)
    while (n != 0) {
      int want = n < 0 || n > 4096 ? 4096 : (int)n;
      int k = c.readSome(b, want);
      if (k == 0 && n < 0) {
        return true;
      }
      if (k <= 0) {
        err = "The download stopped part way.";
        return false;
      }
      if (fwrite(b, 1, k, f) != (size_t)k) {
        err = "Can't write to the SD card (is it full?).";
        return false;
      }
      got += k;
      if (n > 0) {
        n -= k;
      }
      if (progress) {
        progress(got, total);
      }
    }
    return true;
  };
  if (!chunked) {
    return take(len);
  }
  std::string l;
  for (;;) {
    if (!c.readLine(l)) {
      err = "The download stopped part way.";
      return false;
    }
    int64_t n = strtoll(l.c_str(), 0, 16);
    if (n <= 0) {
      break;
    }
    if (!take(n) || !c.readLine(l)) {
      if (err.empty()) {
        err = "The download stopped part way.";
      }
      return false;
    }
  }
  while (c.readLine(l) && !l.empty()) {  // (trailers)
  }
  return true;
}

}  // namespace

int proxyGet(const Proxy& p, const char* url, const char* path, std::string* etag,
             void (*progress)(uint32_t, uint32_t), std::string& err) {
  std::string now = url;
  for (int hops = 0; hops < 5; hops++) {
    Url u;
    if (!parseUrl(now, u)) {
      err = "Not a web address: " + now;
      return -1;
    }
    Conn c;
    if (!c.tcp(p.host, p.port, err)) {
      return -1;
    }
    // https always in a tunnel; http in one too through SOCKS5, and to an HTTP proxy as itself (the whole URL)
    bool viaProxy = p.kind == PROXY_HTTP && !u.tls;
    if (!viaProxy && !tunnel(c, p, u, err)) {
      return -1;
    }
    if (u.tls && !c.startTls(u.host, err)) {
      return -1;
    }
    std::string req = "GET " + (viaProxy ? "http://" + hostPort(u) + u.path : u.path) + " HTTP/1.1\r\n";
    req += "Host: " + hostPort(u) + "\r\n";
    req += "User-Agent: fm1buddy\r\nAccept: */*\r\nAccept-Encoding: identity\r\nConnection: close\r\n";
    if (viaProxy && p.user[0]) {
      req += basicAuth(p);
    }
    if (etag && !etag->empty()) {
      req += "If-None-Match: " + *etag + "\r\n";
    }
    req += "\r\n";
    int status = -1;
    std::string tag, loc;
    int64_t len = -1;
    bool chunked = false;
    if (!c.write(req) || !readHead(c, status, &tag, &loc, &len, &chunked)) {
      err = "Can't reach it through the proxy (no answer from " + u.host + ").";
      return -1;
    }
    if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
      if (loc.empty()) {
        err = "The server's redirect didn't say where to.";
        return -1;
      }
      now = resolve(u, loc);
      continue;
    }
    if (viaProxy && status == 407) {
      err = "The proxy wants a user name and password (407).";
      return -1;
    }
    if (status != 200) {
      return status;
    }
    std::string part = std::string(path) + ".part";
    FILE* f = fopen(part.c_str(), "wb");
    if (!f) {
      err = "Can't write to the SD card.";
      return -1;
    }
    bool ok = readBody(c, f, len, chunked, progress, err);
    fclose(f);
    remove(path);
    if (!ok || rename(part.c_str(), path)) {
      if (ok) {
        err = "Can't rename it on the SD card.";
      }
      remove(part.c_str());
      return -1;
    }
    if (etag) {
      *etag = tag;
    }
    return 200;
  }
  err = "Too many redirects.";
  return -1;
}
