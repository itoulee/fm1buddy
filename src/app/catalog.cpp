#include "catalog.h"

#include <ArduinoJson.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <mutex>

#include "fm1.h"
#include "hal.h"

std::vector<Firmware> catalog::hub, catalog::sd;
std::string catalog::hubUpdated;

std::string catalog::path(const std::string& p) {
  const char* root = hal::sd();
  return root ? root + p : "";
}

static bool readAll(const std::string& p, std::string& out) {
  FILE* f = fopen(p.c_str(), "rb");
  if (!f) {
    return false;
  }
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  out.resize(n > 0 ? n : 0);
  bool ok = n > 0 && fread(&out[0], 1, n, f) == (size_t)n;
  fclose(f);
  return ok;
}

static std::string baseName(const std::string& p) {
  size_t s = p.rfind('/');
  return s == std::string::npos ? p : p.substr(s + 1);
}

// the screen's fonts are ASCII: accented letters lose their accents, curly quotes and dashes straighten
static std::string ascii(const char* s) {
  static const char* LATIN =  // U+00C0 to U+017F
      "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty"
      "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiJjJjKkkLlLlLlLlLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";
  std::string o;
  const uint8_t* p = (const uint8_t*)s;
  while (*p) {
    uint32_t c = *p;
    int n = c < 0x80 ? 0 : c >= 0xF0 ? 3
                       : c >= 0xE0   ? 2
                       : c >= 0xC0   ? 1
                                     : 0;
    c &= n == 3 ? 0x07 : n == 2 ? 0x0F
                     : n == 1   ? 0x1F
                                : 0xFF;
    p++;
    for (int i = 0; i < n && (*p & 0xC0) == 0x80; i++) {
      c = c << 6 | (*p++ & 0x3F);
    }
    if (c < 0x80) {
      o += (char)c;
    } else if (c >= 0xC0 && c <= 0x17F) {
      o += LATIN[c - 0xC0];
      if (c == 0xDF)
        o += 's';  // ss
    } else if (c == 0x2018 || c == 0x2019) {
      o += '\'';
    } else if (c == 0x201C || c == 0x201D) {
      o += '"';
    } else if (c == 0x2013 || c == 0x2014) {
      o += '-';
    } else if (c == 0x2026) {
      o += "...";
    } else if (c == 0xA0) {
      o += ' ';
    } else if (c >= 0x1F000 || (c >= 0x2600 && c <= 0x27BF) || c == 0xFE0F) {
      // emoji: left out
    } else {
      o += '?';
    }
  }
  return o;
}

static void readEntry(JsonObjectConst e, Firmware& f) {
  f.name = ascii(e["name"] | "?");
  f.author = ascii(e["author"] | "");
  f.version = e["version"] | "";
  f.date = e["date"] | "";
  f.what = ascii(e["what"] | "");
  f.pkg = e["pkg"] | "";
  f.file = baseName(e["file"] | f.pkg.c_str());
  f.product = e["product"] | "";
  f.sha256 = e["sha256"] | "";
  f.size = e["size"] | 0;
  f.beta = e["beta"] | false;
  f.stock = e["stock"] | false;
  f.keyboard = e["hid"]["keyboard"] | false;
  f.mouse = e["hid"]["mouse"] | false;
  JsonVariantConst k = e["kind"];
  if (k.is<const char*>()) {
    f.kinds.push_back(k.as<const char*>());
  } else {
    for (JsonVariantConst v : k.as<JsonArrayConst>()) {
      f.kinds.push_back(v | "");
    }
  }
}

// the list in the PSRAM (hal::psramFirst): read, it's 80+ KB of small blocks while it's made, and 40+ KB kept
static bool parseHubIn(std::vector<Firmware>& out, std::string& updated, std::string& err);

// ------------------------------------------------------------------------------------------ HUB_LIST ---
// the list as the board needs it: a line "fm1buddy hub 1" and when the list was made, then a line a firmware, its
// fields between tabs
static const char* const LIST_HEAD = "fm1buddy hub 1";

