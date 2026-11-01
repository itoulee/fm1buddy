#include "catalog.h"

#include <ArduinoJson.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include <algorithm>

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
  f.stars = e["stars"] | 0;
  f.installs = e["installs"] | 0;
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

bool catalog::parseHub(std::vector<Firmware>& out, std::string& updated, std::string& err) {
  hal::psramFirst(true);
  bool ok = parseHubIn(out, updated, err);
  hal::psramFirst(false);
  return ok;
}

static void readFirmwares(JsonArrayConst list, std::vector<Firmware>& out) {
  for (JsonObjectConst e : list) {
    Firmware f;
    readEntry(e, f);
    if (!f.pkg.empty() && f.size && f.sha256.size() == 64 && f.pkg.find("..") == std::string::npos &&
        !f.file.empty() && f.file.find("..") == std::string::npos) {
      out.push_back(f);
    }
  }
}

// the SMK-37's list (SMK_LIST), after the Hub's: its packages, if it's on the SD card
static void readSmk(const JsonDocument& filter, std::vector<Firmware>& out) {
  std::string text;
  if (!readAll(catalog::path(SMK_CACHE), text)) {
    return;
  }
  JsonDocument doc;
  DeserializationError bad = deserializeJson(doc, text, DeserializationOption::Filter(filter));
  text = std::string();
  if (bad) {
    hal::log("catalog: the SMK-37's list didn't read (%s)", bad.c_str());
    return;
  }
  readFirmwares(doc["firmwares"], out);
}

static bool parseHubIn(std::vector<Firmware>& out, std::string& updated, std::string& err) {
  std::string text;
  if (!readAll(catalog::path(HUB_CACHE), text)) {
    err = "The Hub's list isn't on the SD card yet.";
    return false;
  }
  JsonDocument filter;
  filter["updated"] = true;
  for (const char* k : {"name", "author", "version", "date", "what", "pkg", "file", "product", "sha256", "size",
                        "stars", "installs", "beta", "stock", "hid", "kind"}) {
    filter["firmwares"][0][k] = true;
  }
  for (const char* k : {"pkg", "product", "sha256", "size"}) {
    filter["stock"][k] = true;
  }
  JsonDocument doc;
  DeserializationError bad = deserializeJson(doc, text, DeserializationOption::Filter(filter));
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
    out.push_back(f);
  }
  readFirmwares(doc["firmwares"], out);
  doc.clear();
  readSmk(filter, out);
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

static std::string cachedSha(const std::string& full, const char* name, const struct stat& st,
                             std::vector<std::string>& cache, bool& changed) {
  char key[300];
  snprintf(key, sizeof key, "%s %lu %lu ", name, (unsigned long)st.st_size, (unsigned long)st.st_mtime);
  for (auto& line : cache) {
    if (line.size() == strlen(key) + 64 && line.compare(0, strlen(key), key) == 0) {
      return line.substr(strlen(key));
    }
  }
  char hex[65];
  if (!hal::sha256(full.c_str(), hex)) {
    return "";
  }
  cache.push_back(std::string(key) + hex);
  changed = true;
  return hex;
}

static bool isPkg(const char* name) {
  size_t n = strlen(name);
  return name[0] != '.' && n > 5 && !strcasecmp(name + n - 5, ".fwsc");
}

void catalog::scanSd() {
  sd.clear();
  for (Firmware& h : hub) {
    h.onSd = false;
  }
  std::string dir = path(FW_DIR);
  DIR* d = dir.empty() ? 0 : opendir(dir.c_str());
  if (!d) {
    return;
  }
  std::vector<std::string> cache, kept;
  if (FILE* c = fopen(path(SHA_CACHE).c_str(), "r")) {
    char line[400];
    while (fgets(line, sizeof line, c)) {
      line[strcspn(line, "\r\n")] = 0;
      cache.push_back(line);
    }
    fclose(c);
  }
  bool changed = false;
  while (dirent* e = readdir(d)) {
    if (!isPkg(e->d_name)) {
      continue;
    }
    std::string full = dir + "/" + e->d_name;
    struct stat st;
    if (stat(full.c_str(), &st)) {
      continue;
    }
    // what it is: by its sha256 (a Hub firmware, or one of M-VAVE's), not by its name
    std::string sha = cachedSha(full, e->d_name, st, cache, changed);
    kept.push_back(std::string(e->d_name) + " " + std::to_string((unsigned long)st.st_size) + " " +
                   std::to_string((unsigned long)st.st_mtime) + " " + sha);
    Firmware f;
    auto h = std::find_if(hub.begin(), hub.end(), [&](const Firmware& x) { return !sha.empty() && !strcasecmp(x.sha256.c_str(), sha.c_str()); });
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
  if (changed || kept.size() != cache.size()) {  // (only the files that are there now)
    if (FILE* c = fopen(path(SHA_CACHE).c_str(), "w")) {
      for (auto& line : kept) {
        fprintf(c, "%s\n", line.c_str());
      }
      fclose(c);
    }
  }
  std::sort(sd.begin(), sd.end(), [](const Firmware& a, const Firmware& b) { return strcasecmp(a.name.c_str(), b.name.c_str()) < 0; });
}
