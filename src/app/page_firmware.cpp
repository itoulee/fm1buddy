// Firmware: the .fwsc files on the SD card, and the Firmware Hub's list. Tap one for what it is, to save it to the
// SD card, or (soon) to install it on the FM-1.
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <algorithm>

#include "catalog.h"
#include "fm1.h"
#include "hal.h"
#include "jobs.h"
#include "midi.h"
#include "ui_priv.h"

using namespace ui;

enum Source { SD,
              HUB };
enum Sort { NEWEST,
            NAME };

static Source source = SD;
static bool sourceChosen, hubAsked;
static int sortBy = NEWEST;
static std::string kindFilter;  // "": all
// the machines a firmware can be for (an SMK-37 package says which model), and which the lists show: the Hub's, the
// one on the hub until another is picked (-1); the SD card's, all (-1) until one is picked
enum Machine { FM1,
               SMK_PRO,
               SMK_ELITE,
               MKE_P37,
               MACHINES };
static const char* MACHINE_NAMES[MACHINES] = {"FM-1", "SMK Pro", "SMK Elite", "MKE-P37"};
static int hubMachine = -1, sdMachine = -1;
static Firmware sel;  // the one the details show
static lv_obj_t *list, *status, *refresh, *spin;
static lv_obj_t *actions, *progress, *progressText;
static std::string shownFm1;  // the FM-1 the Install button was made for
static std::string listFm1;   // ... and the one the list was made for

static void detailPage(lv_obj_t* page);

// the color of a kind
static uint32_t kindColor(const std::string& k) {
  const Theme& t = theme();
  if (k == "synth")
    return t.knob[3];
  if (k == "groove")
    return t.knob[1];
  if (k == "drums")
    return t.knob[0];
  if (k == "chords")
    return t.knob[2];
  if (k == "stock")
    return t.accent;
  return t.dim;
}

static std::string kb(uint32_t size) {
  char s[16];
  snprintf(s, sizeof s, "%u KB", (unsigned)((size + 1023) / 1024));
  return s;
}

// ------------------------------------------------------------------------------------------- the list ---
// "FM-1_015", "ota-FM-1_900", or ""
static std::string fm1Now() {
  const Fm1Id* id = fm1::known(fm1::current());
  return id ? id->text : "";
}

// what a package (or a device: "ota-SMK-37 Elite_900") is for; -1: not a package
static int machineOf(const std::string& product) {
  if (product.empty()) {
    return -1;
  }
  if (!isSmk(product.c_str())) {
    return FM1;
  }
  std::string m = product.compare(0, 4, "ota-") ? product : product.substr(4);
  return !m.compare(0, 13, "SMK-37 Elite_") ? SMK_ELITE : !m.compare(0, 8, "MKE-P37_") ? MKE_P37
                                                                                       : SMK_PRO;  // (the
  // Starrykey 37 Play takes the Pro's)
}

// the Hub's list: the machine picked, or the one on the hub (the FM-1 without one)
static int hubShows() {
  if (hubMachine >= 0) {
    return hubMachine;
  }
  const Fm1Id* id = fm1::known(fm1::current());
  int m = id ? machineOf(id->text) : -1;
  return m < 0 ? FM1 : m;
}

static bool forDevice(const Firmware& f) {
  return machineOf(f.product) == hubShows();
}

static std::vector<const Firmware*> shown() {
  std::vector<const Firmware*> v;
  for (const Firmware& f : source == SD ? catalog::sd : catalog::hub) {
    if (source == HUB ? !forDevice(f) : sdMachine >= 0 && machineOf(f.product) != sdMachine) {
      continue;
    }
    if (source == SD || kindFilter.empty() || std::find(f.kinds.begin(), f.kinds.end(), kindFilter) != f.kinds.end()) {
      v.push_back(&f);
    }
  }
  if (source == HUB) {
    std::stable_sort(v.begin(), v.end(), [](const Firmware* a, const Firmware* b) {
      if (a->stock != b->stock)
        return a->stock;  // M-VAVE's own first
      if (sortBy == NEWEST && a->date != b->date)
        return a->date > b->date;
      return strcasecmp(a->name.c_str(), b->name.c_str()) < 0;
    });
  }
  return v;
}

