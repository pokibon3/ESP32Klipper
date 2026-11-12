// Local UI demo on the simulated MCU (see m4_ui_demo.hpp).
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "link_tests/m4_ui_demo.hpp"

#include <cstdarg>
#include <cstdio>
#include <memory>

#include "klippy/files.hpp"
#include "klippy/printer.hpp"
#include "klippy_lite/platform.hpp"

namespace kl {

std::function<void(Printer *)> test_printer_hook;

void
install_sim_homing(Printer &p)
{
    auto prev = p.gcode().get_handler("G28");
    auto depth = std::make_shared<int>(0);
    p.gcode().register_command(
        "G28", [&p, prev, depth](GCodeCommand &g, std::string *err) {
            if (*depth == 0) {
                // DIAG low now, high after 1500 steps towards the X/Y
                // endstops (pins as in the plotter's printer.cfg)
                for (const char *c :
                     {"sim_set_input pin=4 forced=1 value=0",
                      "sim_set_input pin=3 forced=1 value=0",
                      "sim_trigger_on_steps pin=4 value=1 step_pin=11"
                      " dir_pin=10 dir_value=0 dir2_pin=5 dir2_value=0"
                      " steps=1500",
                      "sim_trigger_on_steps pin=3 value=1 step_pin=6"
                      " dir_pin=10 dir_value=0 dir2_pin=5 dir2_value=1"
                      " steps=1500"}) {
                    std::string e;
                    p.mcu().serial().send(c, &e);
                }
            }
            ++*depth;
            bool ok = prev(g, err);
            --*depth;
            return ok;
        });
}

namespace {

// A short drawing for trying the UI: a 60 mm square and its diagonals
const char kSquare[] =
    "G90\n"
    "G1 X40 Y40 F12000\n"
    "SET_PIN PIN=LASER VALUE=1\n"
    "G1 X100 Y40 F6000\nG1 X100 Y100\nG1 X40 Y100\nG1 X40 Y40\n"
    "G1 X100 Y100\n"
    "SET_PIN PIN=LASER VALUE=0\n"
    "G1 X40 Y100 F12000\n"
    "SET_PIN PIN=LASER VALUE=1\n"
    "G1 X100 Y40 F6000\n"
    "SET_PIN PIN=LASER VALUE=0\n"
    "G1 X0 Y150 F12000\n";

} // namespace

bool
run_m4_ui_demo(const std::string &port, int baud, const std::string &cfg,
               const char *gcode, size_t gcode_len, double minutes,
               std::string *report)
{
    char buf[256];
    Printer p;
    std::string err;
    if (!p.load_config(cfg, &err)) {
        *report += "config error: " + err + "\n";
        return false;
    }
    if (!p.connect(port, baud, &err)) {
        *report += "connect failed: " + err + "\n";
        return false;
    }
    auto fs = std::make_shared<MemoryFileSystem>();
    if (gcode)
        fs->add("card_opt.gcode", gcode, gcode_len);
    fs->add("square.gcode", kSquare, sizeof(kSquare) - 1);
    p.set_filesystem(fs);
    install_sim_homing(p);
    std::vector<std::string> log;
    p.gcode().set_responder([&](const std::string &m) {
        if (log.size() < 40)
            log.push_back(m);
    });
    if (test_printer_hook)
        test_printer_hook(&p);
    heap_monitor_start();
    uint64_t t0 = now_us();
    while ((now_us() - t0) * 1e-6 < minutes * 60.)
        p.poll();
    int64_t heap_low = heap_monitor_stop();
    if (test_printer_hook)
        test_printer_hook(nullptr);
    PrintStatsInfo ps = p.print_stats();
    snprintf(buf, sizeof(buf),
             "UI demo: %.0f min, ready %d, last print '%s' state '%s',"
             " lowest free heap %lld\n",
             minutes, (int)p.is_ready(), ps.filename.c_str(),
             ps.state.c_str(), (long long)heap_low);
    *report += buf;
    *report += "responses:\n";
    for (auto &m : log)
        *report += "  " + m + "\n";
    bool ok = p.is_ready();
    p.disconnect();
    return ok;
}

} // namespace kl
