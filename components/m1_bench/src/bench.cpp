// M1 benchmark runner.  See bench.hpp.
//
// The G-code front end emulates what Klipper does with the Fraxinus0pen
// printer.cfg: gcode_move G0/G1 handling, the zero padded G00/G01/M03/M05
// macros, M3/M5 (SET_PIN on [pwm_tool LASER] + G4), M02, and G28 through the
// [homing_override] macros (homing itself is replaced by set_position).
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "klippy_lite/bench.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "klippy_lite/platform.hpp"

namespace kl {

namespace {

// MCU command tags/oids are arbitrary here; they only affect encoding size.
constexpr int32_t QUEUE_STEP_TAG = 20, SET_DIR_TAG = 21;
constexpr int32_t QUEUE_DIGITAL_OUT_TAG = 22;
constexpr double MAX_STEPCOMPRESS_ERROR = 0.000025; // stepper.py

// pwm_tool.py MCU_queued_pwm (software PWM, no max_duration) +
// PrinterOutputPin._set_pin()
class PwmTool {
public:
    PwmTool(MotionQueuing &mq, syncemitter *se, uint32_t oid, double freq,
            double cycle_time)
        : mq_(mq), se_(se), oid_(oid), freq_(freq)
    {
        pwm_max_ = std::floor(cycle_time * freq + 0.5);
        last_clock_ = (uint64_t)(0.200 * freq);
    }
    void set_pin(double print_time, double value)
    {
        if (value == last_value_)
            return;
        print_time = std::max(print_time, last_print_time_);
        uint64_t clock = (uint64_t)(print_time * freq_);
        uint32_t v = (uint32_t)(std::max(0., std::min(1., value)) * pwm_max_
                                + 0.5);
        last_clock_ = clock = std::max(last_clock_, clock);
        uint32_t data[4] = {(uint32_t)QUEUE_DIGITAL_OUT_TAG, oid_,
                            (uint32_t)(clock & 0xffffffff), v};
        syncemitter_queue_msg(se_, clock, data, 4);
        mq_.note_mcu_movequeue_activity(clock / freq_, false);
        last_value_ = value;
        last_print_time_ = print_time;
        updates_++;
    }
    uint64_t updates() const { return updates_; }

private:
    MotionQueuing &mq_;
    syncemitter *se_;
    uint32_t oid_;
    double freq_, pwm_max_;
    uint64_t last_clock_;
    double last_value_ = 0., last_print_time_ = 0.;
    uint64_t updates_ = 0;
};

struct GCodeLine {
    std::string cmd;
    bool has[26] = {};
    double val[26] = {};
    bool get(char c, double &out) const
    {
        int i = c - 'A';
        if (!has[i])
            return false;
        out = val[i];
        return true;
    }
};

// Minimal Klipper-style parser: "G01 X1.0 Y2 F3000 ;comment"
bool
parse_line(const char *s, size_t n, GCodeLine &out)
{
    out = GCodeLine();
    size_t i = 0;
    auto skip_ws = [&] { while (i < n && isspace((unsigned char)s[i])) i++; };
    skip_ws();
    if (i >= n || s[i] == ';')
        return false;
    while (i < n && !isspace((unsigned char)s[i]) && s[i] != ';')
        out.cmd.push_back((char)toupper((unsigned char)s[i++]));
    for (;;) {
        skip_ws();
        if (i >= n || s[i] == ';')
            break;
        char c = (char)toupper((unsigned char)s[i++]);
        size_t start = i;
        while (i < n && !isspace((unsigned char)s[i]) && s[i] != ';')
            i++;
        if (c < 'A' || c > 'Z')
            continue;
        std::string num(s + start, i - start);
        out.has[c - 'A'] = true;
        out.val[c - 'A'] = strtod(num.c_str(), nullptr);
    }
    return !out.cmd.empty();
}

// Klipper default zv shaper (extras/shaper_defs.py get_zv_shaper)
void
zv_shaper(double freq, double damping, double A[2], double T[2])
{
    double df = std::sqrt(1. - damping * damping);
    double K = std::exp(-damping * M_PI / df);
    double t_d = 1. / (freq * df);
    A[0] = 1.;
    A[1] = K;
    T[0] = 0.;
    T[1] = .5 * t_d;
}

class Runner {
public:
    Runner(const BenchConfig &cfg) : cfg_(cfg)
    {
        mq_ = std::make_unique<MotionQueuing>(cfg.queuing);
        th_ = std::make_unique<ToolHead>(*mq_, cfg.toolhead);
        th_->estimated_print_time = [this] { return est_print_time(); };
        double step_dist = cfg.rotation_distance
            / (cfg.full_steps_per_rotation * cfg.microsteps);
        uint32_t max_error = (uint32_t)(MAX_STEPCOMPRESS_ERROR * cfg.mcu_freq);
        const char *names[3] = {"stepper_x", "stepper_y", "stepper_z"};
        for (int i = 0; i < 3; i++) {
            syncemitter *se = mq_->allocate_syncemitter(names[i], true);
            stepcompress *sc = syncemitter_get_stepcompress(se);
            stepcompress_fill(sc, i, max_error, QUEUE_STEP_TAG, SET_DIR_TAG);
            stepcompress_set_invert_sdir(sc, 0);
            stepper_kinematics *sk = i == 0 ? corexy_stepper_alloc('+')
                : i == 1 ? corexy_stepper_alloc('-')
                : cartesian_stepper_alloc('z');
            sks_.push_back(sk);
            itersolve_set_trapq(sk, th_->get_trapq(), step_dist);
            stepper_kinematics *active = sk;
            if (cfg.input_shaper && i < 2) {
                // input_shaper.py _get_input_shaper_stepper_kinematics()
                stepper_kinematics *is_sk = input_shaper_alloc();
                sks_.push_back(is_sk);
                itersolve_set_trapq(is_sk, th_->get_trapq(), step_dist);
                if (input_shaper_set_sk(is_sk, sk) == 0) {
                    double A[2], T[2];
                    zv_shaper(cfg.shaper_freq_x, cfg.damping_ratio, A, T);
                    input_shaper_set_shaper_params(is_sk, 'x', 2, A, T);
                    zv_shaper(cfg.shaper_freq_y, cfg.damping_ratio, A, T);
                    input_shaper_set_shaper_params(is_sk, 'y', 2, A, T);
                    active = is_sk;
                }
            }
            syncemitter_set_stepper_kinematics(se, active);
            mq_->add_stepper_kinematics(active);
            th_->add_stepper_kinematics(active);
        }
        syncemitter *pwm_se = mq_->allocate_syncemitter("LASER", false);
        laser_ = std::make_unique<PwmTool>(*mq_, pwm_se, 3, cfg.mcu_freq, 0.100);
        mq_->setup_mcu_movequeue(cfg.move_count, cfg.mcu_freq);
        mq_->check_step_generation_scan_windows();
    }

