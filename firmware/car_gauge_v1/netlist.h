// ============================================================================
// netlist.h  —  the WiFi networks the gauge has seen while parked, merged
//               scan by scan. Pure, like calc.h, so the host tests run it.
//
// 2026-11-02, for uploading the logs from where the car parks: the gauge
// scans just before it sleeps after a drive (netscan.cpp), and the app shows
// what it saw, to allow one with its password (or as open). This is that
// list: one entry per network name, its best signal, whether it is open,
// when and how often it was seen. A name already listed is updated, a new
// one added; when full, the one seen least - the oldest of those - makes way.
// ============================================================================
#pragma once
#include <stdint.h>
#include <string.h>

#define NETLIST_MAX 16

struct SeenNet {
  char     ssid[33];            // as broadcast; never empty (hidden ones are skipped)
  int8_t   rssi;                // the best seen, dBm
  uint8_t  open;                // 1 = no password
  uint16_t count;               // scans it was in
  uint32_t lastSeen;            // epoch s of the last scan it was in; 0 = clock unset
};

struct NetList {
  uint8_t n;
  SeenNet nets[NETLIST_MAX];
};

// One network from a scan. Its best signal is kept; a network that changed
// from open to secured (or back) takes the latest.
static inline void netlist_note(NetList *l, const char *ssid, int8_t rssi, bool open, uint32_t now) {
  if (!ssid || !ssid[0]) return;                  // hidden: nothing to name
  for (uint8_t i = 0; i < l->n; i++) {
    SeenNet &e = l->nets[i];
    if (strncmp(e.ssid, ssid, sizeof(e.ssid)) != 0) continue;
    if (rssi > e.rssi) e.rssi = rssi;
    e.open = open;
    if (e.count < UINT16_MAX) e.count++;
    if (now) e.lastSeen = now;
    return;
  }
  uint8_t at = l->n;
  if (l->n < NETLIST_MAX) {
    l->n++;
  } else {                                        // full: the least seen, oldest first
    at = 0;
    for (uint8_t i = 1; i < l->n; i++) {
      const SeenNet &a = l->nets[i], &b = l->nets[at];
      if (a.count < b.count || (a.count == b.count && a.lastSeen < b.lastSeen)) at = i;
    }
  }
  SeenNet &e = l->nets[at];
  memset(&e, 0, sizeof(e));                       // the name's terminator too
  memcpy(e.ssid, ssid, strnlen(ssid, sizeof(e.ssid) - 1));
  e.rssi = rssi;
  e.open = open;
  e.count = 1;
  e.lastSeen = now;
}
