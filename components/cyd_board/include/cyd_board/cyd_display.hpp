// CYD (ESP32-2432S028R) display + touch for LVGL.
//
//   ILI9341 or ST7789 (menuconfig) 320x240 on SPI2: SCLK 14, MOSI 13, MISO 12, CS 15, DC 2, BL 21
//   XPT2046 touch (bit-banged, keeps SPI3 for the SD card):
//       CLK 25, MOSI 32, MISO 39, CS 33, IRQ 36
//
// This file may be distributed under the terms of the GNU GPLv3 license.
#pragma once

#include <cstdint>
#include <functional>

namespace cyd {

struct UiTaskConfig {
    int core = 0;        // klippy runs on core 1
    int priority = 2;    // below the klippy and step generation tasks
    int stack = 5120;    // ~3 KB used
    int lines = 8;       // display lines per draw buffer (320 * 2 bytes each)
};

// Start LVGL with the display and touch in a new task.  build() runs in
// that task after the drivers are ready; every LVGL call must be made from
// that task (build() can create LVGL timers for periodic work).
bool start_ui(std::function<void()> build,
              const UiTaskConfig &cfg = UiTaskConfig());

// CPU time spent in lv_timer_handler() (rendering, events; without the
// waits for the display transfers) since start, in microseconds, and the
// number of display flushes (diagnostics)
uint64_t ui_busy_us();
uint32_t ui_flushes();

// Heap used by LVGL + display/touch drivers (with the draw buffer) and by
// the screen built in build(), and the UI task's unused stack (bytes).
// Allocations of other tasks while the UI starts are counted too: start
// the UI before or after the other tasks for exact values.
struct UiMemory {
    size_t drivers = 0, screen = 0, stack_unused = 0;
};
UiMemory ui_memory();

} // namespace cyd
