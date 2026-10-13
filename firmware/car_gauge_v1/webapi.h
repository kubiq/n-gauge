// ============================================================================
// webapi.h  —  WiFi AP + phone settings panel / JSON API.
//   Up whenever the board is awake (WIFI_AP_ALWAYS), because the pod's button
//   is not reachable once it is mounted (and no longer toggles it). The old
//   privacy/battery objection to an always-on AP is
//   handled by stopping it as part of the sleep sequence instead: parked, the
//   board sleeps and the AP goes with it.
//
//   GET  /              mobile settings page
//   GET  /api/settings  current settings as JSON
//   GET  /api/live      live telemetry as JSON (oil/boost/coolant/gps/tyres)
//   POST /save          form post from the page; applies + persists
//   GET  /files         read-only listing of the SD card
//   GET  /dl?f=NAME     download a drive log or GPX track
//
//   Read-only by design: there is no delete endpoint. A drive log is often the
//   only copy of a run that cannot easily be repeated, and the AP password is
//   the only thing guarding this server.
// ============================================================================
#pragma once
#include <stdbool.h>
#include <stdint.h>

void web_start();           // bring it up (no-op if already up)
void web_stop();            // bring it down; called before deep sleep
bool web_is_up();
// How many stations are associated. Used by power.cpp to hold off sleep while
// someone is actually on the settings page, rather than merely because the AP
// is up - which, now that it is always up, would mean never sleeping.
uint8_t web_clients();
const char *web_ip();       // AP IP as text, "" when down
// Also for the bench status page (the held boot splash, ui.h):
const char *web_reset_text();   // why the gauge last started, in a word
// SD space in KB, cached for a minute; 0/0 with no card or while shared.
void web_sd_space_kb(uint32_t *usedKb, uint32_t *totalKb);
void web_task();            // call often while up; cheap no-op when down
