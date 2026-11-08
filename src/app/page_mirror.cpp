// The FM-1's screen, big (mirror.h): all of the screen, a back button in the margin beside it
#include "mirror.h"
#include "ui_priv.h"

using namespace ui;

#define FRAME_MS 30
// the FM-1's screen as tall as ours, centered
#define SIDE SCREEN_H
#define LEFT ((SCREEN_W - SIDE) / 2)

static lv_obj_t* img;
static lv_obj_t* waiting;
static lv_timer_t* frames;
static lv_image_dsc_t dsc;

static void frame(lv_timer_t*) {
  mirror::service();
  bool live = mirror::live();
  if (live == !lv_obj_has_flag(waiting, LV_OBJ_FLAG_HIDDEN)) {
    if (live) {
      lv_obj_add_flag(waiting, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_remove_flag(waiting, LV_OBJ_FLAG_HIDDEN);
    }
  }
  int y0, y1;
  if (!mirror::dirty(y0, y1)) {
    return;
  }
  lv_image_cache_drop(&dsc);
  lv_area_t a;
  lv_obj_get_coords(img, &a);
  int top = a.y1;
  a.y1 = top + y0 * SIDE / MIRROR_SIZE;
  a.y2 = top + ((y1 + 1) * SIDE + MIRROR_SIZE - 1) / MIRROR_SIZE;
  lv_obj_invalidate_area(img, &a);
}

static void gone(lv_event_t*) {
  if (frames) {
    lv_timer_delete(frames);
    frames = 0;
  }
  mirror::on(false);
}

void ui::mirrorPage(lv_obj_t* page) {
  const Theme& t = theme();
  fullscreen(true);
  mirror::on(true);
  lv_obj_set_style_pad_all(page, 0, 0);
  lv_obj_set_style_bg_color(page, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
  lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_layout(page, LV_LAYOUT_NONE);

  dsc = {};
  dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  dsc.header.cf = LV_COLOR_FORMAT_RGB565;
  dsc.header.w = MIRROR_SIZE;
  dsc.header.h = MIRROR_SIZE;
  dsc.header.stride = MIRROR_SIZE * 2;
  dsc.data = (const uint8_t*)mirror::pixels();
  dsc.data_size = MIRROR_SIZE * MIRROR_SIZE * 2;
  img = lv_image_create(page);
  lv_obj_set_pos(img, LEFT, 0);
  lv_obj_set_size(img, SIDE, SIDE);
  lv_image_set_inner_align(img, LV_IMAGE_ALIGN_STRETCH);
  if (dsc.data) {
    lv_image_set_src(img, &dsc);
  }
  lv_obj_add_event_cb(img, gone, LV_EVENT_DELETE, 0);

  waiting = label(page, devText("Waiting for the FM-1's screen.\n\nIt needs a Big Screen firmware\n"
                                "(the Hub's bigscreen tag).")
                            .c_str(),
                  &lv_font_montserrat_14, t.dim);
  lv_obj_set_width(waiting, SIDE - 4);
  lv_obj_set_style_text_align(waiting, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(waiting, LV_ALIGN_CENTER, 0, 0);

  lv_obj_t* back = button(page, LV_SYMBOL_LEFT, [](lv_event_t*) { go(MIDI, midiPage); });
  lv_obj_set_size(back, LEFT > 40 ? 40 : LEFT - 4, 30);
  lv_obj_set_pos(back, 2, 4);

  if (frames) {
    lv_timer_delete(frames);
  }
  frames = lv_timer_create(frame, FRAME_MS, 0);
  frame(0);
}
