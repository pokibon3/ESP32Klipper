// M4 test (host): homing on the simulated MCU.
//
//   m4_homing_test [printer.cfg] [port]
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "chelper_api.h"
#include "link_tests/m4_homing.hpp"

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
    std::string cfgpath = argc > 1 ? argv[1]
                                   : "config/skr_pico_corexy_penplotter.cfg";
    std::string port = argc > 2 ? argv[2] : "sim:build/sim_mcu/sim_mcu";
    std::ifstream f(cfgpath);
    if (!f) {
        fprintf(stderr, "cannot read %s\n", cfgpath.c_str());
        return 2;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    printf("config %s, port %s\n", cfgpath.c_str(), port.c_str());
    std::string report;
    bool ok = kl::run_m4_homing_test(port, 250000, ss.str(), &report);
    fputs(report.c_str(), stdout);
    return ok ? 0 : 1;
}
