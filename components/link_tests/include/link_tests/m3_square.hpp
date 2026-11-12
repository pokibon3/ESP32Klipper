// M3 test: CoreXY from printer.cfg, squares at 300 mm/s via G-code.
//
// This file may be distributed under the terms of the GNU GPLv3 license.
#pragma once

#include <string>

namespace kl {

bool run_m3_square_test(const std::string &port, int baud,
                        const std::string &printer_cfg, std::string *report,
                        int squares = 5);

} // namespace kl
