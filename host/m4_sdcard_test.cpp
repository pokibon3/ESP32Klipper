// M4 test (host): SD card print with pause/resume/cancel.
//
//   m4_macro_test <printer.cfg> <file.gcode> [max_lines] [port]
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "chelper_api.h"
#include "link_tests/m4_sdcard.hpp"

namespace kl {
uint64_t now_us() { return (uint64_t)(get_monotonic() * 1e6); }
int64_t heap_free_bytes() { return -1; }
int64_t heap_min_free_bytes() { return -1; }
void heap_monitor_start() {}
int64_t heap_monitor_stop() { return -1; }
std::string task_report() { return ""; }
} // namespace kl

static std::string
read_file(const char *path)
{
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int
main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s printer.cfg file.gcode [max_lines] [port]\n",
                argv[0]);
        return 2;
    }
    int max_lines = argc > 3 ? atoi(argv[3]) : 400;
    std::string port = argc > 4 ? argv[4] : "sim:build/sim_mcu/sim_mcu";
    std::string report, gcode = read_file(argv[2]);
    bool ok = kl::run_m4_sdcard_test(port, 250000, read_file(argv[1]),
                                    gcode.data(), gcode.size(),
                                    &report);
    fputs(report.c_str(), stdout);
    return ok ? 0 : 1;
}
