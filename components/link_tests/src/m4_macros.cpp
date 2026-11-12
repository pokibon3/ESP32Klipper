// M4 test: the existing plotter's printer.cfg (macros, homing_override)
// running the start of a real drawing file, on the simulated MCU.
//
// The file is fed line by line like virtual_sdcard does.  G28 goes through
// [homing_override] -> _HOME_X / _HOME_Y macros -> sensorless G28 X / Y
// (the simulated DIAG pins trigger after a delay).  Drawing lines use the
// G00/G01/M03/M3/M5 macros.  Checks: no errors, the pen output switched as
// often as the M3/M5 sequence implies, the final position matches the last
// coordinates, host and MCU step positions agree.
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "link_tests/m4_macros.hpp"

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#ifndef ESP_PLATFORM
#include <dlfcn.h>
#endif
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
run_m4_macro_test(const std::string &port, int baud, const std::string &cfg,
                  const char *gcode, size_t gcode_len, int max_lines,
                  std::string *report)
{
    Report rep{report};
    int64_t heap0 = heap_free_bytes();
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
    int64_t heap_cfg = heap_free_bytes();
    for (const auto &w : p.warnings())
        rep.printf("warning: %s\n", w.c_str());
    heap_monitor_start();
    bool connected = p.connect(port, baud, &err);
    int64_t min_connect = heap_monitor_stop();
    if (!connected) {
        rep.printf("connect failed: %s\n", err.c_str());
        return false;
    }
    int64_t heap_conn = heap_free_bytes();
    std::vector<std::string> responses;
    p.gcode().set_responder(
        [&](const std::string &m) { responses.push_back(m); });
    auto mcu_cmd = [&](const std::string &c) {
        std::string e;
        p.mcu().serial().send(c, &e);
    };
    // Sensorless homing: DIAG low; the motors "stall" after 1500 steps of
    // -X motion (A and B both negative: dir gpio10=0, gpio5=0) and after
    // 1500 steps of -Y motion (A negative, B positive: gpio10=0, gpio5=1)
    mcu_cmd("sim_set_input pin=4 forced=1 value=0");
    mcu_cmd("sim_set_input pin=3 forced=1 value=0");
    mcu_cmd("sim_trigger_on_steps pin=4 value=1 step_pin=11 dir_pin=10"
            " dir_value=0 dir2_pin=5 dir2_value=0 steps=1500");
    mcu_cmd("sim_trigger_on_steps pin=3 value=1 step_pin=6 dir_pin=10"
            " dir_value=0 dir2_pin=5 dir2_value=1 steps=1500");

    // Feed the file and track what the G-code implies
    size_t gpos = 0;
    std::string line;
    // Next line of the file without copying it (like virtual_sdcard)
    auto next_line = [&](std::string *l) {
        if (gpos >= gcode_len)
            return false;
        const char *s = gcode + gpos;
        const char *nl = (const char *)memchr(s, '\n', gcode_len - gpos);
        size_t n = nl ? (size_t)(nl - s) : gcode_len - gpos;
        l->assign(s, n);
        gpos += n + 1;
        return true;
    };
    int lines = 0, pen_switches = 0;
    bool pen = false;
    double want_x = NAN, want_y = NAN;
    heap_monitor_start();
    uint64_t t0 = now_us();
    double homed_at = 0.;
    bool ok = true;
    std::string end_macro;
    int64_t heap_g28 = -1, min_homing = -1;
    while (lines < max_lines && next_line(&line)) {
        lines++;
        std::string cmd, cl;
        GCodeCommand::ParamList params;
        GCodeDispatch::parse_line(line, &cmd, &params, &cl);
        if (cmd == "M02" || cmd == "M2") {
            // End-of-program macro (moves and pen); run after the checks
            end_macro = line;
            break;
        }
        if (cmd == "M3" || cmd == "M03" || cmd == "M5" || cmd == "M05") {
            bool on = cmd[1] == '3' || cmd == "M03";
            if (on != pen)
                pen_switches++;
            pen = on;
        }
        if (cmd == "G00" || cmd == "G01" || cmd == "G0" || cmd == "G1")
            for (const auto &kv : params) {
                if (kv.first == "X")
                    want_x = atof(kv.second.c_str());
                if (kv.first == "Y")
                    want_y = atof(kv.second.c_str());
            }
        if (!p.run_script(line, &err)) {
            rep.printf("line %d '%s': %s\n", lines, line.c_str(), err.c_str());
            ok = false;
            break;
        }
        if (cmd == "G28") {
#ifndef ESP_PLATFORM
            // Host memory profiling (scratch interposer), if loaded
            if (auto f = (void (*)(void))dlsym(RTLD_DEFAULT,
                                               "memprof_reset_peak"))
                f();
#endif
            min_homing = heap_monitor_stop();
            heap_monitor_start();
            heap_g28 = heap_free_bytes();
            homed_at = (now_us() - t0) * 1e-6;
            const double *c = p.toolhead().commanded_pos();
            rep.printf("G28 via homing_override done in %.2f s, position"
                       " %.3f %.3f %.3f\n", homed_at, c[0], c[1], c[2]);
        }
    }
    if (ok && !p.run_script("M400", &err)) {
        rep.printf("M400: %s\n", err.c_str());
        ok = false;
    }
    double wall = (now_us() - t0) * 1e-6;
    int64_t min_drawing = heap_monitor_stop();
    // Let the last steps reach the MCU
    double until = p.monotonic() + 0.3;
    while (p.monotonic() < until) {
        p.service();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    for (auto &r : responses)
        if (r.find("Unknown command") != std::string::npos)
            rep.printf("%s\n", r.c_str());
    const GCodeMoveState &gm = p.gcode_move();
    rep.printf("%d lines in %.1f s, %llu moves, runouts %llu\n", lines, wall,
               (unsigned long long)p.toolhead().move_count(),
               (unsigned long long)p.toolhead().runouts());
    const PrinterStats &st = p.stats();
    double busy = wall - st.wait_s;
    rep.printf("host loop: busy %.1f s (%.2f ms/line), waiting %.1f s;"
               " macro render %.1f s (%llu renders, %.2f ms each)\n", busy,
               busy * 1e3 / lines, st.wait_s, st.render_s,
               (unsigned long long)st.renders,
               st.renders ? st.render_s * 1e3 / st.renders : 0.);
    double parse_s = p.gcode().parse_s;
    rep.printf("  step flush %.1f s, toolhead.move %.1f s, clock/tick %.1f s,"
               " G-code parse %.1f s (%llu lines, %.3f ms each), other %.1f s\n",
               st.flush_s, st.move_s, st.tick_s, parse_s,
               (unsigned long long)p.gcode().parsed,
               p.gcode().parsed ? parse_s * 1e3 / p.gcode().parsed : 0.,
               busy - st.render_s - st.flush_s - st.move_s - st.tick_s - parse_s);
    rep.printf("position X %.3f Y %.3f (last G-code X %.3f Y %.3f)\n",
               gm.last_position[0], gm.last_position[1], want_x, want_y);
    bool pos_ok = std::fabs(gm.last_position[0] - want_x) < 1e-6
                  && std::fabs(gm.last_position[1] - want_y) < 1e-6;
    bool agree = true;
    for (auto &s : p.steppers()) {
        int64_t mpos;
        if (!p.query_mcu_position(*s, &mpos, &err) || mpos != s->get_mcu_position()) {
            agree = false;
            rep.printf("  %s: mcu %lld host %lld\n", s->name.c_str(),
                       (long long)mpos, (long long)s->get_mcu_position());
        }
    }
    MsgParams sp;
    int64_t edges = -1;
    if (p.mcu().serial().send_with_response("sim_get_pin pin=20", "sim_pin",
                                            &sp, &err))
        edges = sp.get("edges");
    rep.printf("pen (gpio20): %lld switches (G-code implies %d)\n",
               (long long)edges, pen_switches);
    const GenStats &gs = p.motion_queuing().gen_stats();
    rep.printf("step gen: worst %.2f ms, worst load %.1f%%\n", gs.max_us * 1e-3,
               gs.max_load * 100.);
    int64_t heap1 = heap_free_bytes();
    if (heap0 >= 0)
        rep.printf("heap free: %lld at start, %lld after printer.cfg, %lld after"
                   " connect, %lld after G28, %lld at end, min since boot %lld\n",
                   (long long)heap0, (long long)heap_cfg, (long long)heap_conn,
                   (long long)heap_g28, (long long)heap1,
                   (long long)heap_min_free_bytes());
    bool end_ok = true;
    if (!end_macro.empty()) {
        end_ok = p.run_script(end_macro + "\nM400", &err);
        rep.printf("end of file '%s': %s\n", end_macro.c_str(),
                   end_ok ? "OK" : err.c_str());
    }
    bool pass = ok && pos_ok && agree && edges == pen_switches && p.is_ready()
                && end_ok;
    if (!stages.empty())
        *report += "heap by stage:\n" + stages;
    if (min_connect >= 0)
        rep.printf("lowest free heap: %lld during connect, %lld until G28 done,"
                   " %lld while drawing\n", (long long)min_connect,
                   (long long)min_homing, (long long)min_drawing);
    debug_stage_hook = nullptr;
    if (!p.is_ready())
        rep.printf("state: %s\n", p.state_message().c_str());
    rep.printf("checks: run %s, position %s, steps %s, pen %s, ready %s\n",
               ok ? "OK" : "FAIL", pos_ok ? "OK" : "FAIL",
               agree ? "OK" : "FAIL", edges == pen_switches ? "OK" : "FAIL",
               p.is_ready() ? "OK" : "FAIL");
    rep.printf("%s\n", pass ? "M4 macros PASS" : "M4 macros FAIL");
    p.disconnect();
    return pass;
}

} // namespace kl
