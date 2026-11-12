// M2 test: drive one stepper axis through the real Klipper MCU protocol.
//
// Configures stepper_x like the Fraxinus0pen printer.cfg (step gpio11,
// dir gpio10, enable !gpio12), moves +distance and back by half, then
// checks the MCU's step position (stepper_get_position) and, on the
// simulated MCU, the pulse count on the step pin.
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "link_tests/m2_axis.hpp"

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "chelper_api.h"
#include "klippy_lite/motion.hpp"
#include "mcu_link/mcu.hpp"

using namespace kl;

namespace {
// Collects the test output (the ESP32 build cannot print while the
// console UART carries the MCU link)
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

static const double ROTATION_DISTANCE = 47.5, STEPS_PER_ROT = 200 * 16;
static const double MAX_STEPCOMPRESS_ERROR = 0.000025;

// Host main loop pieces: keep the MCU clock synced and the step
// generation flushed while waiting (reactor timers in klippy)
struct HostLoop {
    Mcu &mcu;
    MotionQueuing &mq;
    ToolHead &th;
    double est() const { return mcu.estimated_print_time(get_monotonic()); }
    void step() {
        mcu.tick(get_monotonic());
        mq.flush_handler(est());
    }
    // toolhead._check_pause(): wait while too far ahead of the MCU
    void throttle() {
        while (th.print_time() - est() > th.buffer_time_high && !mcu.is_shutdown()) {
            step();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        step();
    }
    void wait_until(double print_time) {
        while (est() < print_time && !mcu.is_shutdown()) {
            step();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
};

bool
kl::run_m2_axis_test(const std::string &port, int baud, double distance,
                     std::string *report)
{
    Report rep{report};
    double step_dist = ROTATION_DISTANCE / STEPS_PER_ROT;

    Mcu mcu("mcu");
    // [stepper_x] (stepper.py MCU_stepper._build_config)
    int step_oid = mcu.create_oid();
    mcu.add_config_cmd("config_stepper oid=" + std::to_string(step_oid)
                       + " step_pin=gpio11 dir_pin=gpio10 invert_step=0"
                       " step_pulse_ticks=24");
    mcu.add_config_cmd("reset_step_clock oid=" + std::to_string(step_oid)
                       + " clock=0", false, true);
    // enable_pin: !gpio12 (stepper_enable.py, MCU_digital_out)
    int en_oid = mcu.create_oid();
    mcu.add_config_cmd("config_digital_out oid=" + std::to_string(en_oid)
                       + " pin=gpio12 value=1 default_value=1 max_duration=0");

    std::string err;
    if (!mcu.connect(port, baud, &err)) {
        rep.printf("connect failed: %s\nserial stats: %s\n", err.c_str(),
                   mcu.serial().last_stats().c_str());
        return false;
    }
    rep.printf("connected: freq %.0f, move_count %d, %s\n", mcu.mcu_freq(),
           mcu.move_count(), mcu.clocksync().dump_debug().c_str());

    // Motion pipeline (motion_queuing + toolhead + one cartesian stepper)
    MotionQueuing mq;
    ToolHeadConfig thc;
    thc.max_accel = 3000.;
    ToolHead th(mq, thc);
    th.estimated_print_time = [&] {
        return mcu.estimated_print_time(get_monotonic());
    };
    syncemitter *se = mq.allocate_syncemitter("stepper_x", true);
    stepcompress *sc = syncemitter_get_stepcompress(se);
    stepcompress_fill(sc, step_oid, mcu.seconds_to_clock(MAX_STEPCOMPRESS_ERROR),
                      mcu.command_tag("queue_step oid=%c interval=%u count=%hu add=%hi"),
                      mcu.command_tag("set_next_step_dir oid=%c dir=%c"));
    stepcompress_set_invert_sdir(sc, 0);
    stepper_kinematics *sk = cartesian_stepper_alloc('x');
    itersolve_set_trapq(sk, th.get_trapq(), step_dist);
    syncemitter_set_stepper_kinematics(se, sk);
    mq.add_stepper_kinematics(sk);
    th.add_stepper_kinematics(sk);
    mq.check_step_generation_scan_windows();
    mq.setup_mcu_movequeue(mcu.move_count(), mcu.mcu_freq(),
                           mcu.serial().get_serialqueue());
    HostLoop loop{mcu, mq, th};

    // Enable the driver at the start of the motion (queue_digital_out)
    double pt = th.get_last_move_time();
    std::vector<uint8_t> cmd;
    const MsgFormat *qdo = mcu.lookup_command(
        "queue_digital_out oid=%c clock=%u on_ticks=%u", &err);
    if (!qdo) {
        rep.printf("%s\n", err.c_str());
        return false;
    }
    int64_t en_clock = mcu.print_time_to_clock(pt);
    mcu.parser().encode(*qdo, {en_oid, (uint32_t)en_clock, 0}, &cmd, &err);
    mcu.serial().raw_send(cmd, 0, en_clock, nullptr);
    // This test generates all steps in one flush at the end (no background
    // flushing), so start the motion late enough for the ESP32 to finish
    // generating before the first step is due
    th.dwell(0.300);

    // Moves: +distance at 50 mm/s, then back half way at 100 mm/s
    double pos[3] = {distance, 0., 0.};
    th.move(pos, 50.);
    loop.throttle();
    pos[0] = distance / 2.;
    th.move(pos, 100.);
    loop.throttle();
    th.flush_step_generation();
    double end_time = th.print_time();
    rep.printf("queued: motion ends at print_time %.3f (now %.3f)\n", end_time,
           loop.est());
    loop.wait_until(end_time + 0.2);

    if (mcu.is_shutdown()) {
        rep.printf("FAIL: MCU shutdown: %s\n", mcu.shutdown_msg().c_str());
        return false;
    }
    // Check the position counted by the MCU's stepper code
    MsgParams p;
    if (!mcu.serial().send_with_response(
            "stepper_get_position oid=" + std::to_string(step_oid),
            "stepper_position", &p, &err, step_oid)) {
        rep.printf("FAIL: %s\n", err.c_str());
        return false;
    }
    int64_t mcu_pos = p.get("pos");
    int64_t want = llround(distance / 2. / step_dist);
    int64_t out_steps = llround(distance / step_dist);
    int64_t total_steps = out_steps + (out_steps - want);
    rep.printf("stepper position: mcu %lld steps, expected %lld (%.3f mm)\n",
           (long long)mcu_pos, (long long)want, mcu_pos * step_dist);
    bool ok = mcu_pos == want;
    if (mcu.parser().lookup_name("sim_get_pin")) {
        MsgParams sp;
        if (mcu.serial().send_with_response("sim_get_pin pin=11", "sim_pin",
                                            &sp, &err)) {
            rep.printf("step pin edges: %lld (expected %lld = 2 x %lld steps)\n",
                   (long long)sp.get("edges"), (long long)(2 * total_steps),
                   (long long)total_steps);
            ok = ok && sp.get("edges") == 2 * total_steps;
        }
        if (mcu.serial().send_with_response("sim_get_pin pin=12", "sim_pin",
                                            &sp, &err))
            rep.printf("enable pin level: %lld (expected 0 = enabled)\n",
                   (long long)sp.get("level"));
    }
    rep.printf("serial stats: %s\n", mcu.serial().stats().c_str());
    rep.printf("%s\n", ok ? "M2 PASS" : "M2 FAIL");
    mcu.disconnect();
    return ok;
}