    ~Runner()
    {
        // Join step generation threads before freeing kinematics
        th_.reset();
        laser_.reset();
        mq_.reset();
        for (stepper_kinematics *sk : sks_)
            free(sk);
    }

    // Simulated MCU clock (mcu.estimated_print_time).  The benchmark models
    // the steady state where G-code processing is faster than the machine:
    // the host reads G-code until it is buffer_time_high ahead of the MCU,
    // then waits while the MCU clock advances.  The clock advances in small
    // steps, never jumps, so step generation runs in the same small batches
    // as on a real machine.
    double est_print_time() const { return est_; }

    void service()
    {
        const double EST_STEP = 0.010;
        for (;;) {
            while (mq_->flush_handler(est_) && !mq_->failed()) {
            }
            if (mq_->failed()
                || th_->print_time() - est_ <= th_->buffer_time_high)
                break;
            est_ += EST_STEP;
        }
    }

    void finish()
    {
        th_->flush_step_generation();
        mq_->advance_flush_time(mq_->need_flush_time() + BGFLUSH_EXTRA_TIME);
    }

    void g1(const GCodeLine &l)
    {
        static const char axes[3] = {'X', 'Y', 'Z'};
        for (int i = 0; i < 3; i++) {
            double v;
            if (l.get(axes[i], v))
                last_pos_[i] = absolute_ ? v : last_pos_[i] + v;
        }
        double f;
        if (l.get('F', f) && f > 0.)
            speed_ = f * cfg_.speed_factor / 60.;
        th_->move(last_pos_, speed_);
    }

    void g1(double x, double y, double z, double f = 0.)
    {
        GCodeLine l;
        l.cmd = "G1";
        double v[3] = {x, y, z};
        const char axes[3] = {'X', 'Y', 'Z'};
        for (int i = 0; i < 3; i++) {
            if (!std::isnan(v[i])) {
                l.has[axes[i] - 'A'] = true;
                l.val[axes[i] - 'A'] = v[i];
            }
        }
        if (f > 0.) {
            l.has['F' - 'A'] = true;
            l.val['F' - 'A'] = f;
        }
        g1(l);
    }

