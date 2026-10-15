// The gauge's own LVGL configuration, with the PC's display switched on:
// LVGL's SDL driver draws into a window, the mouse is the finger. Everything
// else - fonts, colour depth, memory (its PSRAM hook is plain malloc here,
// stubs/esp_heap_caps.h) - is the firmware's, so the screens are the same.
#include "../firmware/car_gauge_v1/lv_conf.h"

#undef LV_USE_SDL
#define LV_USE_SDL 1
#define LV_SDL_INCLUDE_PATH <SDL2/SDL.h>
#define LV_SDL_RENDER_MODE LV_DISPLAY_RENDER_MODE_DIRECT
#define LV_SDL_BUF_COUNT 1
#define LV_SDL_FULLSCREEN 0
#define LV_SDL_DIRECT_EXIT 1
#define LV_SDL_MOUSEWHEEL_MODE LV_SDL_MOUSEWHEEL_MODE_ENCODER
