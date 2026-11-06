// ============================================================================
// ramlog.cpp  —  see ramlog.h.
// ============================================================================
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <string.h>
#include "ramlog.h"

// The linker's bounds of the internal static data (.data then .bss).
extern "C" char _data_start, _bss_end;

struct RamPart { const char *part; int32_t internal, psram; };
static RamPart parts[RAMLOG_MAX];
static uint8_t nParts = 0;
static size_t  setupFree = 0;      // internal free as setup() began
static size_t  preSetup = 0;       // gone before that: core tasks, USB, ...

static const uint32_t CAPS_IN = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;

RamAt ram_now() {
  return {heap_caps_get_free_size(CAPS_IN), heap_caps_get_free_size(MALLOC_CAP_SPIRAM)};
}

void ram_begin() {
  setupFree = heap_caps_get_free_size(CAPS_IN);
  preSetup = heap_caps_get_total_size(CAPS_IN) - setupFree;
}

static RamPart *find(const char *part, bool add) {
  for (uint8_t i = 0; i < nParts; i++)
    if (strcmp(parts[i].part, part) == 0) return &parts[i];
  if (!add || nParts >= RAMLOG_MAX) return nullptr;
  parts[nParts] = {part, 0, 0};
  return &parts[nParts++];
}

void ram_took(const char *part, const RamAt &before) {
  RamPart *p = find(part, true);
  if (!p) return;
  const RamAt now = ram_now();
  p->internal = (int32_t)before.internal - (int32_t)now.internal;
  p->psram = (int32_t)before.psram - (int32_t)now.psram;
}

void ram_freed(const char *part) {
  RamPart *p = find(part, false);
  if (p) p->internal = p->psram = 0;
}

static int32_t otherBytes() {
  int32_t claimed = 0;
  for (uint8_t i = 0; i < nParts; i++) claimed += parts[i].internal;
  return (int32_t)setupFree - (int32_t)heap_caps_get_free_size(CAPS_IN) - claimed;
}

int ram_json(char *out, size_t n) {
  int len = snprintf(out, n, "{\"static\":%u,\"preSetup\":%u,\"parts\":[",
                     (unsigned)(&_bss_end - &_data_start), (unsigned)preSetup);
  for (uint8_t i = 0; i < nParts && len > 0 && (size_t)len < n; i++)
    len += snprintf(out + len, n - len, "%s{\"part\":\"%s\",\"kb\":%.1f,\"psKb\":%.1f}",
                    i ? "," : "", parts[i].part, parts[i].internal / 1024.0f,
                    parts[i].psram / 1024.0f);
  if (len > 0 && (size_t)len < n)
    len += snprintf(out + len, n - len, "],\"otherKb\":%.1f}", otherBytes() / 1024.0f);
  return (len > 0 && (size_t)len < n) ? len : 0;
}

void ram_print() {
  Serial.printf("[RAM] static %.1f KB, before setup %.1f KB |",
                (&_bss_end - &_data_start) / 1024.0f, preSetup / 1024.0f);
  for (uint8_t i = 0; i < nParts; i++)
    Serial.printf(" %s %.1f", parts[i].part, parts[i].internal / 1024.0f);
  Serial.printf(" | other %.1f | free %.1f KB\n", otherBytes() / 1024.0f,
                heap_caps_get_free_size(CAPS_IN) / 1024.0f);
}
