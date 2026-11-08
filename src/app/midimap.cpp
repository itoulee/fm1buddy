#include "midimap.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* TYPE_NAMES[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, "note", 0, "cc", "pc", "at", "pb", 0};

static int typeOf(const char* w) {
  for (int k = 0; k < 16; k++) {
    if (TYPE_NAMES[k] && !strcmp(w, TYPE_NAMES[k])) {
      return k;
    }
  }
  return 0;
}

static bool space(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// words, lowercase; '...' and "..." keep their spaces
static int words(const char* s, char w[][24], int max) {
  int n = 0;
  while (*s && n < max) {
    while (space(*s)) {
      s++;
    }
    if (!*s) {
      break;
    }
    char q = *s == '"' || *s == '\'' ? *s++ : 0;
    int k = 0;
    while (*s && (q ? *s != q : !space(*s))) {
      if (k < 23) {
        w[n][k++] = (char)tolower((unsigned char)*s);
      }
      s++;
    }
    if (q && *s) {
      s++;
    }
    w[n++][k] = 0;
  }
  return n;
}

static bool number(const char* w, int max, uint8_t& v) {
  if (!*w) {
    return false;
  }
  for (const char* c = w; *c; c++) {
    if (!isdigit((unsigned char)*c)) {
      return false;
    }
  }
  int n = atoi(w);
  if (n > max) {
    return false;
  }
  v = (uint8_t)n;
  return true;
}

// [from|to NAME] [ch N] [TYPE [NUM]]: the words used, -1 on an error
static int side(char w[][24], int n, bool out, MapRule& r, const char** why) {
  int i = 0;
  if (i < n && !strcmp(w[i], out ? "to" : "from")) {
    if (i + 1 >= n) {
      *why = "a name after from/to";
      return -1;
    }
    snprintf(out ? r.to : r.from, sizeof r.from, "%s", w[i + 1]);
    i += 2;
  }
  if (i < n && !strcmp(w[i], "ch")) {
    uint8_t ch;
    if (i + 1 >= n || !number(w[i + 1], 16, ch) || !ch) {
      *why = "ch is 1-16";
      return -1;
    }
    (out ? r.outCh : r.inCh) = ch;
    i += 2;
  }
  if (i < n && typeOf(w[i])) {
    (out ? r.outType : r.inType) = (uint8_t)typeOf(w[i]);
    i++;
    if (i < n && isdigit((unsigned char)w[i][0])) {
      uint8_t v, hi;
      char* dash = out ? 0 : strchr(w[i], '-');  // a range, on the left
      if (dash) {
        *dash = 0;
      }
      if (!number(w[i], 127, v) || (dash && !number(dash + 1, 127, hi))) {
        *why = "numbers are 0-127";
        return -1;
      }
      if (dash && hi < v) {
        *why = "a range goes low-high";
        return -1;
      }
      if (out) {
        r.outNum = v;
      } else {
        r.inNum = v;
        r.inHi = dash ? hi : v;
      }
      i++;
    }
  }
  // on the right: +N / -N, vel N, vel N%
  while (out && i < n) {
    if ((w[i][0] == '+' || w[i][0] == '-') && w[i][1]) {
      uint8_t v;
      if (!number(w[i] + 1, 127, v)) {
        *why = "+N or -N moves notes N semitones";
        return -1;
      }
      r.transpose = (int8_t)(w[i][0] == '-' ? -v : v);
      i++;
    } else if (!strcmp(w[i], "vel")) {
      char num[24];
      snprintf(num, sizeof num, "%s", i + 1 < n ? w[i + 1] : "");
      size_t k = strlen(num);
      bool pct = k && num[k - 1] == '%';
      if (pct) {
        num[k - 1] = 0;
      }
      int v = 0;
      for (const char* c = num; *c; c++) {
        v = isdigit((unsigned char)*c) && v < 1000 ? v * 10 + (*c - '0') : -1000;
      }
      if (!num[0] || v < 1 || v > (pct ? 400 : 127)) {
        *why = "vel is 1-127, or 1-400%";
        return -1;
      }
      if (pct) {
        r.velPct = (uint16_t)v;
      } else {
        r.vel = (uint8_t)v;
      }
      i += 2;
    } else {
      break;
    }
  }
  return i;
}

bool midimap::parse(const char* line, MapRule& r, const char** why) {
  char buf[160], w[16][24];
  *why = 0;
  snprintf(buf, sizeof buf, "%s", line);
  if (char* hash = strchr(buf, '#')) {
    *hash = 0;
  }
  r = MapRule();
  char* arrow = strstr(buf, "->");
  if (!arrow) {
    *why = words(buf, w, 16) ? "a rule needs ->" : 0;
    return false;
  }
  *arrow = 0;
  int n = words(buf, w, 16);
  int used = side(w, n, false, r, why);
  if (used < 0) {
    return false;
  }
  if (used < n) {
    *why = "something left over before ->";
    return false;
  }
  n = words(arrow + 2, w, 16);
  if (n == 1 && !strcmp(w[0], "drop")) {
    r.outType = MAP_DROP;
    return true;
  }
  used = side(w, n, true, r, why);
  if (used < 0) {
    return false;
  }
  if (used < n) {
    *why = "something left over after ->";
    return false;
  }
  return true;
}

bool midimap::load(const char* path, std::vector<MapRule>& out, std::string& problem) {
  out.clear();
  problem.clear();
  FILE* f = fopen(path, "r");
  if (!f) {
    return false;
  }
  char line[160];
  for (int ln = 1; fgets(line, sizeof line, f); ln++) {
    if (!strchr(line, '\n') && !feof(f)) {  // longer than a rule can be: the rest of it skipped, not a line of its own
      for (int c; (c = fgetc(f)) != EOF && c != '\n';) {
      }
      if (problem.empty()) {
        problem = "line " + std::to_string(ln) + ": too long";
      }
      continue;
    }
    MapRule r;
    const char* why;
    if (parse(line, r, &why)) {
      if (out.size() < MAP_RULES) {
        out.push_back(r);
      } else if (problem.empty()) {
        problem = "line " + std::to_string(ln) + ": more than " + std::to_string(MAP_RULES) + " rules";
      }
    } else if (why && problem.empty()) {
      problem = "line " + std::to_string(ln) + ": " + why;
    }
  }
  fclose(f);
  return true;
}

// the top of the file, so it explains itself
static const char HELP[] =
    "# fm1buddy MIDI mapping, for MIDI routing: one rule per line. Without rules, everything any device plays goes\n"
    "# to all the others.\n"
    "#\n"
    "#   [from NAME] [ch N] [TYPE [NUM[-NUM]]] -> [to NAME] [ch N] [TYPE [NUM]] [+N|-N] [vel N|vel N%]\n"
    "#   [from NAME] [ch N] [TYPE [NUM[-NUM]]] -> drop\n"
    "#\n"
    "# TYPE is note, cc, pc (program change), at (channel pressure) or pb (pitch bend). NAME is part of a device's\n"
    "# name, as the MIDI page shows it (any case; \"quote it\" if it has spaces). On the left, NUM-NUM is a range\n"
    "# (note 0-59: the keys below middle C, to split a keyboard). On the right, what you leave out stays as it came\n"
    "# in; without \"to\", it goes to all the other devices. +N or -N moves notes N semitones; vel N plays every note\n"
    "# at velocity N, vel N% scales how hard they're played. The first rule that matches a message decides; a\n"
    "# message no rule matches goes on as it is. Clock, start/stop and SysEx always go on as they are.\n"
    "# Two of the same device on the hub get their hub port's letter after the name (port 1 is A): \"fm-1 a\" and\n"
    "# \"fm-1 b\" each reach just one, and \"fm-1\" reaches both. Keep each on its port and it keeps its letter.\n"
    "# For example, two controllers playing the FM-1 on channels of their own:\n"
    "#\n"
    "#   from nanokey -> to fm-1 ch 1\n"
    "#   from \"launchkey mini\" -> to fm-1 ch 2\n"
    "#   from nanokontrol2 ch 1 cc 10 -> to fm-1 ch 2 cc 5      CC 10 becomes CC 5 on channel 2\n"
    "#   from launchkey note 36 -> to fm-1 note 38\n"
    "#   from keystep note 0-59 -> to fm-1 ch 1 +12            a split: the low keys an octave up on channel 1,\n"
    "#   from keystep note 60-127 -> to fm-1 ch 2 vel 100       the high keys on channel 2, all at velocity 100\n"
    "#   cc 1 -> pc                                            the mod wheel picks programs\n"
    "#   pb -> drop                                            no pitch bend from anything\n"
    "#\n"
    "# The rules are read when fm1buddy starts, when MIDI routing is turned on, and with the reload button on the\n"
    "# MIDI page. The MIDI page's Mapping can also add and delete rules here, by touch.\n";

bool midimap::damaged(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) {
    return false;
  }
  bool zero = false;
  for (int c; !zero && (c = fgetc(f)) != EOF;) {
    zero = c == 0;
  }
  fclose(f);
  return zero;
}

