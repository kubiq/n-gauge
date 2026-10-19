// ============================================================================
// ui_widgets.cpp  —  the widget builders every screen uses, and the swipe that
// moves between screens. See ui_priv.h.
// ============================================================================
#include "ui_priv.h"

// Swipe to change screens: left for the next one, right for the previous one.
// (The hardware button only wakes the gauge, since 2026-10-13.)
//
// It used to be a tap anywhere, which meant a tap could never mean anything
// else - no screen could have a button without also cycling away from itself.
// Swiping frees taps for buttons (the track screen's, first).
//
// The gesture arrives here from anywhere on the screen, children included:
// LVGL bubbles it up through every object carrying LV_OBJ_FLAG_GESTURE_BUBBLE,
// which lv_obj_create sets on anything WITH a parent - and stops at the screen,
// which has none. Vertical swipes are ignored, so they stay free for later use.
void screen_gesture_cb(lv_event_t *e) {
  (void)e;
  lv_indev_t *indev = lv_indev_active();
  const lv_dir_t dir = lv_indev_get_gesture_dir(indev);
  if (dir != LV_DIR_LEFT && dir != LV_DIR_RIGHT) return;
  ui_step_view(dir == LV_DIR_LEFT ? +1 : -1);
  // One swipe, one screen: the finger is still down, and on the new screen
  // LVGL takes what is under it for a fresh press - the rest of the same
  // swipe would be a second gesture, and a third. Ignored until it lifts.
  lv_indev_wait_release(indev);
}

// For button handlers: was the touch that just ended actually a swipe?
//
// Needed because LVGL sends LV_EVENT_CLICKED on release whenever nothing
// SCROLLED, whether or not a gesture fired (lv_indev.c, release path). So a
// swipe that happens to start on a button would also click it - on the track
// screen, a swipe across it could set the start line. The gesture direction is
// reset to LV_DIR_NONE on every new press, so it reliably describes this touch.
bool touch_was_swipe() {
  return lv_indev_get_gesture_dir(lv_indev_active()) != LV_DIR_NONE;
}

lv_obj_t* make_screen() {
  lv_obj_t *s = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(s, COL_BG, 0);
  lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);   // a press must land somewhere
  lv_obj_add_event_cb(s, screen_gesture_cb, LV_EVENT_GESTURE, NULL);
  return s;
}

lv_obj_t* make_arc_sized(lv_obj_t *parent, lv_color_t c, int min, int max,
                         int px, int width) {
  lv_obj_t *a = lv_arc_create(parent);
  lv_obj_set_size(a, px, px);
  lv_obj_center(a);
  lv_arc_set_rotation(a, 135);
  lv_arc_set_bg_angles(a, 0, 270);
  lv_arc_set_range(a, min, max);
  lv_obj_remove_style(a, NULL, LV_PART_KNOB);
  lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_color(a, COL_LINE, LV_PART_MAIN);
  lv_obj_set_style_arc_width(a, width, LV_PART_MAIN);
  lv_obj_set_style_arc_color(a, c, LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(a, width, LV_PART_INDICATOR);
  return a;
}

lv_obj_t* make_arc(lv_obj_t *parent, lv_color_t c, int min, int max) {
  lv_obj_t *a = lv_arc_create(parent);
  lv_obj_set_size(a, 420, 420);
  lv_obj_center(a);
  lv_arc_set_rotation(a, 135);
  lv_arc_set_bg_angles(a, 0, 270);
  lv_arc_set_range(a, min, max);
  lv_obj_remove_style(a, NULL, LV_PART_KNOB);          // no drag knob
  lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_color(a, COL_LINE, LV_PART_MAIN);
  lv_obj_set_style_arc_width(a, 16, LV_PART_MAIN);
  lv_obj_set_style_arc_color(a, c, LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(a, 16, LV_PART_INDICATOR);
  return a;
}

lv_obj_t* make_big_label(lv_obj_t *parent, lv_color_t c, int dy) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_color(l, c, 0);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_48, 0);  // enable in lv_conf.h
  lv_label_set_text(l, "--");
  lv_obj_align(l, LV_ALIGN_CENTER, 0, dy);
  return l;
}


lv_obj_t* make_tyre_val(lv_obj_t *parent, int dx, int dy) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_color(l, COL_INK, 0);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
  lv_label_set_text(l, "--");
  lv_obj_align(l, LV_ALIGN_CENTER, dx, dy);
  return l;
}

// A centred row of [symbol][value]: the caller creates the value label inside
// the returned row. A flex row rather than aligning the symbol to the label,
// because the label's width changes with the number ("9°" vs "105°") and
// lv_obj_align_to() does not follow that; the row re-lays itself out.
// The symbol is an A8 mask, drawn in its image_recolor colour - set that to
// show state.
lv_obj_t* make_icon_row(lv_obj_t *parent, int dy,
                        const lv_image_dsc_t *icon, lv_obj_t **iconOut) {
  lv_obj_t *row = lv_obj_create(parent);
  lv_obj_remove_style_all(row);                 // no background, border, padding
  lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, 10, 0);
  lv_obj_align(row, LV_ALIGN_CENTER, 0, dy);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *img = lv_image_create(row);
  lv_image_set_src(img, icon);
  lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
  lv_obj_set_style_image_recolor(img, COL_DIM, 0);   // no data until told
  lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
  *iconOut = img;
  return row;
}

// Pin a row's value label to the width of the widest thing it can show, text
// left-aligned. Without this the row re-centred on every value - "0.95" is
// narrower than "1.23", "--" narrower still - and the symbol beside it jumped
// sideways with each sample. Call after the label's font is set.
//
// `pattern` is the value's shape with every digit written as '8'. Each digit
// 0-9 is tried in turn and the widest kept: digits are NOT all the same width
// in Montserrat, and sizing for "8.88" alone let "0.00" overflow.
// CLIP rather than the default WRAP, so a value that somehow still does not
// fit can never become a second line - that changed the label's height, the
// row re-centred vertically, and the symbol jumped all the same.
void fix_value_width(lv_obj_t *label, const char *pattern) {
  const lv_font_t *font = lv_obj_get_style_text_font(label, 0);
  char buf[16];
  int32_t widest = 0;
  for (char d = '0'; d <= '9'; d++) {
    size_t i = 0;
    for (; pattern[i] && i < sizeof(buf) - 1; i++)
      buf[i] = pattern[i] == '8' ? d : pattern[i];
    buf[i] = '\0';
    lv_point_t sz;
    lv_text_get_size(&sz, buf, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (sz.x > widest) widest = sz.x;
  }
  lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
  lv_obj_set_width(label, widest);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
}

// "No value" in a fixed-width slot: "--" CENTRED and dim. Left-aligned like a
// reading, the narrow "--" hugged the symbol with the rest of the slot empty
// beside it, so the symbol and dashes sat visibly off-centre (2026-10-09) -
// and in the value's colour they read as a value, not as "nothing yet".
void show_no_value(lv_obj_t *label) {
  lv_label_set_text(label, "--");
  lv_obj_set_style_text_color(label, COL_DIM, 0);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
}
// Back to a reading: left-aligned (see fix_value_width), in its colour.
void show_value_style(lv_obj_t *label, lv_color_t c) {
  lv_obj_set_style_text_color(label, c, 0);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
}

lv_obj_t* make_name(lv_obj_t *parent, const char *txt, int dy) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_color(l, COL_INK, 0);
  lv_label_set_text(l, txt);
  lv_obj_align(l, LV_ALIGN_CENTER, 0, dy);
  return l;
}