static void field(FILE* f, const std::string& v, char end = '\t') {
  for (char c : v) {
    fputc(c == '\t' || c == '\n' || c == '\r' ? ' ' : c, f);
  }
  fputc(end, f);
}

static void writeList(const std::vector<Firmware>& list, const std::string& updated) {
  std::string p = catalog::path(HUB_LIST);
  FILE* f = fopen((p + ".part").c_str(), "w");
  if (!f) {
    hal::log("hub: can't write %s.part (errno %d)", HUB_LIST, errno);
    return;
  }
  fprintf(f, "%s\t", LIST_HEAD);
  field(f, updated, '\n');
  for (const Firmware& e : list) {
    for (const std::string* v : {&e.name, &e.author, &e.version, &e.date, &e.what, &e.pkg, &e.file, &e.product, &e.sha256}) {
      field(f, *v);
    }
    fprintf(f, "%lu\t%c%c%c%c\t", (unsigned long)e.size, e.beta ? 'b' : '-', e.stock ? 's' : '-', e.keyboard ? 'k' : '-',
            e.mouse ? 'm' : '-');
    std::string kinds;
    for (const std::string& k : e.kinds) {
      kinds += (kinds.empty() ? "" : ",") + k;
    }
    field(f, kinds, '\n');
  }
  bool ok = !ferror(f);
  ok = !fclose(f) && ok;
  if (!ok) {
    hal::log("hub: writing %s.part failed (errno %d)", HUB_LIST, errno);
    return;
  }
  remove(p.c_str());
  if (rename((p + ".part").c_str(), p.c_str())) {
    hal::log("hub: can't rename %s.part (errno %d)", HUB_LIST, errno);
  }
}

// line's fields between tabs
static std::vector<std::string> fields(const char* line, const char* end) {
  std::vector<std::string> out;
  const char* a = line;
  for (const char* c = line;; c++) {
    if (c == end || *c == '\t') {
      out.emplace_back(a, c);
      a = c + 1;
      if (c == end) {
        return out;
      }
    }
  }
}

bool catalog::loadHub(std::vector<Firmware>& out, std::string& updated) {
  std::string text;
  if (!readAll(catalog::path(HUB_LIST), text)) {
    return false;
  }
  const char* p = text.c_str();
  const char* end = p + text.size();
  const char* nl = (const char*)memchr(p, '\n', end - p);
  if (!nl) {
    return false;
  }
  std::vector<std::string> head = fields(p, nl);
  if (head.size() != 2 || head[0] != LIST_HEAD) {
    return false;
  }
  hal::psramFirst(true);
  std::vector<Firmware> list;
  for (p = nl + 1; p < end; p = nl + 1) {
    nl = (const char*)memchr(p, '\n', end - p);
    if (!nl) {
      nl = end;
    }
    std::vector<std::string> v = fields(p, nl);
    if (v.size() != 12) {
      continue;
    }
    Firmware f;
    f.name = v[0], f.author = v[1], f.version = v[2], f.date = v[3], f.what = v[4];
    f.pkg = v[5], f.file = v[6], f.product = v[7], f.sha256 = v[8];
    f.size = strtoul(v[9].c_str(), 0, 10);
    f.beta = v[10].find('b') != std::string::npos;
    f.stock = v[10].find('s') != std::string::npos;
    f.keyboard = v[10].find('k') != std::string::npos;
    f.mouse = v[10].find('m') != std::string::npos;
    for (size_t a = 0; a < v[11].size();) {
      size_t c = v[11].find(',', a);
      c = c == std::string::npos ? v[11].size() : c;
      f.kinds.push_back(v[11].substr(a, c - a));
      a = c + 1;
    }
    list.push_back(std::move(f));
  }
  out.swap(list);
  hal::psramFirst(false);
  updated = head[1];
  return true;
}

