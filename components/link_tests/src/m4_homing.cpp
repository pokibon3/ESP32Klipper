// M4 test: homing (G28) with TMC2209 sensorless (virtual) endstops on X/Y
// and a switch on Z, on the simulated MCU.
//
// The simulated MCU raises the DIAG (or switch) pin some time after G28
// starts, as a stalled motor would.  The test checks that the homing move
// stops, that host and MCU step positions agree afterwards, that a move
// after homing produces exactly the expected steps, and the error cases
// ("No trigger", "Must home axis first").
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "link_tests/m4_homing.hpp"

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
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

std::string
replace_all(std::string s, const std::string &from, const std::string &to)
{
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}
} // namespace

bool
run_m4_homing_test(const std::string &port, int baud, const std::string &cfg_in,
                   std::string *report)
{
    Report rep{report};
    // Z: home without the retract/second approach (one timed trigger)
    std::string cfg = replace_all(cfg_in, "homing_speed: 12",
                                  "homing_speed: 12\nhoming_retract_dist: 0");
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
    if (!p.mcu().parser().lookup_name("sim_set_input_delayed")) {
        rep.printf("needs the simulated MCU\n");
        return false;
    }
    std::vector<std::string> responses;
    p.gcode().set_responder(
        [&](const std::string &m) { responses.push_back(m); });
    auto mcu_cmd = [&](const std::string &c) {
        std::string e;
        if (!p.mcu().serial().send(c, &e))
            rep.printf("send failed: %s\n", e.c_str());
    };
    auto run = [&](const char *script, std::string *e) {
        bool ok = p.run_script(script, e);
        return ok;
    };
    auto settle = [&](double secs) {
        double until = p.monotonic() + secs;
        while (p.monotonic() < until) {
            p.service();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    };
    auto positions_agree = [&]() {
        bool ok = true;
        for (auto &s : p.steppers()) {
            int64_t mpos;
            std::string e;
            if (!p.query_mcu_position(*s, &mpos, &e))
                return false;
            if (mpos != s->get_mcu_position()) {
                rep.printf("  %s: mcu %lld, host %lld\n", s->name.c_str(),
                           (long long)mpos, (long long)s->get_mcu_position());
                ok = false;
            }
        }
        return ok;
    };
    bool all_ok = true;
    auto check = [&](const char *what, bool ok) {
        rep.printf("%-46s %s\n", what, ok ? "OK" : "FAIL");
        all_ok = all_ok && ok;
    };
    // TMC DIAG outputs are low while the motors run normally
    mcu_cmd("sim_set_input pin=4 forced=1 value=0");
    mcu_cmd("sim_set_input pin=3 forced=1 value=0");

    // 1. Sensorless X: DIAG goes high 1 s into the homing move
    mcu_cmd("sim_set_input_delayed pin=4 value=1 delay_ms=1000");
    uint64_t t0 = now_us();
    bool ok = run("G28 X", &err);
    double secs = (now_us() - t0) * 1e-6;
    rep.printf("G28 X took %.2f s%s%s\n", secs, ok ? "" : ": ",
               ok ? "" : err.c_str());
    check("G28 X (sensorless, DIAG gpio4)", ok);
    mcu_cmd("sim_set_input pin=4 forced=1 value=0");
    check("  toolhead X = 0", std::fabs(p.toolhead().commanded_pos()[0]) < 1e-9);
    check("  host/MCU step positions agree", positions_agree());

    // 2. Sensorless Y
    mcu_cmd("sim_set_input_delayed pin=3 value=1 delay_ms=1000");
    ok = run("G28 Y", &err);
    if (!ok)
        rep.printf("  error: %s\n", err.c_str());
    check("G28 Y (sensorless, DIAG gpio3)", ok);
    mcu_cmd("sim_set_input pin=3 forced=1 value=0");
    check("  host/MCU step positions agree", positions_agree());

    // 3. Z switch (^!gpio25: pulled up, pressed = low)
    mcu_cmd("sim_set_input_delayed pin=25 value=0 delay_ms=800");
    ok = run("G28 Z", &err);
    if (!ok)
        rep.printf("  error: %s\n", err.c_str());
    check("G28 Z (switch, ^!gpio25)", ok);
    mcu_cmd("sim_set_input pin=25 forced=0 value=0");

    // 4. A move after homing gives exactly the expected steps
    int64_t a0 = 0, b0 = 0, a1 = 0, b1 = 0;
    PrinterStepper &sa = *p.steppers()[0], &sb = *p.steppers()[1];
    p.query_mcu_position(sa, &a0, &err);
    p.query_mcu_position(sb, &b0, &err);
    ok = run("G90\nG1 X50 Y30 F6000\nM400", &err);
    settle(0.3);
    p.query_mcu_position(sa, &a1, &err);
    p.query_mcu_position(sb, &b1, &err);
    int64_t want_a = llround(80. / sa.step_dist), want_b = llround(20. / sb.step_dist);
    rep.printf("G1 X50 Y30: A %+lld steps (want %+lld), B %+lld (want %+lld)\n",
               (long long)(a1 - a0), (long long)want_a, (long long)(b1 - b0),
               (long long)want_b);
    check("move after homing", ok && std::llabs((a1 - a0) - want_a) <= 1
                               && std::llabs((b1 - b0) - want_b) <= 1);
    responses.clear();
    run("M114", &err);
    for (auto &r : responses)
        rep.printf("  %s\n", r.c_str());

    // 5. No trigger: error after the full move, motors off, axes unhomed
    ok = run("G28 X", &err);
    rep.printf("G28 X without trigger: %s\n", ok ? "no error" : err.c_str());
    check("\"No trigger\" error", !ok && err.find("No trigger on x") == 0);
    ok = run("G1 X10", &err);
    rep.printf("G1 after failed homing: %s\n", ok ? "no error" : err.c_str());
    check("\"Must home axis first\" error",
          !ok && err.find("Must home axis first") == 0);

    check("printer still ready", p.is_ready());
    rep.printf("%s\n", all_ok ? "M4 homing PASS" : "M4 homing FAIL");
    p.disconnect();
    return all_ok;
}

} // namespace kl
