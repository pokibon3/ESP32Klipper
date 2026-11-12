// M4 test: existing printer.cfg (macros, homing_override) + drawing file.
//
// This file may be distributed under the terms of the GNU GPLv3 license.
#pragma once

#include <cstddef>
#include <string>

namespace kl {

// gcode/gcode_len: the drawing file, read in place line by line (it is not
// copied: on the ESP32 it is a 180 KB file embedded in flash)
bool run_m4_macro_test(const std::string &port, int baud,
                       const std::string &printer_cfg, const char *gcode,
                       size_t gcode_len, int max_lines, std::string *report);

} // namespace kl