    void set_position(double x, double y, double z)
    {
        double p[3] = {x, y, z};
        th_->set_position(p);
        for (int i = 0; i < 3; i++)
            last_pos_[i] = p[i];
    }

    void set_laser(double value)
    {
        th_->register_lookahead_callback(
            [this, value](double pt) { laser_->set_pin(pt, value); });
    }

    // printer.cfg: [gcode_macro M3]  SET_PIN PIN=LASER VALUE=1 / G4 P10
    void m3() { set_laser(1.); th_->dwell(0.010); }
    // printer.cfg: [gcode_macro M5]  G4 P2 / SET_PIN PIN=LASER VALUE=0
    void m5() { th_->dwell(0.002); set_laser(0.); }

    void g28()
    {
        // [homing_override]: _HOME_X then _HOME_Y.  The sensorless homing
        // moves are replaced by set_position (drip moves are not measured).
        const double NaN = std::nan("");
        set_position(0., 0., 0.);              // G28 X
        absolute_ = false;                     // G91
        g1(5., NaN, NaN, 3000.);               // G1 X+5 F3000
        th_->dwell(2.0);                       // G4 P2000
        set_position(last_pos_[0], 0., 0.);    // G28 Y
        g1(NaN, 5., NaN, 3000.);               // G1 Y+5 F3000
        th_->dwell(0.1);                       // G4 P100
    }

    void m02()
    {
        // printer.cfg: [gcode_macro M02]
        const double NaN = std::nan("");
        g1(0., NaN, NaN, 3000.);
        g1(NaN, 150., NaN);
        m3();
        g1(NaN, 156., NaN, 600.);
        g1(NaN, 150., NaN);
        g1(NaN, 156., NaN);
        g1(NaN, 150., NaN);
        m5();
        // M18: motors off (no motion effect)
    }

    bool execute(const GCodeLine &l)
    {
        const std::string &c = l.cmd;
        double v;
        if (c == "G0" || c == "G1" || c == "G00" || c == "G01") {
            g1(l);
        } else if (c == "M3" || c == "M03" || c == "M4") {
            m3();
        } else if (c == "M5" || c == "M05") {
            m5();
        } else if (c == "G4" || c == "G04") {
            th_->dwell(l.get('P', v) ? v / 1000. : 0.);
        } else if (c == "G90") {
            absolute_ = true;
        } else if (c == "G91") {
            absolute_ = false;
        } else if (c == "G28") {
            g28();
        } else if (c == "M02" || c == "M2") {
            m02();
        } else if (c == "G21" || c == "M18" || c == "M84") {
            // no motion effect
        } else {
            return false;
        }
        return true;
    }

