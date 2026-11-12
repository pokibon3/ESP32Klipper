// Platform hooks implemented separately by the ESP-IDF and host builds.
//
// This file may be distributed under the terms of the GNU GPLv3 license.
#pragma once

#include <cstdint>
#include <string>

namespace kl {

// Monotonic time in microseconds.
uint64_t now_us();

// Free heap bytes and lowest free heap seen so far (-1 if unknown).
int64_t heap_free_bytes();
int64_t heap_min_free_bytes();

// Track the lowest free heap between start and stop, including transient
// lows between samples.  stop returns it (-1 if unsupported).
void heap_monitor_start();
int64_t heap_monitor_stop();

// Task list with stack high-water marks (empty if unsupported)
std::string task_report();

} // namespace kl
