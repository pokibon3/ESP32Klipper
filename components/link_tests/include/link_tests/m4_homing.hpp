// M4 test: homing (sensorless X/Y, switch Z) on the simulated MCU.
//
// This file may be distributed under the terms of the GNU GPLv3 license.
#pragma once

#include <string>

namespace kl {

bool run_m4_homing_test(const std::string &port, int baud,
                        const std::string &printer_cfg, std::string *report);

} // namespace kl