static void open(lv_event_t* e) {
  sel = *(const Firmware*)lv_event_get_user_data(e);
  go(detailPage);
}

static void wifiSettings(lv_event_t*) {
  go(SETTINGS, wifiPage);
}

// the list is made a few rows at a time (240 of them at once held the page for a second), while the spinner turns
#define FILL_CHUNK 6
static std::vector<const Firmware*> filling;  // the rows still to make
static size_t filled;
static lv_timer_t* fillTimer;

static void fillRow(const Firmware* f) {
  const Theme& t = theme();
  lv_obj_t* r = listRow(list, open, (void*)f);
  lv_obj_t* left = box(r);
  lv_obj_set_flex_grow(left, 1);
  lv_obj_t* n = line(left, f->name.c_str(), &lv_font_montserrat_16, t.text);
  lv_obj_set_width(n, LV_PCT(100));
  std::string sub = f->author;
  if (!f->version.empty()) {
    sub += (sub.empty() ? "v" : "  v") + f->version;
  }
  if (sub.empty()) {
    sub = f->product.empty() ? "not a firmware package" : f->product;
  }
  lv_obj_t* s = line(left, sub.c_str(), &lv_font_montserrat_12, t.dim);
  lv_obj_set_width(s, LV_PCT(100));

  lv_obj_t* right = box(r);
  lv_obj_set_width(right, LV_SIZE_CONTENT);
  lv_obj_set_flex_align(right, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
  if (!f->kinds.empty()) {
    label(right, f->kinds[0].c_str(), &lv_font_montserrat_12, kindColor(f->kinds[0]));
  }
  lv_obj_t* tags = box(right, LV_FLEX_FLOW_ROW);
  lv_obj_set_width(tags, LV_SIZE_CONTENT);
  lv_obj_set_style_pad_column(tags, 4, 0);
  if (f->beta) {
    label(tags, "beta", &lv_font_montserrat_12, t.knob[1]);
  } else if (source == HUB && f->onSd) {
    label(tags, LV_SYMBOL_SD_CARD, &lv_font_montserrat_12, t.accent);
  }
  int m = machineOf(f->product);
  if (m >= 0) {
    label(tags, MACHINE_NAMES[m], &lv_font_montserrat_12, m == FM1 ? t.dim : t.knob[3]);
  }
}

static void stopFill() {
  if (fillTimer) {
    lv_timer_delete(fillTimer);
    fillTimer = 0;
  }
  filling.clear();
  filled = 0;
}

static void fillMore(lv_timer_t*) {
  size_t end = filled + FILL_CHUNK < filling.size() ? filled + FILL_CHUNK : filling.size();
  for (; filled < end; filled++) {
    fillRow(filling[filled]);
  }
  if (filled < filling.size()) {
    return;
  }
  stopFill();
}

static void fillList() {
  const Theme& t = theme();
  stopFill();
  lv_obj_clean(list);
  filling = shown();
  filled = 0;
  if (filling.empty()) {
    lv_obj_t* l = label(list, "", &lv_font_montserrat_14, t.dim);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_pad_all(l, 12, 0);
    if (source == SD && hal::sd() && !catalog::sd.empty()) {
      lv_label_set_text(l, "None for that machine on the SD card.");
    } else if (source == SD) {
      lv_label_set_text(l, hal::sd() ? "No firmwares in /fm1 on the SD card. Save some from the Hub, or copy .fwsc files there."
                                     : "No SD card.");
    } else if (!settings.ssid[0]) {
      lv_label_set_text(l, "Pick a Wi-Fi network in Settings to get the Firmware Hub's list.");
      lv_obj_t* b = button(list, LV_SYMBOL_WIFI "  Wi-Fi settings", wifiSettings);
      lv_obj_set_style_margin_left(b, 12, 0);
    } else if (!jobs::running) {
      lv_label_set_text(l, !kindFilter.empty()    ? "None of that kind."
                           : catalog::hub.empty() ? "Nothing yet: tap " LV_SYMBOL_REFRESH " to get the list."
                                                  : "None for that machine yet.");
    }
    return;
  }
  fillMore(0);  // (the first rows now: the page looks alive)
  if (!filling.empty()) {
    fillTimer = lv_timer_create(fillMore, 16, 0);
  }
}

static void showStatus() {
  if (!status) {
    return;
  }
  if (!jobs::running && fm1Now() != listFm1) {
    hubMachine = -1;
    go(firmwarePage);  // an FM-1 or an SMK-37 came or went: the Hub's list is for it
    return;
  }
  const Theme& t = theme();
  bool busy = jobs::running && jobs::kind == jobs::HUB;
  if (refresh) {
    lv_obj_add_flag(busy ? refresh : spin, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(busy ? spin : refresh, LV_OBJ_FLAG_HIDDEN);
  }
  std::string s;
  if (busy) {
    s = jobs::status();
  } else if (source == SD) {
    s = hal::sd() ? std::to_string(catalog::sd.size()) + " in /fm1 on the SD card" : "No SD card";
  } else if (!catalog::hubUpdated.empty()) {
    s = "List from " + catalog::hubUpdated;
  }
  lv_label_set_text(status, s.c_str());
  lv_obj_set_style_text_color(status, color(t.dim), 0);
}

static void hubDone() {
  if (jobs::kind == jobs::HUB) {
    if (!jobs::ok) {
      toast(devText(jobs::error).c_str(), true);
    }
    go(firmwarePage);
  }
}

static void startHub(lv_event_t*) {
  if (jobs::hubList()) {
    showStatus();
    fillList();
  }
}

static void pickSource(lv_event_t* e) {
  source = (Source)(intptr_t)lv_event_get_user_data(e);
  sourceChosen = true;
  go(firmwarePage);
}

static void pickKind(lv_event_t* e) {
  lv_obj_t* dd = (lv_obj_t*)lv_event_get_target(e);
  char s[24];
  lv_dropdown_get_selected_str(dd, s, sizeof s);
  kindFilter = lv_dropdown_get_selected(dd) ? s : "";
  fillList();
}

static void pickMachine(lv_event_t* e) {
  int i = lv_dropdown_get_selected((lv_obj_t*)lv_event_get_target(e));
  if (source == HUB) {
    hubMachine = i;
    go(firmwarePage);  // (its kinds and count are the machine's)
  } else {
    sdMachine = i - 1;  // ("All" first)
    fillList();
  }
}

// three to a row: small text
static void menuStyle(lv_obj_t* dd) {
  lv_obj_set_style_pad_ver(dd, 7, 0);
  lv_obj_set_style_pad_left(dd, 8, 0);
  lv_obj_set_style_pad_right(dd, 6, 0);
  lv_obj_set_style_text_font(dd, &lv_font_montserrat_12, 0);
}

static lv_obj_t* machineMenu(lv_obj_t* row) {
  std::string o = source == SD ? "All" : "";
  for (int i = 0; i < MACHINES; i++) {
    o += (o.empty() ? "" : "\n") + std::string(MACHINE_NAMES[i]);
  }
  lv_obj_t* dd = lv_dropdown_create(row);
  lv_dropdown_set_options(dd, o.c_str());
  lv_dropdown_set_selected(dd, source == SD ? sdMachine + 1 : hubShows());
  lv_obj_set_height(dd, 30);
  lv_obj_set_flex_grow(dd, 1);
  menuStyle(dd);
  lv_obj_add_event_cb(dd, pickMachine, LV_EVENT_VALUE_CHANGED, 0);
  return dd;
}

static void pickSort(lv_event_t* e) {
  sortBy = lv_dropdown_get_selected((lv_obj_t*)lv_event_get_target(e));
  fillList();
}

void ui::firmwareSource(bool hub) {
  source = hub ? HUB : SD;
  sourceChosen = true;  // (the SD card's even with none on it: no Wi-Fi then)
}

void ui::firmwarePage(lv_obj_t* page) {
  const Theme& t = theme();
  if (!sourceChosen && catalog::sd.empty()) {
    source = HUB;
  }
  refresh = spin = 0;
  listFm1 = fm1Now();
  lv_obj_t* head = header(page, 0);
  lv_obj_set_style_pad_column(head, 4, 0);
  const char* names[] = {"SD card", "Hub"};
  size_t counts[] = {catalog::sd.size(), (size_t)std::count_if(catalog::hub.begin(), catalog::hub.end(), forDevice)};
  for (int i = 0; i < 2; i++) {
    char s[32];
    snprintf(s, sizeof s, counts[i] ? "%s  %u" : "%s", names[i], (unsigned)counts[i]);
    lv_obj_t* b = button(head, s, pickSource, (void*)(intptr_t)i);
    lv_obj_set_height(b, 30);
    lv_obj_set_flex_grow(b, 1);
    if (i == source) {
      lv_obj_add_state(b, LV_STATE_CHECKED);
    }
  }
  if (source == HUB) {
    refresh = button(head, LV_SYMBOL_REFRESH, startHub);
    lv_obj_set_size(refresh, 40, 30);
    spin = lv_spinner_create(head);
    lv_obj_set_size(spin, 26, 26);
    lv_obj_set_style_arc_width(spin, 4, 0);
    lv_obj_set_style_arc_width(spin, 4, LV_PART_INDICATOR);
    lv_obj_set_style_margin_hor(spin, 7, 0);

    // for what, what kind, and in what order
    lv_obj_t* opts = box(page, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(opts, 4, 0);
    machineMenu(opts);
    std::vector<std::string> kinds;
    for (const Firmware& f : catalog::hub) {
      if (!forDevice(f)) {
        continue;
      }
      for (const std::string& k : f.kinds) {
        if (std::find(kinds.begin(), kinds.end(), k) == kinds.end()) {
          kinds.push_back(k);
        }
      }
    }
    std::sort(kinds.begin(), kinds.end());
    std::string o = "All kinds";
    int at = 0;
    for (size_t i = 0; i < kinds.size(); i++) {
      o += "\n" + kinds[i];
      if (kinds[i] == kindFilter) {
        at = i + 1;
      }
    }
    if (!at) {
      kindFilter.clear();
    }
    for (int i = 0; i < 2; i++) {
      lv_obj_t* dd = lv_dropdown_create(opts);
      lv_dropdown_set_options(dd, i ? "Newest\nA-Z" : o.c_str());
      lv_dropdown_set_selected(dd, i ? sortBy : at);
      lv_obj_set_height(dd, 30);
      lv_obj_set_flex_grow(dd, 1);
      menuStyle(dd);
      lv_obj_add_event_cb(dd, i ? pickSort : pickKind, LV_EVENT_VALUE_CHANGED, 0);
    }
  }
  if (source == SD && !catalog::sd.empty()) {
    lv_obj_t* opts = box(page, LV_FLEX_FLOW_ROW);
    machineMenu(opts);
  }
  list = scroller(page);
  lv_obj_add_event_cb(list, [](lv_event_t*) { stopFill(); }, LV_EVENT_DELETE, 0);  // (half-built: the page went)
  status = line(page, "", &lv_font_montserrat_12, t.dim);
  lv_obj_set_width(status, LV_PCT(100));

  // the Hub's list, the first time it's looked at after power-on (not while Bluetooth MIDI runs: Wi-Fi would pause it)
  if (source == HUB && settings.hubAuto && !hubAsked && settings.ssid[0] && hal::sd() &&
      !(midi::routing && hal::bleIsOn())) {
    hubAsked = true;
    jobs::hubList(false);
  }
  fillList();
  showStatus();
  tick = showStatus;
  jobDone = hubDone;
}

// ------------------------------------------------------------------------------------------ the details ---
static void showProgress() {
  bool busy = jobs::running && (jobs::kind == jobs::DOWNLOAD || jobs::kind == jobs::INSTALL);
  if (!busy && fm1Now() != shownFm1) {
    go(detailPage);  // an FM-1 came or went: the Install button changes
    return;
  }
  lv_obj_add_flag(busy ? actions : progress, LV_OBJ_FLAG_HIDDEN);
  lv_obj_remove_flag(busy ? progress : actions, LV_OBJ_FLAG_HIDDEN);
  if (busy) {
    uint32_t t = jobs::total, d = jobs::done;
    lv_bar_set_value(lv_obj_get_child(progress, 0), t ? (int32_t)((uint64_t)d * 100 / t) : 0, LV_ANIM_OFF);
    lv_label_set_text(progressText, devText(jobs::status()).c_str());
  }
}

static void downloaded() {
  if (jobs::kind != jobs::DOWNLOAD && jobs::kind != jobs::INSTALL) {
    return;
  }
  if (jobs::ok) {
    toast(jobs::kind == jobs::INSTALL ? "Installed." : "Saved to the SD card.");
    sel.onSd = true;
  } else {
    toast(devText(jobs::error).c_str(), true);
  }
  go(detailPage);
}

static void save(lv_event_t*) {
  if (!jobs::download(sel)) {
    toast("Busy: try again in a moment.", true);
  }
  showProgress();
}

static void install(lv_event_t* e) {
  lv_obj_t* l = lv_obj_get_child((lv_obj_t*)lv_event_get_target(e), 0);
  const char* sure = hasBackup() ? "Sure? Tap again" : "No backup. Sure?";
  if (strcmp(lv_label_get_text(l), sure)) {
    lv_label_set_text(l, sure);
    return;
  }
  if (!jobs::install(sel)) {
    toast("Busy: try again in a moment.", true);
  }
  showProgress();
}

static void erase(lv_event_t* e) {
  lv_obj_t* b = (lv_obj_t*)lv_event_get_target(e);
  lv_obj_t* l = lv_obj_get_child(b, 0);
  if (strcmp(lv_label_get_text(l), "Sure?")) {
    lv_label_set_text(l, "Sure?");  // a second tap deletes it
    return;
  }
  std::string p = catalog::path(std::string(FW_DIR "/") + sel.file);
  if (remove(p.c_str())) {
    toast("Can't delete it.", true);
    return;
  }
  catalog::scanSd();
  toast("Deleted.");
  go(firmwarePage);
}

static void detailPage(lv_obj_t* page) {
  const Theme& t = theme();
  header(page, sel.name.c_str(), firmwarePage);
  lv_obj_t* body = scroller(page);
  lv_obj_set_style_pad_row(body, 6, 0);
  lv_obj_set_style_pad_hor(body, 4, 0);

  std::string meta = sel.author.empty() ? "" : "by " + sel.author;
  if (!sel.version.empty()) {
    meta += (meta.empty() ? "v" : "   v") + sel.version;
  }
  if (!sel.date.empty()) {
    meta += "   " + sel.date;
  }
  if (!meta.empty()) {
    lv_obj_t* m = label(body, meta.c_str(), &lv_font_montserrat_14, t.dim);
    lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(m, LV_PCT(100));
  }

  lv_obj_t* chips = box(body, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_style_pad_gap(chips, 4, 0);
  for (const std::string& k : sel.kinds) {
    chip(chips, k.c_str(), kindColor(k));
  }
  if (sel.beta) {
    chip(chips, "beta", t.knob[1]);
  }
  if (sel.onSd) {
    chip(chips, LV_SYMBOL_SD_CARD " on SD", t.accent);
  }

  std::string facts;
  facts += kb(sel.size) + "   " + (sel.product.empty() ? "not a firmware package" : sel.product);
  lv_obj_t* f = label(body, facts.c_str(), &lv_font_montserrat_12, t.dim);
  lv_label_set_long_mode(f, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(f, LV_PCT(100));

  // nothing to go back to: a backup first (from stock, it's the only copy of M-VAVE's firmware and the FM-1's
  // settings, projects and samples)
  const Fm1Id* now = fm1::known(fm1::current());
  if (now && !now->ota() && !isSmk(now->model) && hal::sd() && !hasBackup()) {  // (backups: the FM-1 only, so far)
    lv_obj_t* br = box(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(br, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(br, 6, 0);
    lv_obj_t* bl = label(br, devText(LV_SYMBOL_WARNING " No backup of the FM-1 on the SD card yet: one lets you go back.").c_str(),
                         &lv_font_montserrat_12, t.knob[1]);
    lv_label_set_long_mode(bl, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(bl, 1);
    button(br, LV_SYMBOL_SAVE " Back up", [](lv_event_t*) { go(SOUNDS, backupPage); });
  }

  if (!sel.what.empty()) {
    lv_obj_t* w = label(body, sel.what.c_str(), &lv_font_montserrat_14, t.text);
    lv_label_set_long_mode(w, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(w, LV_PCT(100));
  } else if (!sel.hub && !sel.stock && source == SD) {
    label(body, "Not in the Hub's list.", &lv_font_montserrat_14, t.dim);
  }
  if (sel.keyboard || sel.mouse) {
    std::string h = std::string(LV_SYMBOL_USB "  Uses a USB ") + (sel.keyboard && sel.mouse ? "keyboard and mouse" : sel.keyboard ? "keyboard"
                                                                                                                                  : "mouse");
    label(body, h.c_str(), &lv_font_montserrat_14, t.knob[2]);
  }

  // what to do with it
  actions = box(page, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(actions, 6, 0);
  if (source == HUB && !sel.onSd) {
    lv_obj_t* b = button(actions, LV_SYMBOL_DOWNLOAD "  Save to SD", save);
    lv_obj_set_flex_grow(b, 1);
    if (!hal::sd()) {
      lv_obj_add_state(b, LV_STATE_DISABLED);
    }
  }
  if (source == SD) {
    lv_obj_t* b = button(actions, LV_SYMBOL_TRASH "  Delete", erase);
    lv_obj_set_flex_grow(b, 1);
  }
  // installing: on the FM-1 that answered, if the package is for its model
  shownFm1 = fm1Now();
  const Fm1Id* id = fm1::known(fm1::current());
  std::string model = sel.product.substr(0, sel.product.find('_'));
  bool smk = isSmk(sel.product.c_str());
  const char* why = !id                                                     ? (smk ? "Install: no SMK" : "Install: no FM-1")
                    : sel.product.empty()                                   ? "Not a firmware package"
                    : !id->ota() && !sameHardware(model.c_str(), id->model) ? "Not for this device"
                    // (an update loader's name can be another firmware's: one saying SMK-37 is one, so only the
                    // SMK-37's go on it; one saying FM-1 may be an SMK-37 with an FM-1 firmware's loader)
                    : id->ota() && isSmk(id->model) && !smk    ? "Not for this device"
                    : source == HUB && !sel.onSd && !hal::sd() ? "Install: no SD card"
                                                               : 0;
  lv_obj_t* inst = button(actions, why ? why : id->ota() ? "Finish install"
                                                         : "Install",
                          why ? 0 : install);
  lv_obj_set_flex_grow(inst, 1);
  if (why) {
    lv_obj_add_state(inst, LV_STATE_DISABLED);
  } else {
    lv_obj_add_state(inst, LV_STATE_CHECKED);  // the accent: the thing to do here
  }

  progress = box(page);
  lv_obj_set_style_pad_row(progress, 4, 0);
  lv_obj_t* bar = lv_bar_create(progress);
  lv_obj_set_size(bar, LV_PCT(100), 10);
  progressText = label(progress, "", &lv_font_montserrat_12, t.dim);

  showProgress();
  tick = showProgress;
  jobDone = downloaded;
}