    MotionQueuing &mq() { return *mq_; }
    ToolHead &th() { return *th_; }
    PwmTool &laser() { return *laser_; }

private:
    const BenchConfig &cfg_;
    std::unique_ptr<MotionQueuing> mq_;
    std::unique_ptr<ToolHead> th_;
    std::unique_ptr<PwmTool> laser_;
    std::vector<stepper_kinematics *> sks_;
    double last_pos_[3] = {0., 0., 0.};
    bool absolute_ = true;
    double speed_ = 25.; // gcode_move.py default speed
    double est_ = 0.;
};

} // namespace

BenchResult
run_bench(const char *gcode, size_t len, const BenchConfig &cfg)
{
    BenchResult r;
    r.label = cfg.label;
    bench_sink_reset();
    r.heap_free_before = heap_free_bytes();
    int64_t heap_min = r.heap_free_before;
    heap_monitor_start();
    {
        Runner run(cfg);
        r.kin_flush_delay = run.mq().get_kin_flush_delay();
        r.heap_after_setup = heap_free_bytes();
        const bool trace = getenv("BENCH_TRACE") != nullptr;
        GCodeLine line;
        uint64_t t0 = now_us();
        size_t pos = 0;
        while (pos < len && !run.mq().failed()) {
            const char *s = gcode + pos;
            const char *nl = (const char *)memchr(s, '\n', len - pos);
            size_t n = nl ? (size_t)(nl - s) : len - pos;
            pos += n + 1;
            if (!parse_line(s, n, line))
                continue;
            r.lines++;
            if (!run.execute(line))
                r.unknown_cmds++;
            run.service();
            if (heap_min >= 0)
                heap_min = std::min(heap_min, heap_free_bytes());
            if (trace && r.lines % 250 == 0)
                fprintf(stderr, "trace line %llu heap_use %lld print_time %.3f"
                        " step_gen %.3f\n", (unsigned long long)r.lines,
                        (long long)(r.heap_free_before - heap_free_bytes()),
                        run.th().print_time(),
                        run.mq().last_step_gen_time());
        }
        run.finish();
        r.cpu_total_us = now_us() - t0;
        if (heap_min >= 0)
            heap_min = std::min(heap_min, heap_free_bytes());
        const GenStats &gs = run.mq().gen_stats();
        r.cpu_gen_us = gs.total_us;
        r.gen_calls = gs.calls;
        r.gen_max_us = gs.max_us;
        r.gen_max_load = gs.max_load;
        r.moves = run.th().move_count();
        r.lookahead_max = run.th().lookahead_max();
        r.runouts = run.th().runouts();
        r.pwm_updates = run.laser().updates();
        r.print_time = run.th().print_time();
        r.ok = !run.mq().failed();
    }
    r.heap_true_min = heap_monitor_stop();
    r.heap_min_free = heap_min;
    bench_sink_get(&r.sink);
    return r;
}

std::vector<BenchConfig>
default_suite()
{
    std::vector<BenchConfig> s(4);
    s[0].label = "x1 feed, no shaper";
    s[1].label = "x1 feed, zv shaper";
    s[1].input_shaper = true;
    s[2].label = "x2 feed, zv shaper";
    s[2].input_shaper = true;
    s[2].speed_factor = 2.;
    s[3].label = "x5 feed, zv shaper";
    s[3].input_shaper = true;
    s[3].speed_factor = 5.;
    return s;
}

void
print_result(const BenchResult &r, FILE *out)
{
    double cpu_s = r.cpu_total_us * .000001;
    double gen_s = r.cpu_gen_us * .000001;
    double plan_s = cpu_s - gen_s;
    // serialqueue frames: up to 64 bytes with 5 bytes of header/trailer
    double wire_bytes = r.sink.bytes * 64. / 59.;
    double uart_bps = r.print_time > 0. ? wire_bytes / r.print_time : 0.;
    fprintf(out, "=== %s ===\n", r.label);
    fprintf(out, "  status          : %s\n", r.ok ? "OK" : "FAILED");
    fprintf(out, "  lines/moves     : %llu lines, %llu moves, %llu unknown,"
            " %llu pen switches\n", (unsigned long long)r.lines,
            (unsigned long long)r.moves, (unsigned long long)r.unknown_cmds,
            (unsigned long long)r.pwm_updates);
    fprintf(out, "  motion time     : %.3f s\n", r.print_time);
    fprintf(out, "  cpu time        : %.3f s (plan %.3f s + step gen %.3f s)\n",
            cpu_s, plan_s, gen_s);
    fprintf(out, "  realtime ratio  : %.2fx  (>1 = faster than the machine)\n",
            r.realtime_ratio());
    fprintf(out, "  step gen calls  : %llu, worst %.2f ms, worst load %.1f%%\n",
            (unsigned long long)r.gen_calls, r.gen_max_us * .001,
            r.gen_max_load * 100.);
    fprintf(out, "  kin_flush_delay : %.4f s, lookahead max %u moves,"
            " %llu runouts\n", r.kin_flush_delay, (unsigned)r.lookahead_max,
            (unsigned long long)r.runouts);
    fprintf(out, "  mcu messages    : %llu msgs, %llu bytes, ~%.0f B/s on UART"
            " (%.1f%% of 250000 baud)\n", (unsigned long long)r.sink.messages,
            (unsigned long long)r.sink.bytes, uart_bps,
            uart_bps / 25000. * 100.);
    fprintf(out, "  output hash     : %016llx\n",
            (unsigned long long)r.sink.hash);
    if (r.heap_free_before >= 0)
        fprintf(out, "  heap            : %lld free before, %lld min during"
                " (peak use %lld bytes)\n", (long long)r.heap_free_before,
                (long long)r.heap_min_free,
                (long long)(r.heap_free_before - r.heap_min_free));
    if (r.heap_true_min >= 0)
        fprintf(out, "  heap true min   : %lld free (true peak use %lld bytes)\n",
                (long long)r.heap_true_min,
                (long long)(r.heap_free_before - r.heap_true_min));
}

} // namespace kl
