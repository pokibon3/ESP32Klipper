// M2 test: one stepper axis through the real Klipper MCU protocol.
//
// This file may be distributed under the terms of the GNU GPLv3 license.
#pragma once

#include <string>

namespace kl {

// port: "sim:<path>" / tty path (host) or "uart:<n>:<tx>:<rx>" (ESP32).
// Appends a human readable report; returns true on PASS.
bool run_m2_axis_test(const std::string &port, int baud, double distance,
                      std::string *report);

} // namespace kl
