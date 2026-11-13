// M5Stack CoreS3 display + touch for LVGL (same interface as cyd_board).
//
//   ILI9342C 320x240 on SPI2: SCLK 36, MOSI 37, CS 3, DC 35
//   microSD on the same bus: CS 4, MISO 35 (shared with the display's DC)
//   Internal I2C (SDA 12, SCL 11): AXP2101 0x34 (power, backlight on
//   DLDO1), AW9523 0x58 (LCD / touch reset), FT6336U touch 0x38
//
// This file may be distributed under the terms of the GNU GPLv3 license.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace cores3 {

struct UiTaskConfig {
    int core = 0;      // klippy runs on core 1
    int priority = 2;  // below the klippy and step generation tasks
    int stack = 5120;
    int lines = 20;    // display lines per draw buffer (320 * 2 bytes each)
};

// Power (AXP2101, AW9523) and the SPI bus; once, from any task.  Called by
// start_ui() and mount_sd().
bool board_init();

// Mount the microSD card at path (FAT); false without a card.  The bus is
// shared with the display (works while the screen runs).
bool mount_sd(const char *path, int mhz = 20);

// Start LVGL with the display and touch in a new task; build() runs in
// that task (every LVGL call must be made from it).
bool start_ui(std::function<void()> build,
              const UiTaskConfig &cfg = UiTaskConfig());

uint64_t ui_busy_us();
uint32_t ui_flushes();

struct UiMemory {
    size_t drivers = 0, screen = 0, stack_unused = 0;
};
UiMemory ui_memory();

} // namespace cores3
