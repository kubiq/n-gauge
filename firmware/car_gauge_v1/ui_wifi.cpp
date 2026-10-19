// ============================================================================
// ui_wifi.cpp  —  the WiFi screen: how to reach the settings panel, the
// countdown back to the gauges, and the "Share SD card" button.
// See ui_priv.h.
// ============================================================================
#include "ui_priv.h"
#include "power.h"      // a tap is a sign of life; hold off sleep
#include <math.h>       // ceilf
#include <string.h>     // strncmp/strncpy in the update guard

// wifi widgets
static lv_obj_t *wifiSsid, *wifiPass, *wifiIp, *wifiQr, *wifiClients, *wifiArc, *wifiTitle;

// "Share SD card with PC" on the WiFi screen - see ui_update_usbdisk().
static lv_obj_t *shareBtn, *shareLbl;
static volatile bool shareTapped = false;

void ui_share_btn_cb(lv_event_t *e) {
  (void)e;
  if (touch_was_swipe()) return;         // a swipe that started on the button
  power_note_activity();
  shareTapped = true;                    // acted on from loop(), not from LVGL
}

// Any press on the WiFi screen restarts its countdown - see ui_take_wifi_touch().
static volatile bool wifiTouched = false;

static void wifi_touch_cb(lv_event_t *e) {
  (void)e;
  wifiTouched = true;                    // acted on from loop(), not from LVGL
}

