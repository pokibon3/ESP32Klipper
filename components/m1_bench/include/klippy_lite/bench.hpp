// M1 benchmark: run a G-code file through the ported Klipper host motion
// pipeline (gcode_move -> toolhead lookahead -> trapq -> itersolve
// (+ input shaper) -> stepcompress -> steppersync) without an MCU, and
// measure how fast it runs compared to the motion time it produces.
//
// This file may be distributed under the terms of the GNU GPLv3 license.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "klippy_lite/motion.hpp"

namespace kl {

// Buffering for the ESP32 port (Klipper values in parentheses).  Chosen to
// fit the CYD heap: queue 0.5 s ahead of the MCU (1.0 s), generate steps
// 0.2-0.35 s ahead (0.45-0.7 s), keep 0.25 s of history (30 s).
inline ToolHeadConfig
esp32_toolhead()
{
    ToolHeadConfig c;
    c.buffer_time_high = 0.5;
    if (const char *e = getenv("BENCH_MAXMOVES"))
        c.lookahead_max_moves = (size_t)atoi(e);
    else
        c.lookahead_max_moves = 64;
    return c;
}

inline MotionQueuingConfig
esp32_queuing()
{
    MotionQueuingConfig c;
    c.history_expire = 0.25;
    c.bgflush_sg_low = 0.20;
    c.bgflush_sg_high = 0.35;
    return c;
}

struct BenchConfig {
    const char *label = "default";
    // Kinematics / steppers (defaults: Fraxinus0pen printer.cfg)
    double rotation_distance = 47.5;
    int full_steps_per_rotation = 200;
    int microsteps = 16;
    ToolHeadConfig toolhead = esp32_toolhead();
    MotionQueuingConfig queuing = esp32_queuing();
    // Input shaper (defaults: Fraxinus0pen SAVE_CONFIG values)
    bool input_shaper = false;
    double shaper_freq_x = 83.2;
    double shaper_freq_y = 58.6;
    double damping_ratio = 0.1;
    // Feedrate multiplier applied to every F value (like M220 S<pct>/100)
    double speed_factor = 1.0;
    // MCU (SKR Pico RP2040)
    double mcu_freq = 12000000.;
    int move_count = 500;
};

struct BenchResult {
    const char *label = "";
    bool ok = false;
    uint64_t lines = 0, unknown_cmds = 0, moves = 0, pwm_updates = 0;
    uint64_t runouts = 0;
    double print_time = 0.;          // motion time produced (s)
    uint64_t cpu_total_us = 0;       // wall time of the whole run
    uint64_t cpu_gen_us = 0;         // time inside steppersyncmgr_gen_steps
    uint64_t gen_calls = 0, gen_max_us = 0;
    double gen_max_load = 0.;        // worst gen_steps cpu/motion ratio
    double kin_flush_delay = 0.;
    size_t lookahead_max = 0;
    bench_sink_stats sink = {};
    int64_t heap_free_before = -1, heap_min_free = -1, heap_after_setup = -1;
    int64_t heap_true_min = -1;      // incl. transient lows (ESP32 only)

    double realtime_ratio() const {
        return cpu_total_us ? print_time / (cpu_total_us * .000001) : 0.;
    }
};

BenchResult run_bench(const char *gcode, size_t len, const BenchConfig &cfg);
void print_result(const BenchResult &r, FILE *out);

// The standard M1 suite (same on host and ESP32):
//   1. as written, no input shaper
//   2. as written, zv input shaper (Fraxinus0pen configuration)
//   3. 2x feedrate, zv input shaper
//   4. 5x feedrate, zv input shaper (travel capped by max_velocity 500)
std::vector<BenchConfig> default_suite();

} // namespace kl