bool catalog::parseHub(std::vector<Firmware>& out, std::string& updated, std::string& err) {
  hal::psramFirst(true);
  bool ok = parseHubIn(out, updated, err);
  hal::psramFirst(false);
  if (ok) {
    writeList(out, updated);
    remove(catalog::path(HUB_CACHE).c_str());  // (only the list is kept)
  }
  return ok;
}

static void readFirmwares(JsonArrayConst list, std::vector<Firmware>& out) {
  size_t i = 0;
  for (JsonObjectConst e : list) {
    if (!(i++ & 15)) {
      hal::idle();  // (the list is long: let the board's idle task run)
    }
    Firmware f;
    readEntry(e, f);
    if (f.stock) {
      f.kinds = {"stock"};  // M-VAVE's own, whatever the list's tags say
    }
    if (f.pkg.compare(0, 8, "https://") == 0 && f.size && f.sha256.size() == 64 &&
        f.pkg.find("..") == std::string::npos && !f.file.empty() && f.file.find("..") == std::string::npos) {
      out.push_back(f);
    }
  }
}

// the list is 400+ KB: read in 16 KB steps, so the board's idle task (which its watchdog watches) runs now and then
struct PacedText {
  const char* p;
  size_t n, i = 0, since = 0;
  void pace(size_t k) {
    if ((since += k) < 0x4000) {
      return;
    }
    since = 0;
    hal::idle();
  }
  int read() {
    if (i >= n) {
      return -1;
    }
    pace(1);
    return (uint8_t)p[i++];
  }
  size_t readBytes(char* b, size_t len) {
    size_t k = n - i < len ? n - i : len;
    memcpy(b, p + i, k);
    i += k;
    pace(k);
    return k;
  }
};

static bool parseHubIn(std::vector<Firmware>& out, std::string& updated, std::string& err) {
  std::string text;
  if (!readAll(catalog::path(HUB_CACHE), text)) {
    err = "The Hub's list isn't on the SD card yet.";
    return false;
  }
  JsonDocument filter;
  filter["updated"] = true;
  for (const char* k : {"name", "author", "version", "date", "what", "pkg", "file", "product", "sha256", "size",
                        "beta", "stock", "hid", "kind"}) {
    filter["firmwares"][0][k] = true;
  }
  for (const char* k : {"pkg", "product", "sha256", "size"}) {
    filter["stock"][k] = true;
  }
  JsonDocument doc;
  PacedText in{text.data(), text.size()};
  DeserializationError bad = deserializeJson(doc, in, DeserializationOption::Filter(filter));
  text = std::string();
  if (bad) {
    err = std::string("The Hub's list didn't read (") + bad.c_str() + ").";
    return false;
  }
  out.clear();
  updated = doc["updated"] | "";
  JsonObjectConst stock = doc["stock"];
  if (!stock.isNull()) {
    Firmware f;
    readEntry(stock, f);
    f.name = "M-VAVE stock";
    if (f.product.size() > 5 && f.product.compare(0, 5, "FM-1_") == 0) {
      f.name += " V" + std::to_string(atoi(f.product.c_str() + 5));  // "M-VAVE stock V15"
    }
    f.author = "M-VAVE";
    f.what = "The firmware the FM-1 came with: put it back to how it was.";
    f.kinds = {"stock"};
    f.stock = true;
    if (f.pkg.compare(0, 8, "https://") == 0 && f.size && f.sha256.size() == 64) {
      out.push_back(f);
    }
  }
  readFirmwares(doc["firmwares"], out);
  return true;
}

std::string catalog::product(const char* p) {
  std::vector<uint8_t> head(64 * 0x30);  // the most interleave blocks a package has (fm1.cpp)
  FILE* f = fopen(p, "rb");
  if (!f) {
    return "";
  }
  uint32_t n = fread(head.data(), 1, head.size(), f);
  fclose(f);
  char s[24];
  uint32_t blocks;
  return pkgHead(head.data(), n, s, sizeof s, blocks) ? s : "";
}