// WIFI: how to reach the settings panel. The passphrase is generated per
// device and stored in NVS, so this screen is the only place it can be read
// - there is no default printed in the source and no reachable button.
lv_obj_t *ui_build_wifi() {
  lv_obj_t *s = make_screen();
  // Laid out top to bottom; the "Share SD card" button replaces the title
  // while a PC is connected. The QR is 130 px - still a few pixels per
  // module for this short payload.
  wifiTitle = make_name(s, "SCAN TO JOIN", -185);
  // Countdown to the automatic return to the gauges (WIFI_VIEW_MAX_MS): full
  // on arrival, empty at the switch - see ui_update_wifi_countdown(). Thin,
  // so it frames the screen rather than reading as a gauge.
  wifiArc = make_arc_sized(s, COL_BLUE, 0, WIFI_ARC_STEPS, 440, 6);
  lv_arc_set_value(wifiArc, WIFI_ARC_STEPS);
  // A touch anywhere on it means someone is still reading: start over.
  lv_obj_add_event_cb(s, wifi_touch_cb, LV_EVENT_PRESSED, NULL);

  // The QR carries a standard WIFI: payload, so a phone camera joins the
  // network without anyone typing a random 10-character passphrase. White
  // background with a quiet zone: a code drawn straight onto the dark theme
  // usually will not scan, and the border is part of the spec, not padding.
  wifiQr = lv_qrcode_create(s);
  lv_qrcode_set_size(wifiQr, 130);
  lv_qrcode_set_dark_color(wifiQr, lv_color_black());
  lv_qrcode_set_light_color(wifiQr, lv_color_white());
  lv_obj_set_style_border_color(wifiQr, lv_color_white(), 0);
  lv_obj_set_style_border_width(wifiQr, 6, 0);
  lv_obj_align(wifiQr, LV_ALIGN_CENTER, 0, -58);   // -129..13 with the border
  lv_obj_clear_flag(wifiQr, LV_OBJ_FLAG_CLICKABLE);   // taps still cycle views

  // Network name first, then the passphrase: that is the order they are asked
  // for, and the order they read in. Both in text as well as in the code,
  // because scanning fails often enough - dirty lens, sun on an AMOLED - that
  // the fallback has to be readable.
  wifiSsid = make_tyre_val(s, 0, 38);
  lv_obj_set_style_text_color(wifiSsid, COL_OIL, 0);
  wifiPass = make_tyre_val(s, 0, 70);
  lv_obj_set_style_text_color(wifiPass, COL_INK, 0);
  wifiIp   = make_name(s, "", 100);
  lv_obj_set_style_text_color(wifiIp, COL_BLUE, 0);
  // Connected clients, which doubles as confirmation that a phone actually
  // associated - useful when a join silently fails.
  wifiClients = make_name(s, "", 122);
  lv_obj_set_style_text_color(wifiClients, COL_LINE, 0);

  // Lend the SD card to a PC as a USB drive (usbdisk.cpp). Only shown while a
  // PC is connected, i.e. on the bench, and then in the title's place at the
  // top: at the bottom (+140) it sat against the fault notice, which shows
  // "no CAN bus" on the bench and took the taps. As big as fits for a
  // finger: 190x44 at -171 spans -193..-149, 20 px above the QR, with
  // its rounded ends (r <= 208) inside the countdown arc (r 214..220); the
  // touch area reaches 8 px further, over parts that take no taps.
  shareBtn = lv_button_create(s);
  lv_obj_set_size(shareBtn, 190, 44);
  lv_obj_align(shareBtn, LV_ALIGN_CENTER, 0, -171);
  lv_obj_set_ext_click_area(shareBtn, 8);
  lv_obj_set_style_radius(shareBtn, 22, 0);
  lv_obj_set_style_bg_color(shareBtn, COL_BG, 0);
  lv_obj_set_style_bg_color(shareBtn, COL_LINE, LV_STATE_PRESSED);
  lv_obj_set_style_border_color(shareBtn, COL_BLUE, 0);
  lv_obj_set_style_border_width(shareBtn, 2, 0);
  lv_obj_set_style_shadow_width(shareBtn, 0, 0);
  lv_obj_clear_flag(shareBtn, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(shareBtn, ui_share_btn_cb, LV_EVENT_CLICKED, NULL);
  shareLbl = lv_label_create(shareBtn);
  lv_obj_set_style_text_font(shareLbl, &lv_font_montserrat_14, 0);
  lv_obj_clear_flag(shareLbl, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_center(shareLbl);
  lv_obj_add_flag(shareBtn, LV_OBJ_FLAG_HIDDEN);
  return s;
}

void ui_update_wifi_countdown(float remaining) {
  // A redraw every frame: nothing else moves on this screen, so the sweep
  // can have the frame budget.
  static int last = -2;   // matches neither hidden (-1) nor any step
  int v = -1;
  if (remaining >= 0) {
    v = (int)ceilf(remaining * WIFI_ARC_STEPS);
    if (v > WIFI_ARC_STEPS) v = WIFI_ARC_STEPS;
  }
  if (v == last) return;
  // Show or hide only on a change. Clearing HIDDEN invalidates the whole
  // object even when it is already shown (lv_obj_remove_flag), and doing it
  // on every step redrew the 446 px arc - QR and all - each frame: ~140 ms a
  // frame, the loop down to 7 Hz, and swipes on this screen missed because
  // touch was read that rarely (2026-10-13). lv_arc_set_value() itself only
  // invalidates the sliver that moved.
  if ((v < 0) != (last < 0) || last == -2) {
    if (v < 0) lv_obj_add_flag(wifiArc, LV_OBJ_FLAG_HIDDEN);
    else       lv_obj_clear_flag(wifiArc, LV_OBJ_FLAG_HIDDEN);
  }
  last = v;
  if (v >= 0) lv_arc_set_value(wifiArc, v);
}

void ui_update_usbdisk(bool pcConnected, bool shared) {
  static int last = -1;
  const int s = !pcConnected ? 0 : shared ? 2 : 1;
  if (s == last) return;
  last = s;
  // The button takes the title's place, so only one of the two shows.
  if (s == 0) {
    lv_obj_add_flag(shareBtn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(wifiTitle, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_obj_add_flag(wifiTitle, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(shareBtn, LV_OBJ_FLAG_HIDDEN);
  const lv_color_t c = s == 2 ? COL_OIL : COL_BLUE;
  // While shared, the same button takes the card back (usbdisk_unshare).
  lv_label_set_text(shareLbl, s == 2 ? "Stop sharing SD card" : "Share SD card with PC");
  lv_obj_set_style_text_color(shareLbl, c, 0);
  lv_obj_set_style_border_color(shareBtn, c, 0);
}

bool ui_take_share_tap() {
  const bool t = shareTapped;
  shareTapped = false;
  return t;
}

bool ui_take_wifi_touch() {
  const bool t = wifiTouched;
  wifiTouched = false;
  return t;
}

void ui_update_wifi(const char *ssid, const char *pass, bool apUp,
                    const char *ip, uint8_t clients) {
  // Guarded like the rest. The passphrase changes at most once in a session,
  // but the client count changes whenever a phone comes and goes, so it is
  // part of the comparison - otherwise the count would never refresh.
  static char lastPass[33] = "\x01";     // impossible value, forces first draw
  static char lastSsid[33] = "\x01";
  static bool lastUp = false;
  static int  lastClients = -1;
  const bool passChanged =
      strncmp(lastPass, pass ? pass : "", sizeof(lastPass) - 1) != 0;
  const bool ssidChanged =
      strncmp(lastSsid, ssid ? ssid : "", sizeof(lastSsid) - 1) != 0;
  if (lastUp == apUp && lastClients == (int)clients &&
      !passChanged && !ssidChanged)
    return;
  lastUp = apUp;
  lastClients = (int)clients;
  strncpy(lastPass, pass ? pass : "", sizeof(lastPass) - 1);
  lastPass[sizeof(lastPass) - 1] = '\0';
  strncpy(lastSsid, ssid ? ssid : "", sizeof(lastSsid) - 1);
  lastSsid[sizeof(lastSsid) - 1] = '\0';

  if (clients == 0)      lv_label_set_text(wifiClients, "no phone connected");
  else if (clients == 1) lv_label_set_text(wifiClients, "1 phone connected");
  else                   lv_label_set_text_fmt(wifiClients, "%u connected", clients);
  lv_obj_set_style_text_color(wifiClients, clients ? COL_GOOD : COL_LINE, 0);

  lv_label_set_text(wifiSsid, ssid ? ssid : "");
  lv_label_set_text(wifiPass, pass ? pass : "");
  if (apUp && ip && ip[0]) {
    lv_label_set_text_fmt(wifiIp, "http://%s/", ip);
    lv_obj_set_style_text_color(wifiIp, COL_BLUE, 0);
  } else {
    lv_label_set_text(wifiIp, "starting...");
    lv_obj_set_style_text_color(wifiIp, COL_LINE, 0);
  }

  // Standard WIFI: payload, understood by both Android and iOS cameras.
  // Built here from the live values rather than baked in, because the
  // passphrase is generated per device and changeable from the settings page -
  // a hardcoded code would go stale with nothing to show it had.
  //
  // The separators ; and : and the escape \ are meaningful in this format, so
  // a passphrase containing them would need escaping. Ours cannot: the
  // generator's alphabet is letters and digits only (see WIFI_AP_PASS_ALPHABET),
  // and a passphrase typed on the settings page is the user's own to get wrong.
  // Only when the passphrase actually changed: regenerating a QR is a real
  // amount of work (encode plus a full redraw of a 160 px canvas) and would
  // otherwise run every time a phone connects or drops.
  if (passChanged || ssidChanged) {
    // \ ; , : and " are separators in the WIFI: format and must be escaped, or
    // a name like "Jakub;s car" silently produces a code that either fails to
    // scan or joins the wrong network. This was safe while the SSID was a
    // compile-time constant and the passphrase came from an alphanumeric
    // generator; it stopped being safe the moment both became user-editable
    // from the settings page.
    char payload[160];
    size_t o = 0;
    const char *head = "WIFI:T:WPA;S:";
    while (*head && o < sizeof(payload) - 1) payload[o++] = *head++;
    for (int part = 0; part < 2; part++) {
      const char *s = part == 0 ? (ssid ? ssid : "") : (pass ? pass : "");
      for (; *s && o < sizeof(payload) - 3; s++) {
        if (*s == '\\' || *s == ';' || *s == ',' || *s == ':' || *s == '"')
          payload[o++] = '\\';
        payload[o++] = *s;
      }
      const char *sep = part == 0 ? ";P:" : ";;";
      while (*sep && o < sizeof(payload) - 1) payload[o++] = *sep++;
    }
    payload[o] = '\0';
    lv_qrcode_update(wifiQr, payload, o);
  }
}
