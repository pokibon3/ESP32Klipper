// M2 test (host): one stepper axis through the real Klipper MCU protocol.
//
//   m2_axis_test [port] [distance_mm]
//
// port defaults to "sim:build/sim_mcu/sim_mcu" (starts the simulated MCU);
// a tty path connects to a real MCU.
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <cstdio>
#include <cstdlib>
#include <string>

#include "chelper_api.h"
#include "link_tests/m2_axis.hpp"

namespace kl {
uint64_t now_us() { return (uint64_t)(get_monotonic() * 1e6); }
int64_t heap_free_bytes() { return -1; }
int64_t heap_min_free_bytes() { return -1; }
void heap_monitor_start() {}
int64_t heap_monitor_stop() { return -1; }
std::string task_report() { return ""; }
} // namespace kl

int
main(int argc, char **argv)
{
    std::string port = argc > 1 ? argv[1] : "sim:build/sim_mcu/sim_mcu";
    double distance = argc > 2 ? atof(argv[2]) : 100.;
    printf("connecting to %s ...\n", port.c_str());
    std::string report;
    bool ok = kl::run_m2_axis_test(port, 250000, distance, &report);
    fputs(report.c_str(), stdout);
    return ok ? 0 : 1;
}
