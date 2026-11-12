// Runs the printer on the simulated MCU (or a real one) with the
// Moonraker-compatible API, so Mainsail can be used from a browser on the
// development machine.
//
//   moonraker_host <config_dir> <gcodes_dir> [www_dir] [port] [mcu_port]
//
// <config_dir>/printer.cfg is the configuration (the "config" root in
// Mainsail), <gcodes_dir> the "gcodes" root, <www_dir> Mainsail's files.
// Open http://localhost:<port>/ (default 7125).
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "chelper_api.h"
#include "klippy/printer.hpp"
#include "link_tests/m4_ui_demo.hpp"
#include "moonraker/moonraker.hpp"
#include "moonraker/posix_server.hpp"

namespace kl {
uint64_t now_us() { return (uint64_t)(get_monotonic() * 1e6); }
int64_t heap_free_bytes() { return -1; }
int64_t heap_min_free_bytes() { return -1; }
void heap_monitor_start() {}
int64_t heap_monitor_stop() { return -1; }
std::string task_report() { return ""; }
} // namespace kl

static volatile sig_atomic_t stop_requested = 0;

int
main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s config_dir gcodes_dir [www_dir] [port]"
                        " [mcu_port]\n", argv[0]);
        return 2;
    }
    std::string config_dir = argv[1], gcodes_dir = argv[2];
    std::string www = argc > 3 ? argv[3] : "build/mainsail";
    int port = argc > 4 ? atoi(argv[4]) : 7125;
    std::string mcu_port = argc > 5 ? argv[5] : "sim:build/sim_mcu/sim_mcu";

    std::ifstream f(config_dir + "/printer.cfg");
    std::stringstream ss;
    ss << f.rdbuf();
    kl::PrinterOptions opt;
    opt.keep_config = true;
    kl::Printer p(opt);
    std::string err;
    if (!p.load_config(ss.str(), &err)) {
        fprintf(stderr, "printer.cfg: %s\n", err.c_str());
        return 1;
    }
    for (const auto &w : p.warnings())
        fprintf(stderr, "warning: %s\n", w.c_str());
    if (!p.connect(mcu_port, 250000, &err)) {
        fprintf(stderr, "connect: %s\n", err.c_str());
        return 1;
    }
    if (mcu_port.compare(0, 4, "sim:") == 0)
        kl::install_sim_homing(p);
    auto gcodes = std::make_shared<kl::PosixFileSystem>(gcodes_dir);
    auto config = std::make_shared<kl::PosixFileSystem>(config_dir);
    p.set_filesystem(gcodes);
    p.gcode().set_responder([](const std::string &m) {
        printf("%s\n", m.c_str());
    });

    kl::Moonraker::Options mo;
    mo.cpu = "host";
    kl::Moonraker mr(p, gcodes, config, mo);
    kl::PosixServer server(mr, port, www);
    if (!server.start(&err)) {
        fprintf(stderr, "server: %s\n", err.c_str());
        return 1;
    }
    printf("Mainsail: http://localhost:%d/  (Ctrl-C to stop)\n", port);
    signal(SIGINT, [](int) { stop_requested = 1; });
    while (!stop_requested)
        p.poll();
    server.stop();
    p.disconnect();
    return 0;
}
