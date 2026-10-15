# The gauge firmware on the PC - see sim_main.cpp. Built from the firmware's
# own sources where they are, with stand-ins (stubs/, sim_*.cpp) for the
# board, and LVGL from the Arduino libraries folder with this folder's
# lv_conf.h (the gauge's, plus the SDL window).
cmake_minimum_required(VERSION 3.16)
project(gauge_sim C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(FW ${CMAKE_CURRENT_SOURCE_DIR}/../firmware/car_gauge_v1)
set(LVGL_DIR $ENV{HOME}/Arduino/libraries/lvgl CACHE PATH "LVGL, as the Arduino build uses it")

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)

file(GLOB_RECURSE LVGL_SOURCES ${LVGL_DIR}/src/*.c)
add_library(lvgl STATIC ${LVGL_SOURCES})
# This folder first: its lv_conf.h, and stubs/ for esp_heap_caps.h (LVGL's
# memory pool hook, from the gauge's config).
target_include_directories(lvgl PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/stubs ${LVGL_DIR} ${SDL2_INCLUDE_DIRS})
target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE)
target_link_libraries(lvgl PUBLIC ${SDL2_LIBRARIES})

file(GLOB FW_IMAGES ${FW}/*_img.c ${FW}/icon_*.c)
add_executable(gauge-sim
  sim_main.cpp
  sim_hw.cpp
  sim_replay.cpp
  gauge_firmware.cpp
  ${FW}/ui.cpp
  ${FW}/laps.cpp
  ${FW}/settings.cpp
  ${FW_IMAGES})
target_include_directories(gauge-sim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/stubs ${FW})
target_compile_options(gauge-sim PRIVATE -Wno-unused-function -Wno-unused-variable)
target_link_libraries(gauge-sim PRIVATE lvgl)
