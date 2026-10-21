// Stand-in: no task watchdog on the PC.
#pragma once
#include <cstdint>
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 0x103
typedef struct { uint32_t timeout_ms; uint32_t idle_core_mask; bool trigger_panic; } esp_task_wdt_config_t;
inline int esp_task_wdt_init(const esp_task_wdt_config_t *) { return 0; }
inline int esp_task_wdt_reconfigure(const esp_task_wdt_config_t *) { return 0; }
inline int esp_task_wdt_add(void *) { return 0; }
inline int esp_task_wdt_delete(void *) { return 0; }
inline int esp_task_wdt_status(void *) { return 0; }
inline int esp_task_wdt_reset() { return 0; }
