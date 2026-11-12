// ============================================================================
// upload.h  —  the logs to the home server, over the phone's hotspot.
//
// Why and how (2026-11-04..06, TODO "Upload the logs"): the phone turns its
// hotspot on when it joins the car's Bluetooth (a Samsung routine), so the
// way home is there whenever the car is used. As the car starts - in the
// garage, or setting off, where the GPS points matter least - the gauge
// joins it, brings up a WireGuard tunnel into the home network and sends
// what the card holds that the server (server/upload_server.py) does not.
// Once per wake. Not before the sleep: by then the phone has gone.
//
// The radio is the upload's for that while: the AP goes down first (it is
// down while driving anyway, AP_DRIVE_OFF) and comes back after. It is not
// tried while a phone is on the AP. Measured with the WiFi/GPS test build:
// sending costs ~6 dB of GNSS C/N0 at any TX power - for the seconds an
// upload takes.
//
// The logs go as the card has them: encrypted to the phone's key. HTTP in
// the tunnel, which encrypts and authenticates; the server's token is a
// second lock. Resumed by size: a log only ever grows (server/README.md).
//
// The gauge's WireGuard key is made here, on first boot, and never leaves:
// the app shows its public half, to add as a peer on the router.
// ============================================================================
#pragma once
#include <stddef.h>
#include <stdint.h>

// setup(): the settings from NVS, and the key pair if there is none yet.
void upload_init();

// loop(), every pass: starts the upload when it is time, collects the result
// when it is done.
void upload_task(uint32_t now);

// apTask(): the AP stays down while this is true.
bool upload_holds_radio();
// power: no sleep in the middle of one.
bool upload_busy();

// Bench 'U': now, whatever the car says.
void upload_now();

// A hotspot and a server are set up (the app's Home server card).
bool upload_configured();

// How far the last tunnel close got before this boot (1-6, 6 = finished;
// 0 = none known) - for the boot line after a crash.
uint8_t upload_last_close_step();

// The gauge's own WiFi is asked for (apTask: its screen shown, a phone on
// it): a running upload stops at once, and none starts while this holds -
// the WiFi screen always brings the AP up (asked 2026-11-12). From loop(),
// every pass.
void upload_give_way(bool yes);

// The status screen (ui_update_status): where the upload is, in words.
// From loop(); the task's step and counts are read as they are - a number
// a step old at worst.
struct UpStatus {
  uint8_t tone;        // 0 waiting, 1 busy, 2 done, 3 failed
  char head[24];       // "Uploading", "Waiting", "Done", ...
  char detail[64];     // the step, or what it waits for
  char last[64];       // the last upload: when, and what came of it
  char fw[64];         // a firmware waiting to install, or ""
};
void upload_status(UpStatus *s);

// An upload this wake downloaded a new, signed firmware to the card
// (OTA_SD_PATH): power.cpp installs it before sleeping (ota_check_sd).
bool upload_fw_staged();

// /save's keys (upSsid, upPass, upUrl, upToken, wgAddr, wgPeer, wgHost,
// wgPort, upMaxMin - the time limit, minutes - upAuto - firmware updates
// from the server, "0" off - and wgNewKey): saved at
// once, whatever the value - a refused one had
// to be typed again (2026-11-06). The usual forms are cleaned up (no
// http://, a /32, host:port); what still looks wrong comes back as a few
// words, nullptr when nothing does.
const char *upload_setting(const char *key, const char *value);
extern const char *const UPLOAD_KEYS[];      // nullptr-terminated

// GET /api/upload: the settings (no password, token or private key - only
// whether they are set), the gauge's public key, the last upload.
int upload_json(char *out, size_t n);