// M-VAVE's own releases, by their sha256: known on the SD card whatever they're called, and whatever the Hub lists
static const struct {
  const char* sha256;
  const char* name;
  bool open;  // M-VAVE Open (else the stock firmware)
} OFFICIAL[] = {
    {"db1642b2b6fa5c2cccb11ffd13878068bb28601678d3644049f99dc40e7edb8a", "M-VAVE stock V15", false},
    {"d017fb65e70aca97ee29d944470c3515a74581df5c0576bf70de917c7b2b61d5", "M-VAVE stock V16", false},
    {"7e7b6d260163cdf9d716f7fab44f19be600edeffcd390de74370b83c917fd0c6", "M-VAVE Open V14", true},
};

// each file's sha256, kept on the card by its name, size and time (so each is read once): "name size mtime sha256"
#define SHA_CACHE DATA_DIR "/sd-sha256.txt"

// the ones read since power-on, too (so a card that can't be written isn't read again and again)
static std::mutex knownMu;
static std::vector<std::string> known;

static void readCache(std::vector<std::string>& cache) {
  if (FILE* c = fopen(catalog::path(SHA_CACHE).c_str(), "r")) {
    char line[400];
    while (fgets(line, sizeof line, c)) {
      line[strcspn(line, "\r\n")] = 0;
      cache.push_back(line);
    }
    fclose(c);
  }
  std::lock_guard<std::mutex> l(knownMu);
  cache.insert(cache.end(), known.begin(), known.end());
}

static std::string cacheKey(const char* name, const struct stat& st) {
  char key[300];
  snprintf(key, sizeof key, "%s %lu %lu ", name, (unsigned long)st.st_size, (unsigned long)st.st_mtime);
  return key;
}

// the cached sha256 of a file, "" if it isn't known yet
static std::string cachedSha(const std::string& key, const std::vector<std::string>& cache) {
  for (auto& line : cache) {
    if (line.size() == key.size() + 64 && line.compare(0, key.size(), key) == 0) {
      return line.substr(key.size());
    }
  }
  return "";
}

static bool writeCache(const std::vector<std::string>& lines) {
  mkdir(catalog::path(DATA_DIR).c_str(), 0777);
  std::string p = catalog::path(SHA_CACHE);
  FILE* c = fopen(p.c_str(), "w");
  if (!c) {
    hal::log("sd: can't write %s (errno %d)", SHA_CACHE, errno);
    return false;
  }
  for (auto& line : lines) {
    fprintf(c, "%s\n", line.c_str());
  }
  bool ok = !ferror(c);
  if (fclose(c) || !ok) {
    hal::log("sd: writing %s failed (errno %d)", SHA_CACHE, errno);
    return false;
  }
  return true;
}

static bool isPkg(const char* name) {
  size_t n = strlen(name);
  return name[0] != '.' && n > 5 && !strcasecmp(name + n - 5, ".fwsc");
}

static std::atomic<bool> hashing{false}, hashedNew{false};

