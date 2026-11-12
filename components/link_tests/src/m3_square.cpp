// M3 test: CoreXY plotter built from printer.cfg, driven with G-code.
//
// Draws squares of 100 mm at 300 mm/s (F18000) with the pen (pwm_tool
// LASER) down while drawing, then checks that both CoreXY steppers are
// back at their start position on the MCU and that the pen output
// switched as often as commanded.  Reports timing, step generation load
// and (on the ESP32) heap use.
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "link_tests/m3_square.hpp"

#include <cstdarg>
#include <chrono>
#include <cstdio>
#include <map>
#include <thread>

#include "chelper_api.h"
#include "klippy/printer.hpp"
#include "klippy_lite/platform.hpp"

namespace kl {

namespace {
struct Report {
    std::string *out;
    void printf(const char *fmt, ...) __attribute__((format(printf, 2, 3))) {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);
        *out += buf;
    }
};
} // namespace

bool
run_m3_square_test(const std::string &port, int baud, const std::string &cfg,
                   std::string *report, int squares)
{
    Report rep{report};
    int64_t heap0 = heap_free_bytes();
    // Heap left after each connect stage
    std::string stages;
    int64_t last_heap = heap0;
    debug_stage_hook = [&](const char *stage) {
        int64_t h = heap_free_bytes();
        if (h >= 0) {
            char buf[128];
            snprintf(buf, sizeof(buf), "  %-42s %7lld free (%+lld)\n", stage,
                     (long long)h, (long long)(h - last_heap));
            stages += buf;
            last_heap = h;
        }
    };
    Printer p;
    std::string err;
    if (!p.load_config(cfg, &err)) {
        rep.printf("config error: %s\n", err.c_str());
        return false;
    }
    debug_stage("printer.cfg loaded (objects created)");
    for (const auto &w : p.warnings())
        rep.printf("warning: %s\n", w.c_str());
    if (!p.connect(port, baud, &err)) {
        rep.printf("connect failed: %s\nserial stats: %s\n", err.c_str(),
                   p.mcu().serial().last_stats().c_str());
        return false;
    }
    int64_t heap1 = heap_free_bytes();
    bool tmc_ok = true;
    // TMC2209: registers written at connect must match Klipper's values
    // (stepper_x of the sample config: 0.5 A, 16 microsteps, both edges,
    // stealthChop threshold 999999, SGTHRS 30)
    if (p.mcu().parser().lookup_name("sim_tmc_get")) {
        static const std::map<int, const char *> regnum = {
            {0x00, "GCONF"}, {0x03, "SLAVECONF"}, {0x10, "IHOLD_IRUN"},
            {0x11, "TPOWERDOWN"}, {0x13, "TPWMTHRS"}, {0x14, "TCOOLTHRS"},
            {0x40, "SGTHRS"}, {0x42, "COOLCONF"}, {0x6c, "CHOPCONF"},
            {0x70, "PWMCONF"}};
        static const std::map<std::string, uint32_t> want_x = {
            {"GCONF", 0x000001c0}, {"CHOPCONF", 0x34030053},
            {"IHOLD_IRUN", 0x00080f0f}, {"PWMCONF", 0xc80d0e24},
            {"SGTHRS", 30}, {"TPOWERDOWN", 20}, {"SLAVECONF", 0x200}};
        for (auto &t : p.tmcs()) {
            int addr = t->stepper_name() == "stepper_x" ? 0
                : t->stepper_name() == "stepper_y" ? 2 : 1;
            std::string line = "tmc2209 " + t->stepper_name() + ":";
            for (const auto &kv : regnum) {
                MsgParams r;
                if (!p.mcu().serial().send_with_response(
                        "sim_tmc_get addr=" + std::to_string(addr) + " reg="
                        + std::to_string(kv.first), "sim_tmc_reg", &r, &err))
                    continue;
                uint32_t got = (uint32_t)r.get("val");
                uint32_t mine = 0;
                for (const auto &rv : t->registers())
                    if (rv.first == kv.second)
                        mine = rv.second;
                char buf[48];
                snprintf(buf, sizeof(buf), " %s=%08x", kv.second, (unsigned)got);
                line += buf;
                tmc_ok = tmc_ok && got == mine;
                if (t->stepper_name() == "stepper_x") {
                    auto w = want_x.find(kv.second);
                    if (w != want_x.end() && w->second != got) {
                        tmc_ok = false;
                        line += "(want " + std::to_string(w->second) + ")";
                    }
                }
            }
            rep.printf("%s\n", line.c_str());
        }
        rep.printf("TMC registers: %s\n", tmc_ok ? "OK" : "MISMATCH");
    }
    rep.printf("connected: %d moves on the MCU, kin_flush_delay %.4f\n",
               p.mcu().move_count(), p.motion_queuing().get_kin_flush_delay());
    std::vector<std::string> responses;
    p.gcode().set_responder(
        [&](const std::string &m) { responses.push_back(m); });

    std::string script = "SET_KINEMATIC_POSITION X=0 Y=0 Z=0\nG90\nG1 F18000\n";
    for (int i = 0; i < squares; i++)
        script += "SET_PIN PIN=LASER VALUE=1\n"
                  "G1 X100 Y0\nG1 X100 Y100\nG1 X0 Y100\nG1 X0 Y0\n"
                  "SET_PIN PIN=LASER VALUE=0\n";
    script += "M400\nM114\n";
    uint64_t t0 = now_us();
    double pt0 = p.toolhead().print_time();
    bool ok = p.run_script(script, &err);
    double wall = (now_us() - t0) * 1e-6;
    if (!ok) {
        rep.printf("G-code error: %s\n", err.c_str());
        return false;
    }
    if (p.mcu().is_shutdown()) {
        rep.printf("FAIL: MCU shutdown: %s\n", p.mcu().shutdown_msg().c_str());
        return false;
    }
    // Let the input shaper tail (kin_flush_delay) reach the MCU before
    // querying positions
    double until = p.monotonic() + 0.3;
    while (p.monotonic() < until) {
        p.service();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    for (const auto &r : responses)
        rep.printf("%s\n", r.c_str());
    double motion = p.toolhead().print_time() - pt0;
    const GenStats &gs = p.motion_queuing().gen_stats();
    rep.printf("%d squares: motion %.3f s, wall %.3f s, %lld moves,"
               " lookahead max %u, runouts %llu\n", squares, motion, wall,
               (long long)p.toolhead().move_count(),
               (unsigned)p.toolhead().lookahead_max(),
               (unsigned long long)p.toolhead().runouts());
    rep.printf("step gen: %llu calls, %.3f s cpu, worst %.2f ms, worst load"
               " %.1f%%\n", (unsigned long long)gs.calls, gs.total_us * 1e-6,
               gs.max_us * 1e-3, gs.max_load * 100.);

    // Both CoreXY steppers (and Z) must be back where they started
    for (auto &s : p.steppers()) {
        int64_t pos;
        if (!p.query_mcu_position(*s, &pos, &err)) {
            rep.printf("FAIL: %s\n", err.c_str());
            return false;
        }
        rep.printf("%s: mcu position %lld (expected %lld)\n", s->name.c_str(),
                   (long long)pos, (long long)s->get_mcu_position());
        ok = ok && pos == s->get_mcu_position() && pos == 0;
    }
    if (p.mcu().parser().lookup_name("sim_get_pin")) {
        MsgParams sp;
        const struct { int pin; const char *name; } pins[] = {
            {11, "stepper_x step"}, {6, "stepper_y step"}, {20, "LASER"}};
        for (const auto &pi : pins) {
            if (!p.mcu().serial().send_with_response(
                    "sim_get_pin pin=" + std::to_string(pi.pin), "sim_pin",
                    &sp, &err))
                continue;
            rep.printf("gpio%d (%s): %lld edges, level %lld\n", pi.pin,
                       pi.name, (long long)sp.get("edges"),
                       (long long)sp.get("level"));
            if (pi.pin == 20)
                ok = ok && sp.get("edges") == 2 * squares && !sp.get("level");
        }
    }
    int64_t heap2 = heap_free_bytes();
    if (heap0 >= 0)
        rep.printf("heap free: %lld at start, %lld after connect, %lld after"
                   " moves, min since boot %lld\n", (long long)heap0,
                   (long long)heap1, (long long)heap2,
                   (long long)heap_min_free_bytes());
    if (!stages.empty())
        *report += "heap by stage:\n" + stages;
    std::string tasks = task_report();
    if (!tasks.empty())
        *report += "tasks (name state prio stack_free num):\n" + tasks;
    debug_stage_hook = nullptr;
    rep.printf("serial stats: %s\n", p.mcu().serial().stats().c_str());
    ok = ok && tmc_ok && p.is_ready();
    if (!p.is_ready())
        rep.printf("state: %s\n", p.state_message().c_str());
    rep.printf("%s\n", ok ? "M3 PASS" : "M3 FAIL");
    p.disconnect();
    return ok;
}

} // namespace kl