bool midimap::writeHelp(const char* path) {
  FILE* f = fopen(path, "w");
  if (!f) {
    return false;
  }
  // from RAM: HELP is in flash, and the SD card's DMA reads a write that big straight from where it is (on the board
  // its whole sectors came out as zeros)
  std::string text(HELP);
  bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
  return !fclose(f) && ok;
}

// a name as the file has it: "quoted" if it has spaces
static std::string word(const char* s) {
  return strchr(s, ' ') ? std::string("\"") + s + "\"" : std::string(s);
}

std::string midimap::text(const MapRule& r) {
  std::string s;
  auto num = [&](uint8_t type, uint8_t n, uint8_t hi) {
    if (!type) {
      return;
    }
    s += std::string(" ") + TYPE_NAMES[type];
    if (n != MAP_ANY) {
      s += " " + std::to_string(n);
      if (hi != MAP_ANY && hi != n) {
        s += "-" + std::to_string(hi);
      }
    }
  };
  if (r.from[0]) {
    s += " from " + word(r.from);
  }
  if (r.inCh) {
    s += " ch " + std::to_string(r.inCh);
  }
  num(r.inType, r.inNum, r.inHi);
  s += " ->";
  if (r.outType == MAP_DROP) {
    return s.substr(s[0] == ' ') + " drop";
  }
  if (r.to[0]) {
    s += " to " + word(r.to);
  }
  if (r.outCh) {
    s += " ch " + std::to_string(r.outCh);
  }
  num(r.outType, r.outNum, MAP_ANY);
  if (r.transpose) {
    s += (r.transpose > 0 ? " +" : " -") + std::to_string(abs(r.transpose));
  }
  if (r.vel) {
    s += " vel " + std::to_string(r.vel);
  } else if (r.velPct) {
    s += " vel " + std::to_string(r.velPct) + "%";
  }
  return s.substr(s[0] == ' ');
}

