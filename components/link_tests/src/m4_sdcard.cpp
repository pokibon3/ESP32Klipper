// M4 test: printing a file from storage (virtual_sdcard) with pause,
// resume and cancel, using the existing plotter's printer.cfg on the
// simulated MCU.  The file is driven by Printer::poll() like on the
// device; commands are submitted the way a UI or API would.
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "link_tests/m4_sdcard.hpp"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "chelper_api.h"
#include "klippy/files.hpp"
#include "klippy/printer.hpp"
#include "klippy_lite/platform.hpp"
#include "link_tests/m4_ui_demo.hpp"

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
run_m4_sdcard_test(const std::string &port, int baud, const std::string &cfg,
                   const char *gcode, size_t gcode_len, std::string *report)
{
    Report rep{report};
    // Free heap at each phase boundary and the lowest value inside it
    std::string heaplog;
    auto heap_phase = [&](const char *name) {
        int64_t lo = heap_monitor_stop();
        char buf[160];
        snprintf(buf, sizeof(buf), "  %-28s now %7lld, lowest %7lld\n", name,
                 (long long)heap_free_bytes(), (long long)lo);
        heaplog += buf;
        heap_monitor_start();
    };
    heap_monitor_start();
    Printer p;
    std::string err;
    if (!p.load_config(cfg, &err)) {
        rep.printf("config error: %s\n", err.c_str());
        return false;
    }
    if (!p.connect(port, baud, &err)) {
        rep.printf("connect failed: %s\n", err.c_str());
        return false;
    }
    heap_phase("config + connect");
    auto fs = std::make_shared<MemoryFileSystem>();
    fs->add("card_opt.gcode", gcode, gcode_len);
    p.set_filesystem(fs);
    if (test_printer_hook)
        test_printer_hook(&p);
    std::vector<std::string> responses;
    p.gcode().set_responder([&](const std::string &m) {
        if (m.compare(0, 2, "!!") == 0 || m.find("Done") != std::string::npos
            || m.find("File") == 0)
            responses.push_back(m);
    });
    auto mcu_cmd = [&](const std::string &c) {
        std::string e;
        p.mcu().serial().send(c, &e);
    };
    auto arm_diag = [&]() {
        // Sensorless homing stand-in (see m4_macros.cpp)
        mcu_cmd("sim_set_input pin=4 forced=1 value=0");
        mcu_cmd("sim_set_input pin=3 forced=1 value=0");
        mcu_cmd("sim_trigger_on_steps pin=4 value=1 step_pin=11 dir_pin=10"
                " dir_value=0 dir2_pin=5 dir2_value=0 steps=1500");
        mcu_cmd("sim_trigger_on_steps pin=3 value=1 step_pin=6 dir_pin=10"
                " dir_value=0 dir2_pin=5 dir2_value=1 steps=1500");
    };
    auto poll_for = [&](double secs) {
        double until = p.monotonic() + secs;
        while (p.monotonic() < until)
            p.poll();
    };
    auto mcu_positions = [&]() {
        std::vector<int64_t> v;
        for (auto &s : p.steppers()) {
            int64_t pos = 0;
            std::string e;
            p.query_mcu_position(*s, &pos, &e);
            v.push_back(pos);
        }
        return v;
    };
    auto pen_edges = [&]() {
        MsgParams sp;
        std::string e;
        if (!p.mcu().serial().send_with_response("sim_get_pin pin=20",
                                                 "sim_pin", &sp, &e))
            return (int64_t)-1;
        return sp.get("edges");
    };
    bool all_ok = true;
    auto check = [&](const char *what, bool ok) {
        rep.printf("%-48s %s\n", what, ok ? "OK" : "FAIL");
        all_ok = all_ok && ok;
    };

    // 1. Print with a pause at 30 %
    arm_diag();
    p.submit_script("SDCARD_PRINT_FILE FILENAME=card_opt.gcode");
    uint64_t t0 = now_us();
    bool paused = false, resumed = false;
    double pause_secs = 2.0;
    while ((now_us() - t0) * 1e-6 < 400.) {
        p.poll();
        SdcardInfo sd = p.sdcard_status();
        PrintStatsInfo ps = p.print_stats();
        if (!paused && sd.progress >= 0.30) {
            heap_phase("print to 30 %");
            p.submit_script("BASE_PAUSE");
            poll_for(1.0); // let queued moves finish
            ps = p.print_stats();
            std::vector<int64_t> a = mcu_positions();
            poll_for(pause_secs);
            std::vector<int64_t> b = mcu_positions();
            rep.printf("paused at %.1f %% (byte %zu), state '%s'\n",
                       sd.progress * 100., p.sdcard_status().file_position,
                       ps.state.c_str());
            check("PAUSE: state paused", ps.state == "paused");
            check("PAUSE: no motion while paused", a == b);
            check("PAUSE: file reading stopped",
                  p.sdcard_status().file_position == sd.file_position
                  || !p.sdcard_status().is_active);
            heap_phase("pause");
            p.submit_script("BASE_RESUME");
            paused = true;
            continue;
        }
        if (paused && !resumed && ps.state == "printing")
            resumed = true;
        if (ps.state == "complete" || ps.state == "error"
            || ps.state == "cancelled")
            break;
    }
    // Let the end macro's last commands reach the MCU (skipped with
    // M4_NO_SETTLE to start the next print right away)
    if (!getenv("M4_NO_SETTLE"))
        poll_for(1.0);
    heap_phase("resume to end");
    PrintStatsInfo ps = p.print_stats();
    double wall = (now_us() - t0) * 1e-6;
    {
        const PrinterStats &st = p.stats();
        rep.printf("host: runouts %llu; busy %.1f s, waiting %.1f s (step"
                   " flush %.1f s, macro render %.1f s)\n",
                   (unsigned long long)p.toolhead().runouts(),
                   wall - st.wait_s, st.wait_s, st.flush_s, st.render_s);
    }
    rep.printf("print: state '%s' %s, total %.1f s, print_duration %.1f s,"
               " wall %.1f s\n", ps.state.c_str(), ps.message.c_str(),
               ps.total_duration, ps.print_duration, wall);
    check("RESUME: printing again", resumed);
    check("print completed", ps.state == "complete");
    check("print_duration excludes the pause",
          std::fabs(ps.total_duration - ps.print_duration - pause_secs - 1.0)
              < 0.5);
    int64_t edges = pen_edges();
    // 1750 pen switches in the file + 2 in the M02 end macro
    rep.printf("pen switches: %lld (want 1752)\n", (long long)edges);
    check("pen switches", edges == 1752);
    const GCodeMoveState &gm = p.gcode_move();
    rep.printf("final position X %.3f Y %.3f (M02 ends at X0 Y150)\n",
               gm.last_position[0], gm.last_position[1]);
    check("final position", std::fabs(gm.last_position[0]) < 1e-6
                            && std::fabs(gm.last_position[1] - 150.) < 1e-6);
    for (auto &r : responses)
        if (r.compare(0, 2, "!!") == 0)
            rep.printf("  %s\n", r.c_str());

    // 2. Start again and cancel at 10 %
    responses.clear();
    arm_diag();
    p.submit_script("SDCARD_PRINT_FILE FILENAME=card_opt.gcode");
    t0 = now_us();
    bool cancelled = false;
    while ((now_us() - t0) * 1e-6 < 120.) {
        p.poll();
        if (!p.is_ready()) {
            rep.printf("shutdown at %.1f s, progress %.1f %%: %s\n",
                       (now_us() - t0) * 1e-6,
                       p.sdcard_status().progress * 100.,
                       p.state_message().c_str());
            break;
        }
        if (!cancelled && p.sdcard_status().progress >= 0.10) {
            p.submit_script("CANCEL_PRINT");
            cancelled = true;
            poll_for(1.5);
            break;
        }
    }
    ps = p.print_stats();
    std::vector<int64_t> a = mcu_positions();
    poll_for(1.0);
    std::vector<int64_t> b = mcu_positions();
    rep.printf("cancel: state '%s', sd active %d\n", ps.state.c_str(),
               (int)p.sdcard_status().is_active);
    check("CANCEL_PRINT: state cancelled", ps.state == "cancelled");
    check("CANCEL_PRINT: motion stopped", a == b);
    check("printer still ready", p.is_ready());
    heap_phase("second print + cancel");
    rep.printf("heap by phase:\n%s", heaplog.c_str());
    heap_monitor_stop();
    rep.printf("%s\n", all_ok ? "M4 sdcard PASS" : "M4 sdcard FAIL");
    if (test_printer_hook)
        test_printer_hook(nullptr);
    p.disconnect();
    return all_ok;
}

} // namespace kl