int catalog::scanSdInto(std::vector<Firmware>& hub, std::vector<Firmware>& sd) {
  sd.clear();
  for (Firmware& h : hub) {
    h.onSd = false;
  }
  std::string dir = path(FW_DIR);
  DIR* d = dir.empty() ? 0 : opendir(dir.c_str());
  if (!d) {
    return 0;
  }
  std::vector<std::string> cache;
  readCache(cache);
  int unknown = 0;
  while (dirent* e = readdir(d)) {
    if (!isPkg(e->d_name)) {
      continue;
    }
    std::string full = dir + "/" + e->d_name;
    struct stat st;
    if (stat(full.c_str(), &st)) {
      continue;
    }
    // what it is: by its sha256 (a Hub firmware, or one of M-VAVE's), not by its name. Until that's known (it's
    // read in the background: hashSd), a Hub firmware by its file name and size (the install checks the sha256)
    std::string sha = cachedSha(cacheKey(e->d_name, st), cache);
    if (sha.empty()) {
      unknown++;
    }
    Firmware f;
    auto h = std::find_if(hub.begin(), hub.end(), [&](const Firmware& x) {
      return sha.empty() ? x.size == (uint32_t)st.st_size && !strcasecmp(x.file.c_str(), e->d_name)
                         : !strcasecmp(x.sha256.c_str(), sha.c_str());
    });
    auto o = std::find_if(std::begin(OFFICIAL), std::end(OFFICIAL), [&](const auto& x) { return sha == x.sha256; });
    if (h != hub.end()) {
      h->onSd = true;
      f = *h;
      f.hub = true;
    } else if (o != std::end(OFFICIAL)) {
      f.name = o->name;
      f.author = "M-VAVE";
      f.what = o->open ? "M-VAVE's open firmware: a groovebox, with a web editor (open.m-vave.com)."
                       : "The firmware the FM-1 came with: put it back to how it was.";
      f.kinds = {o->open ? "groove" : "stock"};
      f.stock = !o->open;
      f.sha256 = sha;  // (the install checks it)
    } else {
      f.name = e->d_name;
      f.name.resize(f.name.size() - 5);
    }
    f.onSd = true;
    f.file = e->d_name;
    f.size = st.st_size;
    f.product = product(full.c_str());
    sd.push_back(f);
  }
  closedir(d);
  std::sort(sd.begin(), sd.end(), [](const Firmware& a, const Firmware& b) { return strcasecmp(a.name.c_str(), b.name.c_str()) < 0; });
  return unknown;
}

// the sha256 of the files the cache doesn't know yet (seconds each), on a thread of their own; the cache is written as
// it goes (so a restart meanwhile keeps what's done), and keeps only the files that are there
static void hashTask(void*) {
  uint32_t t = hal::millis();
  std::string dir = catalog::path(FW_DIR);
  std::vector<std::string> cache, kept;
  readCache(cache);
  int done = 0;
  bool ok = true;
  if (DIR* d = dir.empty() ? 0 : opendir(dir.c_str())) {
    while (dirent* e = readdir(d)) {
      if (!isPkg(e->d_name)) {
        continue;
      }
      std::string full = dir + "/" + e->d_name;
      struct stat st;
      if (stat(full.c_str(), &st)) {
        continue;
      }
      std::string key = cacheKey(e->d_name, st), sha = cachedSha(key, cache);
      if (sha.empty()) {
        char hex[65];
        if (!hal::sha256(full.c_str(), hex)) {
          continue;
        }
        sha = hex;
        {
          std::lock_guard<std::mutex> l(knownMu);
          known.push_back(key + sha);
        }
        if (++done % 20 == 0 && ok) {
          std::vector<std::string> sofar = kept;  // (the ones still to go keep their old lines)
          sofar.push_back(key + sha);
          sofar.insert(sofar.end(), cache.begin(), cache.end());
          ok = writeCache(sofar);
        }
      }
      kept.push_back(key + sha);
    }
    closedir(d);
  }
  if (ok && (done || kept.size() != cache.size())) {
    ok = writeCache(kept);
  }
  hal::log("sd: %d sha256 read (%u ms), %u in the cache%s", done, (unsigned)(hal::millis() - t), (unsigned)kept.size(),
           ok ? "" : ": not written");
  hashedNew = hashedNew || done;
  hashing = false;
}

void catalog::hashSd() {
  if (!path(FW_DIR).empty() && !hashing.exchange(true)) {
    hal::spawn(hashTask, 0, 8192);
  }
}

bool catalog::sdHashed() {
  return hashedNew.exchange(false);
}

void catalog::scanSd() {
  if (scanSdInto(hub, sd)) {
    hashSd();
  }
}