std::string midimap::describe(const MapRule& r) {
  static const char* PLURAL[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, "notes", 0, "CCs", "programs", "pressure", "pitch bend", 0};
  static const char* ONE[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, "note", 0, "CC", "program", "pressure", "pitch bend", 0};
  auto what = [](uint8_t type, uint8_t n, uint8_t hi) {
    if (!type) {
      return std::string();
    }
    if (n == MAP_ANY) {
      return std::string(PLURAL[type]);
    }
    if (hi != MAP_ANY && hi != n) {
      return std::string(PLURAL[type]) + " " + std::to_string(n) + "-" + std::to_string(hi);
    }
    return std::string(ONE[type]) + " " + std::to_string(n);
  };
  auto join = [](std::string a, const std::string& b) { return a.empty() ? b : b.empty() ? a
                                                                                         : a + ", " + b; };
  std::string in = r.from[0] ? r.from : "anything";
  if (r.inCh) {
    in += " ch " + std::to_string(r.inCh);
  }
  in = join(in, what(r.inType, r.inNum, r.inHi));
  if (r.outType == MAP_DROP) {
    return in + " -> dropped";
  }
  std::string out = r.to[0] ? r.to : "the others";
  if (r.outCh) {
    out += " ch " + std::to_string(r.outCh);
  }
  out = join(out, what(r.outType, r.outNum, MAP_ANY));
  if (r.transpose) {
    out = join(out, (r.transpose > 0 ? "+" : "-") + std::to_string(abs(r.transpose)));
  }
  if (r.vel) {
    out = join(out, "vel " + std::to_string(r.vel));
  } else if (r.velPct) {
    out = join(out, "vel " + std::to_string(r.velPct) + "%");
  }
  return in + " -> " + out;
}

