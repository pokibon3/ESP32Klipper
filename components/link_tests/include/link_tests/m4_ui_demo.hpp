// Local UI support for the link tests: the tests hand their printer to the
// UI, and an interactive demo runs the printer on the simulated MCU until a
// time limit.
//
// This file may be distributed under the terms of the GNU GPLv3 license.
#pragma once

#include <cstddef>
#include <functional>
#include <string>

namespace kl {

class Printer;

// Called with the test's printer once it is ready, and with nullptr before
// the printer goes away (set by the firmware that shows the local UI)
extern std::function<void(Printer *)> test_printer_hook;

// The simulated MCU's stand-in for sensorless homing: wrap G28 so that the
// X/Y DIAG pins trigger after some motor steps (see m4_macros.cpp)
void install_sim_homing(Printer &p);

// Interactive demo: printer.cfg + G-code files in memory, driven from the
// local UI for `minutes`.  Reports what was done.
bool run_m4_ui_demo(const std::string &port, int baud,
                    const std::string &printer_cfg, const char *gcode,
                    size_t gcode_len, double minutes, std::string *report);

} // namespace kl
