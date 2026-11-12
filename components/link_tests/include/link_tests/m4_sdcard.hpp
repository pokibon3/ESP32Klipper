// M4 test: virtual_sdcard print with pause/resume/cancel.
//
// This file may be distributed under the terms of the GNU GPLv3 license.
#pragma once

#include <cstddef>
#include <string>

namespace kl {

bool run_m4_sdcard_test(const std::string &port, int baud,
                        const std::string &printer_cfg, const char *gcode,
                        size_t gcode_len, std::string *report);

} // namespace kl