bool midimap::matches(const char* part, const char* name) {
  if (!*part) {
    return true;
  }
  char lower[64];
  int k = 0;
  for (; name[k] && k < (int)sizeof lower - 1; k++) {
    lower[k] = (char)tolower((unsigned char)name[k]);
  }
  lower[k] = 0;
  return strstr(lower, part);
}

static uint32_t lenOf(uint8_t status) {
  uint8_t k = status >> 4;
  return k == 0xC || k == 0xD ? 2 : 3;
}

int midimap::apply(const std::vector<MapRule>& rules, const uint32_t* from, const uint32_t* to, int d,
                   const uint8_t* msg, uint8_t* out, uint32_t* outN, uint32_t* toMask) {
  uint8_t kind = msg[0] >> 4, ch = (msg[0] & 15) + 1;
  uint8_t d1 = lenOf(msg[0]) > 1 ? msg[1] : 0, d2 = lenOf(msg[0]) > 2 ? msg[2] : 0;
  bool hasNum = kind == 0x8 || kind == 0x9 || kind == 0xA || kind == 0xB;
  for (size_t i = 0; i < rules.size(); i++) {
    const MapRule& r = rules[i];
    if (!(from[i] >> d & 1)) {
      continue;
    }
    if (r.inCh && r.inCh != ch) {
      continue;
    }
    if (r.inType) {
      if (r.inType == MAP_NOTE ? kind != 0x8 && kind != 0x9 : r.inType != kind) {
        continue;
      }
      // the number: a note's, a CC's, or the program
      if (r.inNum != MAP_ANY && (hasNum || kind == 0xC) && (d1 < r.inNum || d1 > r.inHi)) {
        continue;
      }
    }
    *toMask = to[i];
    // dropped, or "to" a device that isn't there: nowhere, not everywhere
    if (r.outType == MAP_DROP || (r.to[0] && !to[i])) {
      *outN = 0;
      return (int)i;
    }
    // the message's value, and what it becomes
    uint8_t val = hasNum ? d2 : kind == 0xE ? d2
                                            : d1;
    if (kind == 0x8) {
      val = 0;
    }
    uint8_t okind = r.outType ? r.outType : kind;
    uint8_t c = (uint8_t)((r.outCh ? r.outCh : ch) - 1);
    uint8_t num = r.outNum != MAP_ANY ? r.outNum : hasNum ? d1
                                                          : 0;
    if (okind == 0x8 || okind == 0x9) {
      if (r.outNum == MAP_ANY && !hasNum) {
        num = 60;
      }
      int moved = num + r.transpose;
      if (moved < 0 || moved > 127) {  // moved off the keyboard: nothing
        *outN = 0;
        return (int)i;
      }
      num = (uint8_t)moved;
      // a note off stays one (with its own velocity); anything else is on at its value, off at 0
      if (kind == 0x8 && !r.outType) {
        out[0] = (uint8_t)(0x80 | c), out[1] = num, out[2] = d2;
      } else {
        if (val && r.vel) {
          val = r.vel;
        } else if (val && r.velPct) {
          uint32_t v = (uint32_t)val * r.velPct / 100;
          val = (uint8_t)(v < 1 ? 1 : v > 127 ? 127
                                              : v);
        }
        out[0] = (uint8_t)((val ? 0x90 : 0x80) | c), out[1] = num, out[2] = val;
      }
      *outN = 3;
    } else if (okind == MAP_POLY || okind == MAP_CC) {
      out[0] = (uint8_t)(okind << 4 | c), out[1] = num, out[2] = val;
      *outN = 3;
    } else if (okind == MAP_PC) {
      out[0] = (uint8_t)(0xC0 | c), out[1] = r.outNum != MAP_ANY ? r.outNum : val;
      *outN = 2;
    } else if (okind == MAP_AT) {
      out[0] = (uint8_t)(0xD0 | c), out[1] = val;
      *outN = 2;
    } else {  // MAP_PB
      out[0] = (uint8_t)(0xE0 | c), out[1] = kind == 0xE ? d1 : 0, out[2] = val;
      *outN = 3;
    }
    return (int)i;
  }
  return -1;
}
