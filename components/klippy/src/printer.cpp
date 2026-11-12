// The printer object (see printer.hpp).
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "klippy/printer.hpp"
#include "printer_internal.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdlib>
#include <cstdio>
#include <set>
#include <thread>

namespace kl {

static constexpr double MAX_STEPCOMPRESS_ERROR = 0.000025;
static constexpr double MIN_BOTH_EDGE_DURATION = 0.000000500;
static constexpr double MIN_OPTIMIZED_BOTH_EDGE_DURATION = 0.000000150;
static constexpr double DISABLE_STALL_TIME = 0.100;
static constexpr double PRIMING_CMD_TIME = 0.100;

static std::string
fmt(const char *f, ...) __attribute__((format(printf, 1, 2)));
static std::string
fmt(const char *f, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof(buf), f, ap);
    va_end(ap);
    return buf;
}

/****************************************************************
 * pins.py
 ****************************************************************/

bool
parse_pin(const std::string &pin_desc, bool can_invert, bool can_pullup,
          PinParams *out, std::string *err)
{
    auto strip = [](std::string s) {
        size_t b = s.find_first_not_of(" \t"), e = s.find_last_not_of(" \t");
        return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
    };
    std::string desc = strip(pin_desc);
    *out = PinParams();
    if (can_pullup && !desc.empty() && (desc[0] == '^' || desc[0] == '~')) {
        out->pullup = desc[0] == '^' ? 1 : -1;
        desc = strip(desc.substr(1));
    }
    if (can_invert && !desc.empty() && desc[0] == '!') {
        out->invert = true;
        desc = strip(desc.substr(1));
    }
    size_t colon = desc.find(':');
    if (colon == std::string::npos) {
        out->pin = desc;
    } else {
        out->chip = strip(desc.substr(0, colon));
        out->pin = strip(desc.substr(colon + 1));
    }
    if (out->pin.empty() || out->pin.find_first_of("^~!: ") != std::string::npos) {
        if (err) {
            std::string fmtd = "Invalid pin description '" + pin_desc + "'\n"
                "Format is: [^~] [!] [chip_name:] pin_name";
            *err = fmtd;
        }
        return false;
    }
    return true;
}

/****************************************************************
 * Output pins (pwm_tool.py MCU_queued_pwm, soft PWM only)
 ****************************************************************/



/****************************************************************
 * Status objects for templates ("printer" in G-code macros)
 ****************************************************************/

JValue
Printer::status_object(const std::string &name)
{
    // The full objects (status.cpp) first; the rest below
    JValue more;
    if (status_more(name, &more))
        return more;
    auto list3 = [](const double *p, double e) {
        return JValue::make_list({JValue::number(p[0]), JValue::number(p[1]),
                                  JValue::number(p[2]), JValue::number(e)});
    };
    if (name == "toolhead" && toolhead_) {
        JValue d = JValue::make_dict();
        const double *c = toolhead_->commanded_pos();
        d.dict->set("position", list3(c, 0.));
        std::string homed;
        for (int i = 0; i < 3; i++)
            if (limits_[i][0] <= limits_[i][1])
                homed += (char)('x' + i);
        d.dict->set("homed_axes", JValue::str(homed));
        d.dict->set("axis_minimum", list3(axes_min_, 0.));
        d.dict->set("axis_maximum", list3(axes_max_, 0.));
        d.dict->set("max_velocity", JValue::number(toolhead_->max_velocity));
        d.dict->set("max_accel", JValue::number(toolhead_->max_accel));
        d.dict->set("square_corner_velocity",
                    JValue::number(toolhead_->square_corner_velocity));
        d.dict->set("print_time", JValue::number(toolhead_->print_time()));
        return d;
    }
    if (name == "gcode_move") {
        JValue d = JValue::make_dict();
        double gp[3];
        for (int i = 0; i < 3; i++)
            gp[i] = gm_.last_position[i] - gm_.base_position[i];
        d.dict->set("position", list3(gm_.last_position, gm_.last_position[3]));
        d.dict->set("gcode_position", list3(gp, 0.));
        d.dict->set("absolute_coordinates", JValue::boolean(gm_.absolute_coord));
        d.dict->set("absolute_extrude", JValue::boolean(gm_.absolute_extrude));
        d.dict->set("speed_factor", JValue::number(gm_.speed_factor * 60.));
        d.dict->set("speed", JValue::number(gm_.speed / gm_.speed_factor));
        return d;
    }
    for (auto &op : output_pins_) {
        if (name == "pwm_tool " + op->name || name == "output_pin " + op->name) {
            JValue d = JValue::make_dict();
            d.dict->set("value", JValue::number(op->last_value));
            return d;
        }
    }
    if (name.compare(0, 12, "gcode_macro ") == 0) {
        for (auto &m : macros_)
            if ("gcode_macro " + m->name == name) {
                JValue d = JValue::make_dict();
                *d.dict = m->variables;
                return d;
            }
    }
    return JValue::undefined(name);
}

/****************************************************************
 * PrinterStepper
 ****************************************************************/

int64_t
PrinterStepper::get_mcu_position() const
{
    double mcu_pos_dist = itersolve_get_commanded_pos(active_sk())
                          + mcu_position_offset;
    double mcu_pos = mcu_pos_dist / step_dist;
    return mcu_pos >= 0. ? (int64_t)(mcu_pos + .5) : (int64_t)(mcu_pos - .5);
}

/****************************************************************
 * Printer setup
 ****************************************************************/

Printer::Printer(const PrinterOptions &opt) : opt_(opt), mcu_("mcu")
{
    gcode_.set_monitor([this](const std::string &m) {
        for (auto &l : response_listeners_)
            l(m);
        if (m.compare(0, 3, "!! ") == 0) {
            std::lock_guard<std::mutex> g(snapshot_lock_);
            last_error_ = m.substr(3);
            error_seq_++;
            next_snapshot_ = 0.;
        }
    });
}

Printer::~Printer()
{
    disconnect();
}

double
Printer::monotonic() const
{
    return get_monotonic();
}

PrinterStepper *
Printer::lookup_stepper(const std::string &name)
{
    for (auto &s : steppers_)
        if (s->name == name)
            return s.get();
    return nullptr;
}

std::string
Printer::state_message() const
{
    if (!shutdown_msg_.empty())
        return "Shutdown: " + shutdown_msg_;
    if (mcu_.is_shutdown())
        return "MCU shutdown: " + mcu_.shutdown_msg();
    return ready_ ? "Printer is ready" : "Printer is not ready";
}

bool
Printer::load_stepper(ConfigSection sec, char kin_axis, std::string *err)
{
    auto s = std::make_unique<PrinterStepper>();
    s->name = sec.get_name();
    if (!parse_pin(sec.get("step_pin"), true, false, &s->step_pin, err)
        || !parse_pin(sec.get("dir_pin"), true, false, &s->dir_pin, err))
        return false;
    if (s->step_pin.chip != "mcu" || s->dir_pin.chip != "mcu") {
        *err = "Stepper pins must be on the main mcu (" + s->name + ")";
        return false;
    }
    // stepper.py parse_step_distance()
    s->rotation_distance = sec.getfloat("rotation_distance", 0., NAN, NAN, 0.);
    if (!sec.has("rotation_distance"))
        sec.get("rotation_distance"); // records "must be specified"
    int64_t microsteps = sec.getint("microsteps");
    int64_t full_steps = sec.getint("full_steps_per_rotation", 200, 1);
    if (full_steps % 4) {
        *err = "full_steps_per_rotation invalid in section '" + s->name + "'";
        return false;
    }
    if (sec.has("gear_ratio")) {
        *err = "gear_ratio is not supported yet (" + s->name + ")";
        return false;
    }
    double steps_per_rotation = (double)(full_steps * microsteps);
    s->step_dist = s->rotation_distance / steps_per_rotation;
    if (sec.has("step_pulse_duration"))
        s->step_pulse_duration = sec.getfloat("step_pulse_duration", 0., 0.,
                                              .001);
    // stepper_enable.py
    std::string en = sec.get("enable_pin", "");
    if (!en.empty()) {
        if (!parse_pin(en, true, false, &s->enable_pin, err))
            return false;
        s->has_enable = true;
    }
    // stepper.py PrinterRail (homing parameters are used by M4)
    s->endstop_pin = sec.get("endstop_pin", "");
    s->position_endstop = sec.getfloat("position_endstop", 0.);
    s->position_min = sec.getfloat("position_min", 0.);
    s->position_max = sec.getfloat("position_max", 0., NAN, NAN,
                                   s->position_min);
    if (!sec.has("position_max"))
        sec.get("position_max");
    if (s->position_endstop < s->position_min
        || s->position_endstop > s->position_max) {
        *err = "position_endstop in section '" + s->name
               + "' must be between position_min and position_max";
        return false;
    }
    // stepper.py PrinterRail homing parameters
    s->homing_speed = sec.getfloat("homing_speed", 5., NAN, NAN, 0.);
    s->second_homing_speed = sec.getfloat("second_homing_speed",
                                          s->homing_speed / 2., NAN, NAN, 0.);
    s->homing_retract_speed = sec.getfloat("homing_retract_speed",
                                           s->homing_speed, NAN, NAN, 0.);
    s->homing_retract_dist = sec.getfloat("homing_retract_dist", 5., 0.);
    if (sec.has("homing_positive_dir")) {
        s->homing_positive_dir = sec.getboolean("homing_positive_dir", false);
    } else {
        double axis_len = s->position_max - s->position_min;
        if (s->position_endstop <= s->position_min + axis_len / 4.)
            s->homing_positive_dir = false;
        else if (s->position_endstop >= s->position_max - axis_len / 4.)
            s->homing_positive_dir = true;
        else {
            *err = "Unable to infer homing_positive_dir in section '"
                   + s->name + "'";
            return false;
        }
    }

    PrinterStepper *sp = s.get();
    // Motion objects (MCU_stepper.__init__ / setup_itersolve)
    sp->se = mq_->allocate_syncemitter(sp->name.c_str(), true);
    sp->sc = syncemitter_get_stepcompress(sp->se);
    stepcompress_set_invert_sdir(sp->sc, sp->dir_pin.invert);
    if (kinematics_ == "corexy" && kin_axis != 'z')
        sp->kin_sk = corexy_stepper_alloc(kin_axis == 'x' ? '+' : '-');
    else
        sp->kin_sk = cartesian_stepper_alloc(kin_axis);
    sp->oid = mcu_.create_oid();
    mcu_.register_config_callback([this, sp] { build_stepper_config(*sp); });
    if (sp->has_enable) {
        sp->enable_oid = mcu_.create_oid();
        reserved_move_slots_++; // mcu.request_move_queue_slot()
        int inv = sp->enable_pin.invert;
        mcu_.add_config_cmd(fmt("config_digital_out oid=%d pin=%s value=%d"
                                " default_value=%d max_duration=0",
                                sp->enable_oid, sp->enable_pin.pin.c_str(),
                                inv, inv));
    }
    steppers_.push_back(std::move(s));
    return true;
}

void
Printer::build_stepper_config(PrinterStepper &s)
{
    // stepper.py MCU_stepper._build_config()
    if (s.step_pulse_duration < 0.)
        s.step_pulse_duration = .000002;
    int invert_step = s.step_pin.invert;
    const MessageParser &mp = mcu_.parser();
    int64_t ssbe = mp.get_constant_int("STEPPER_STEP_BOTH_EDGE", 0);
    int64_t sbe = mp.get_constant_int("STEPPER_BOTH_EDGE", 0);
    int64_t sou = mp.get_constant_int("STEPPER_OPTIMIZED_UNSTEP", 0);
    bool want_both_edges = s.req_step_both_edge;
    if (s.step_pulse_duration > MIN_BOTH_EDGE_DURATION)
        want_both_edges = false;
    else if (sbe && s.step_pulse_duration > MIN_OPTIMIZED_BOTH_EDGE_DURATION)
        want_both_edges = false;
    else if (!sbe && !ssbe)
        want_both_edges = false;
    else if (sou)
        want_both_edges = false;
    if (want_both_edges) {
        s.step_both_edge = true;
        invert_step = -1;
        if (sbe)
            s.step_pulse_duration = 0.;
    }
    int64_t step_pulse_ticks = mcu_.seconds_to_clock(s.step_pulse_duration);
    mcu_.add_config_cmd(fmt("config_stepper oid=%d step_pin=%s dir_pin=%s"
                            " invert_step=%d step_pulse_ticks=%u", s.oid,
                            s.step_pin.pin.c_str(), s.dir_pin.pin.c_str(),
                            invert_step, (unsigned)step_pulse_ticks));
    mcu_.add_config_cmd(fmt("reset_step_clock oid=%d clock=0", s.oid), false,
                        true);
    s.reset_cmd_tag = mcu_.command_tag("reset_step_clock oid=%c clock=%u");
    stepcompress_fill(s.sc, s.oid,
                      (uint32_t)mcu_.seconds_to_clock(MAX_STEPCOMPRESS_ERROR),
                      mcu_.command_tag("queue_step oid=%c interval=%u count=%hu"
                                       " add=%hi"),
                      mcu_.command_tag("set_next_step_dir oid=%c dir=%c"));
}

bool
Printer::load_output_pin(ConfigSection sec, bool pwm_tool, std::string *err)
{
    // pwm_tool.py PrinterOutputPin (soft PWM through the motion queue)
    auto op = std::make_unique<OutputPin>();
    op->name = sec.get_name().substr(sec.get_name().find(' ') + 1);
    op->pwm_tool = pwm_tool;
    if (!parse_pin(sec.get("pin"), true, false, &op->pin, err))
        return false;
    if (sec.getboolean("hardware_pwm", false)) {
        *err = "hardware_pwm is not supported yet (" + sec.get_name() + ")";
        return false;
    }
    op->cycle_time = sec.getfloat("cycle_time", 0.100, NAN, 3., 0.);
    op->scale = sec.getfloat("scale", 1., NAN, NAN, 0.);
    op->max_duration = sec.getfloat("maximum_mcu_duration", 0., 0., 3.);
    if (op->max_duration > 0. && op->max_duration < 0.5) {
        *err = "maximum_mcu_duration must be at least 0.5 or 0";
        return false;
    }
    op->last_value = sec.getfloat("value", 0., 0., op->scale) / op->scale;
    double shutdown = sec.getfloat("shutdown_value", 0., 0., op->scale)
                      / op->scale;
    if (!pwm_tool)
        sec.getboolean("pwm", false);
    double start = op->last_value;
    if (op->pin.invert) {
        start = 1. - start;
        shutdown = 1. - shutdown;
    }
    op->start_value = std::max(0., std::min(1., start));
    op->shutdown_value = std::max(0., std::min(1., shutdown));
    if (op->max_duration > 0. && op->start_value != op->shutdown_value) {
        *err = "Pin with max duration must have start value equal to"
               " shutdown value";
        return false;
    }
    if (op->shutdown_value != 0. && op->shutdown_value != 1.) {
        *err = "shutdown value must be 0.0 or 1.0 on soft pwm";
        return false;
    }
    op->oid = mcu_.create_oid();
    op->se = mq_->allocate_syncemitter(sec.get_name().c_str(), false);
    OutputPin *p = op.get();
    mcu_.register_config_callback([this, p] {
        // MCU_queued_pwm._build_config()
        double pt = mcu_.estimated_print_time(monotonic());
        p->last_clock = mcu_.print_time_to_clock(pt + 0.200);
        int64_t cycle_ticks = mcu_.seconds_to_clock(p->cycle_time);
        int64_t duration_ticks = mcu_.seconds_to_clock(p->max_duration);
        mcu_.add_config_cmd(fmt("config_digital_out oid=%d pin=%s value=%d"
                                " default_value=%d max_duration=%d", p->oid,
                                p->pin.pin.c_str(), p->start_value >= 1.,
                                p->shutdown_value >= .5, (int)duration_ticks));
        p->default_value = (int64_t)(p->shutdown_value >= .5) * cycle_ticks;
        mcu_.add_config_cmd(fmt("set_digital_out_pwm_cycle oid=%d"
                                " cycle_ticks=%d", p->oid, (int)cycle_ticks));
        p->pwm_max = (double)cycle_ticks;
        p->last_pwm = (int64_t)(p->start_value * p->pwm_max + .5);
        mcu_.add_config_cmd(fmt("queue_digital_out oid=%d clock=%u on_ticks=%d",
                                p->oid, (unsigned)(p->last_clock & 0xffffffff),
                                (int)p->last_pwm), true);
        p->set_cmd_tag = mcu_.command_tag(
            "queue_digital_out oid=%c clock=%u on_ticks=%u");
    });
    gcode_.register_mux_command(
        "SET_PIN", "PIN", op->name,
        [this, p](GCodeCommand &g, std::string *err) {
            double value;
            if (!g.get_float("VALUE", &value, err, nullptr, 0., p->scale))
                return false;
            value /= p->scale;
            toolhead_->register_lookahead_callback([this, p, value](double pt) {
                if (value == p->last_value)
                    return;
                pt = std::max(pt, p->last_print_time);
                // MCU_queued_pwm.set_pwm()
                uint64_t clock = (uint64_t)mcu_.print_time_to_clock(pt);
                double v = p->pin.invert ? 1. - value : value;
                int64_t pwm = (int64_t)(std::max(0., std::min(1., v))
                                        * p->pwm_max + .5);
                p->last_clock = clock = std::max(p->last_clock, clock);
                p->last_pwm = pwm;
                uint32_t data[4] = {(uint32_t)p->set_cmd_tag, (uint32_t)p->oid,
                                    (uint32_t)(clock & 0xffffffff),
                                    (uint32_t)pwm};
                syncemitter_queue_msg(p->se, clock, data, 4);
                mq_->note_mcu_movequeue_activity(
                    clock / mcu_.mcu_freq(), false);
                p->last_value = value;
                p->last_print_time = pt;
            });
            return true;
        },
        "Set the value of an output pin");
    output_pins_.push_back(std::move(op));
    return true;
}

bool
Printer::calc_shaper(ShaperParams &p, std::string *err)
{
    // extras/shaper_defs.py
    double df = std::sqrt(1. - p.damping * p.damping);
    double t_d = 1. / (p.freq * df);
    if (p.type == "zv") {
        double K = std::exp(-p.damping * M_PI / df);
        p.n = 2;
        p.A[0] = 1.; p.A[1] = K;
        p.T[0] = 0.; p.T[1] = .5 * t_d;
    } else if (p.type == "zvd") {
        double K = std::exp(-p.damping * M_PI / df);
        p.n = 3;
        p.A[0] = 1.; p.A[1] = 2. * K; p.A[2] = K * K;
        p.T[0] = 0.; p.T[1] = .5 * t_d; p.T[2] = t_d;
    } else if (p.type == "mzv") {
        // get_mzv_shaper(n=3, t=0.75): least squares solve of M A = F
        const int n = 3;
        const double t = 0.75;
        double tau = t * (n - 2.) / (n - 2. * t - 1.);
        double T[n], M[5][n], F[5] = {1., 0., 0., 0., 0.};
        for (int i = 0; i < n; i++)
            T[i] = i * t / (n - 1.);
        for (int j = 0; j < n; j++)
            M[0][j] = 1.;
        for (int i = 0; i < n - 1; i++)
            for (int j = 0; j < n; j++) {
                double w = 2. * M_PI * (1. + i / tau) * T[j];
                M[1 + 2 * i][j] = std::cos(w);
                M[2 + 2 * i][j] = std::sin(w);
            }
        // Normal equations (M^T M) A = M^T F
        double N[n][n + 1] = {};
        for (int a = 0; a < n; a++) {
            for (int b = 0; b < n; b++)
                for (int r = 0; r < 5; r++)
                    N[a][b] += M[r][a] * M[r][b];
            for (int r = 0; r < 5; r++)
                N[a][n] += M[r][a] * F[r];
        }
        for (int c = 0; c < n; c++) {
            int piv = c;
            for (int r = c + 1; r < n; r++)
                if (std::fabs(N[r][c]) > std::fabs(N[piv][c]))
                    piv = r;
            for (int k = 0; k <= n; k++)
                std::swap(N[c][k], N[piv][k]);
            for (int r = 0; r < n; r++) {
                if (r == c)
                    continue;
                double f = N[r][c] / N[c][c];
                for (int k = c; k <= n; k++)
                    N[r][k] -= f * N[c][k];
            }
        }
        double K = std::exp(-2. * t * p.damping * M_PI / ((n - 1.) * df));
        double Kp = K;
        p.n = n;
        for (int i = 0; i < n; i++) {
            p.A[i] = N[i][n] / N[i][i];
            p.T[i] = T[i];
        }
        for (int i = 1; i < n; i++) {
            p.T[i] *= t_d;
            p.A[i] *= Kp;
            Kp *= K;
        }
    } else if (p.type == "ei") {
        const double v_tol = 1. / 20., dr = p.damping;
        double a1 = (0.24968 + 0.24961 * v_tol)
            + ((0.80008 + 1.23328 * v_tol) + (0.49599 + 3.17316 * v_tol) * dr) * dr;
        double a3 = (0.25149 + 0.21474 * v_tol)
            + ((-0.83249 + 1.41498 * v_tol) + (0.85181 - 4.90094 * v_tol) * dr) * dr;
        double a2 = 1. - a1 - a3;
        double t2 = 0.4999 + (((0.46159 + 8.57843 * v_tol) * v_tol)
                              + (((4.26169 - 108.644 * v_tol) * v_tol)
                                 + ((1.75601 + 336.989 * v_tol) * v_tol) * dr)
                                * dr) * dr;
        p.n = 3;
        p.A[0] = a1; p.A[1] = a2; p.A[2] = a3;
        p.T[0] = 0.; p.T[1] = t2 * t_d; p.T[2] = t_d;
    } else {
        *err = "Unsupported shaper type: " + p.type;
        return false;
    }
    return true;
}

bool
Printer::load_input_shaper(ConfigSection sec, std::string *err)
{
    // extras/input_shaper.py InputShaperParams
    std::string def_type = sec.get("shaper_type", "mzv");
    const char *axes[2] = {"x", "y"};
    for (int i = 0; i < 2; i++) {
        ShaperParams &p = shaper_[i];
        std::string ax = axes[i];
        p.type = sec.get("shaper_type_" + ax, def_type);
        p.damping = sec.getfloat("damping_ratio_" + ax, 0.1, 0., 0.99);
        p.freq = sec.getfloat("shaper_freq_" + ax, 0., 0.);
        if (p.freq > 0. && !calc_shaper(p, err))
            return false;
        if (p.freq > 0.)
            have_shaper_ = true;
    }
    return true;
}

void
Printer::apply_input_shaper()
{
    // input_shaper.py _get_input_shaper_stepper_kinematics()
    if (!have_shaper_)
        return;
    for (auto &s : steppers_) {
        stepper_kinematics *is_sk = input_shaper_alloc();
        if (input_shaper_set_sk(is_sk, s->kin_sk) < 0) {
            free(is_sk);
            continue; // stepper not moved by x or y
        }
        s->shaper_sk = is_sk;
        for (int i = 0; i < 2; i++) {
            ShaperParams &p = shaper_[i];
            input_shaper_set_shaper_params(is_sk, i ? 'y' : 'x',
                                           p.freq > 0. ? p.n : 0, p.A, p.T);
        }
    }
}

bool
Printer::load_config(const std::string &text, std::string *err,
                     const std::string &filename)
{
    debug_stage("load_config start");
    if (!config_.load(text, filename)) {
        *err = config_.error();
        return false;
    }
    debug_stage("printer.cfg parsed");
    // [printer] (toolhead.py, kinematics)
    ConfigSection pc = config_.getsection("printer");
    if (!config_.has_section("printer")) {
        *err = "Section 'printer' not found";
        return false;
    }
    kinematics_ = pc.get("kinematics");
    if (kinematics_ != "corexy" && kinematics_ != "cartesian") {
        *err = "Kinematics '" + kinematics_ + "' is not supported"
               " (corexy, cartesian)";
        return false;
    }
    ToolHeadConfig thc;
    thc.max_velocity = pc.getfloat("max_velocity", 0., NAN, NAN, 0.);
    if (!pc.has("max_velocity"))
        pc.get("max_velocity");
    thc.max_accel = pc.getfloat("max_accel", 0., NAN, NAN, 0.);
    if (!pc.has("max_accel"))
        pc.get("max_accel");
    thc.minimum_cruise_ratio = pc.getfloat("minimum_cruise_ratio", 0.5, 0.,
                                           NAN, NAN, 1.);
    thc.square_corner_velocity = pc.getfloat("square_corner_velocity", 5., 0.);
    thc.buffer_time_high = opt_.buffer_time_high;
    thc.lookahead_max_moves = opt_.lookahead_max_moves;
    max_z_velocity_ = pc.getfloat("max_z_velocity", thc.max_velocity, NAN,
                                  thc.max_velocity, 0.);
    max_z_accel_ = pc.getfloat("max_z_accel", thc.max_accel, NAN,
                               thc.max_accel, 0.);
    if (config_.has_error()) {
        *err = config_.error();
        return false;
    }
    mq_ = std::make_unique<MotionQueuing>(opt_.queuing);
    toolhead_ = std::make_unique<ToolHead>(*mq_, thc);

    // Steppers and kinematics
    const char *axes = "xyz";
    for (int i = 0; i < 3; i++) {
        std::string name = std::string("stepper_") + axes[i];
        if (!config_.has_section(name)) {
            *err = "Section '" + name + "' not found";
            return false;
        }
        if (!load_stepper(config_.getsection(name), axes[i], err))
            return false;
        PrinterStepper &s = *steppers_.back();
        axes_min_[i] = s.position_min;
        axes_max_[i] = s.position_max;
    }
    debug_stage("toolhead + steppers");
    std::set<std::string> handled = {"printer", "mcu", "stepper_x",
                                      "stepper_y", "stepper_z"};
    // [mcu] (serial port is chosen by the caller)
    ConfigSection mc = config_.getsection("mcu");
    mcu_serial_ = mc.get("serial", "");
    // Strip a trailing comment ("uart:2:27:22    # CYD ...")
    mcu_serial_ = mcu_serial_.substr(0, mcu_serial_.find_first_of(" \t#;"));
    mcu_baud_ = (int)mc.getint("baud", 250000);
    mc.get("restart_method", "");
    for (const auto &name : config_.sections()) {
        if (name.compare(0, 9, "pwm_tool ") == 0
            || name.compare(0, 11, "output_pin ") == 0) {
            if (!load_output_pin(config_.getsection(name),
                                 name[0] == 'p', err))
                return false;
            handled.insert(name);
        } else if (name.compare(0, 8, "tmc2209 ") == 0) {
            std::string sname = name.substr(8);
            PrinterStepper *st = lookup_stepper(sname);
            if (!st || !config_.has_section(sname)) {
                *err = "Could not find config section '[" + sname
                       + "]' required by tmc driver";
                return false;
            }
            Tmc2209 *t = Tmc2209::create(config_.getsection(name),
                                         config_.getsection(sname), mcu_,
                                         tmc_buses_, tmc_bus_owned_, err);
            if (!t)
                return false;
            tmcs_.emplace_back(t);
            // tmc.py _handle_mcu_identify(): 100ns pulses, both edges
            if (st->step_pulse_duration < 0.)
                st->step_pulse_duration = .000000100;
            st->req_step_both_edge = true;
            handled.insert(name);
        } else if (name == "input_shaper") {
            if (!load_input_shaper(config_.getsection(name), err))
                return false;
            handled.insert(name);
        }
    }
    if (config_.has_error()) {
        *err = config_.error();
        return false;
    }
    debug_stage("tmc, output pins, input shaper");
    if (!setup_endstops(err))
        return false;
    apply_input_shaper();
    // Register stepper kinematics with the toolhead / motion queuing
    trapq *tq = toolhead_->get_trapq();
    for (auto &s : steppers_) {
        stepper_kinematics *sk = s->active_sk();
        itersolve_set_trapq(sk, tq, s->step_dist);
        syncemitter_set_stepper_kinematics(s->se, sk);
        mq_->add_stepper_kinematics(sk);
        toolhead_->add_stepper_kinematics(sk);
    }
    mq_->check_step_generation_scan_windows();
    setup_toolhead_hooks();
    register_commands();
    if (!load_sdcard(&handled, err))
        return false;
    debug_stage("endstops, G-code commands");
    if (!load_macros(&handled, err))
        return false;
    debug_stage("gcode_macro, homing_override");
    for (auto &t : tmcs_)
        t->register_commands(gcode_, [this] {
            return toolhead_->get_last_move_time();
        });

    // Report what is not supported (yet)
    for (const auto &name : config_.sections())
        if (!handled.count(name))
            warnings_.push_back("section [" + name + "] is not supported yet");
    for (const auto &opt : config_.unused_options()) {
        std::string sec = opt.substr(1, opt.find(']') - 1);
        if (handled.count(sec))
            warnings_.push_back("option " + opt + " is not supported");
    }
    if (opt_.keep_config) {
        for (const auto &name : config_.sections()) {
            const auto *d = config_.section_data(name);
            std::vector<std::pair<std::string, std::string>> opts;
            if (d)
                for (const auto &kv : *d)
                    opts.emplace_back(kv.first, kv.second);
            config_copy_.emplace_back(name, std::move(opts));
        }
    }
    // The parsed config text is not needed any more (macros keep their
    // compiled templates); free it - mostly macro source on a plotter.
    config_ = ConfigFile();
    debug_stage("config text freed");
    return true;
}

void
Printer::setup_toolhead_hooks()
{
    gcode_.clock = [this] { return monotonic(); };
    toolhead_->estimated_print_time = [this] {
        return mcu_.estimated_print_time(monotonic());
    };
    toolhead_->check_move = [this](Move &m, std::string *err) {
        return check_move(m, err);
    };
    toolhead_->kin_set_position = [this](const double *pos) {
        // stepper.py MCU_stepper.set_position()
        for (auto &s : steppers_) {
            int64_t mcu_pos = s->get_mcu_position();
            itersolve_set_position(s->active_sk(), pos[0], pos[1], pos[2]);
            s->mcu_position_offset = mcu_pos * s->step_dist
                - itersolve_get_commanded_pos(s->active_sk());
        }
    };
    mq_->estimated_print_time = [this] {
        return mcu_.estimated_print_time(monotonic());
    };
    mq_->pause = [this](double s) { pause(s); };
}

/****************************************************************
 * Connection
 ****************************************************************/

bool
Printer::connect(const std::string &port, int baud, std::string *err)
{
    if (!toolhead_) {
        *err = "No configuration loaded";
        return false;
    }
    mcu_.set_shutdown_callback([this](const std::string &msg) {
        ready_ = false;
        fprintf(stderr, "Printer shutdown: %s\n", msg.c_str());
    });
    if (!mcu_.connect(port, baud, err))
        return false;
    // mcu.py MCU._connect(): steppersync with the MCU's move queue
    int move_count = mcu_.move_count() - reserved_move_slots_;
    if (move_count <= 0) {
        *err = "Too few moves available on MCU";
        return false;
    }
    mq_->setup_mcu_movequeue(move_count, mcu_.mcu_freq(),
                             mcu_.serial().get_serialqueue());
    debug_stage("move queue setup");
    // stepper.py _query_mcu_position(): sync the step position offsets
    for (auto &s : steppers_)
        if (!sync_mcu_position(*s, err))
            return false;
    // tmc.py TMCCommandHelper._handle_connect()
    for (auto &t : tmcs_) {
        PrinterStepper *st = lookup_stepper(t->stepper_name());
        std::string log;
        t->handle_connect(st && st->step_both_edge, &log);
        if (!log.empty())
            warnings_.push_back(log);
    }
    debug_stage("TMC init");
    shutdown_msg_.clear();
    ready_ = true;
    // Keep step flushing and clock sync going while this task waits for
    // MCU responses (queries, TMC register access)
    mcu_.serial().set_wait_hook([this] {
        double now = monotonic();
        mcu_.tick(now);
        if (ready_ && !mq_->drip_active() && !in_flush_) {
            in_flush_ = true;
            mq_->flush_handler(mcu_.estimated_print_time(now));
            in_flush_ = false;
        }
    });
    if (!tmcs_.empty()) {
        tmc_thread_stop_ = false;
        tmc_thread_ = std::thread([this] { tmc_check_thread(); });
    }
    return true;
}

void
Printer::invoke_shutdown(const std::string &msg)
{
    if (!ready_)
        return;
    ready_ = false;
    shutdown_msg_ = msg;
    fprintf(stderr, "Printer shutdown: %s\n", msg.c_str());
    mcu_.emergency_stop();
}

void
Printer::tmc_check_thread()
{
    while (!tmc_thread_stop_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        for (auto &t : tmcs_) {
            std::string err;
            if (!t->periodic_check(monotonic(), &err)) {
                std::lock_guard<std::mutex> g(tmc_error_lock_);
                tmc_error_msg_ = err;
                tmc_error_ = true;
            }
        }
    }
}

void
Printer::disconnect()
{
    tmc_thread_stop_ = true;
    if (tmc_thread_.joinable())
        tmc_thread_.join();
    ready_ = false;
    mcu_.disconnect();
}

bool
Printer::query_mcu_position(PrinterStepper &s, int64_t *pos, std::string *err)
{
    MsgParams p;
    if (!mcu_.serial().send_with_response(
            "stepper_get_position oid=" + std::to_string(s.oid),
            "stepper_position", &p, err, s.oid))
        return false;
    *pos = p.get("pos");
    return true;
}

/****************************************************************
 * Host loop
 ****************************************************************/

void
Printer::service()
{
    double now = monotonic();
    mcu_.tick(now);
    if (now >= next_snapshot_)
        publish_snapshot(now);
    run_calls();
    double t1 = monotonic();
    stats_.tick_s += t1 - now;
    if (!ready_ || !toolhead_ || mq_->drip_active() || in_flush_)
        return;
    in_flush_ = true;
    mq_->flush_handler(mcu_.estimated_print_time(now));
    in_flush_ = false;
    stats_.flush_s += monotonic() - t1;
    // TMC drivers: init on motor enable, then periodic error checks
    if (tmc_enable_pending_) {
        tmc_enable_pending_ = false;
        for (auto &t : tmcs_) {
            std::string err;
            if (!t->do_enable(&err)) {
                invoke_shutdown(err);
                return;
            }
        }
    }
    if (tmc_error_) {
        std::string msg;
        {
            std::lock_guard<std::mutex> g(tmc_error_lock_);
            msg = tmc_error_msg_;
        }
        tmc_error_ = false;
        invoke_shutdown(msg);
        return;
    }
    // toolhead._priming_handler(): start moving when input pauses
    if (!toolhead_->lookahead_empty() && !toolhead_->in_main_state()
        && now - last_move_submit_ > PRIMING_CMD_TIME)
        toolhead_->flush_lookahead(true);
}

void
Printer::publish_snapshot(double now)
{
    next_snapshot_ = now + 0.2;
    PrinterSnapshot s;
    s.ready = is_ready();
    s.state_message = state_message();
    s.print = print_stats();
    SdcardInfo sd = sdcard_status();
    s.progress = sd.progress;
    s.sd_active = sd.is_active;
    if (toolhead_) {
        const double *c = toolhead_->commanded_pos();
        for (int i = 0; i < 3; i++)
            s.live_pos[i] = c[i];
        // motion_report.get_trapq_position()
        if (ready_) {
            pull_move m;
            double pt = mcu_.estimated_print_time(now);
            if (trapq_extract_old(toolhead_->get_trapq(), &m, 1, 0., pt)) {
                double rel = pt - m.print_time;
                double mt = std::max(0., std::min(m.move_t, rel));
                double dist = (m.start_v + .5 * m.accel * mt) * mt;
                s.live_pos[0] = m.start_x + m.x_r * dist;
                s.live_pos[1] = m.start_y + m.y_r * dist;
                s.live_pos[2] = m.start_z + m.z_r * dist;
                if (rel >= 0. && rel <= m.move_t)
                    s.live_velocity = m.start_v + m.accel * mt;
            }
        }
    }
    for (int i = 0; i < 3; i++) {
        s.gcode_pos[i] = gm_.last_position[i] - gm_.base_position[i];
        s.axis_min[i] = axes_min_[i];
        s.axis_max[i] = axes_max_[i];
        if (limits_[i][0] <= limits_[i][1])
            s.homed_axes += (char)('x' + i);
    }
    if (!output_pins_.empty()) {
        s.pen_pin = output_pins_[0]->name;
        s.pen_value = output_pins_[0]->last_value;
    }
    s.speed_factor = gm_.speed_factor * 6000.;
    s.busy = running_cmd_;
    {
        std::lock_guard<std::mutex> g(pending_lock_);
        s.queued = pending_.size();
    }
    std::lock_guard<std::mutex> g(snapshot_lock_);
    s.last_error = last_error_;
    s.error_seq = error_seq_;
    s.seq = snapshot_.seq + 1;
    snapshot_ = std::move(s);
}

bool
Printer::reset_mcu()
{
    return mcu_.reset();
}

PrinterSnapshot
Printer::snapshot()
{
    std::lock_guard<std::mutex> g(snapshot_lock_);
    return snapshot_;
}

void
Printer::throttle()
{
    // toolhead._check_pause(): don't run too far ahead of the MCU
    for (;;) {
        service();
        double est = mcu_.estimated_print_time(monotonic());
        if (!ready_ || toolhead_->print_time() - est <= opt_.buffer_time_high)
            break;
        sleep_s(0.002);
    }
}

void
Printer::sleep_s(double s)
{
    double t0 = monotonic();
    std::this_thread::sleep_for(std::chrono::microseconds((int64_t)(s * 1e6)));
    stats_.wait_s += monotonic() - t0;
}

void
Printer::pause(double seconds)
{
    // reactor.pause(): other timers keep running, in particular the
    // background step flush (await_flush_time() before a drip move can wait
    // up to buffer_time_high while earlier moves still need their steps)
    double end = monotonic() + seconds;
    for (;;) {
        service();
        double now = monotonic();
        if (now >= end)
            return;
        double left = end - now;
        sleep_s(std::min(left, 0.002));
    }
}

void
Printer::wait_moves()
{
    toolhead_->flush_lookahead();
    while (ready_
           && toolhead_->print_time() >= mcu_.estimated_print_time(monotonic())) {
        service();
        sleep_s(0.005);
    }
}

/****************************************************************
 * Kinematics (corexy.py / cartesian.py)
 ****************************************************************/

bool
Printer::check_move(Move &m, std::string *err)
{
    auto move_error = [&](const char *msg) {
        if (err)
            *err = fmt("%s: %.3f %.3f %.3f [%.3f]", msg, m.end_pos[0],
                       m.end_pos[1], m.end_pos[2], 0.);
        return false;
    };
    auto check_endstops = [&]() {
        for (int i = 0; i < 3; i++) {
            if (m.axes_d[i] != 0. && (m.end_pos[i] < limits_[i][0]
                                      || m.end_pos[i] > limits_[i][1])) {
                if (limits_[i][0] > limits_[i][1])
                    return move_error("Must home axis first");
                return move_error("Move out of range");
            }
        }
        return true;
    };
    double xpos = m.end_pos[0], ypos = m.end_pos[1];
    if (xpos < limits_[0][0] || xpos > limits_[0][1] || ypos < limits_[1][0]
        || ypos > limits_[1][1])
        if (!check_endstops())
            return false;
    if (m.axes_d[2] == 0.)
        return true;
    if (!check_endstops())
        return false;
    double z_ratio = m.move_d / std::fabs(m.axes_d[2]);
    m.limit_speed(max_z_velocity_ * z_ratio, max_z_accel_ * z_ratio);
    return true;
}

void
Printer::set_position(const double pos[3], const char *homing_axes)
{
    toolhead_->set_position(pos);
    for (const char *a = homing_axes; *a; a++) {
        int i = *a - 'x';
        if (i >= 0 && i < 3) {
            limits_[i][0] = axes_min_[i];
            limits_[i][1] = axes_max_[i];
        }
    }
    // gcode_move.reset_last_position()
    for (int i = 0; i < 3; i++)
        gm_.last_position[i] = pos[i];
}

/****************************************************************
 * Stepper enable (stepper_enable.py)
 ****************************************************************/

void
Printer::send_enable(PrinterStepper &s, double print_time, bool on)
{
    s.enabled = on;
    if (!s.has_enable)
        return;
    // MCU_digital_out.set_digital()
    uint64_t clock = (uint64_t)mcu_.print_time_to_clock(print_time);
    const MsgFormat *f = mcu_.lookup_command(
        "queue_digital_out oid=%c clock=%u on_ticks=%u");
    std::vector<uint8_t> cmd;
    std::string err;
    if (!f || !mcu_.parser().encode(*f, {s.enable_oid,
                                         (uint32_t)(clock & 0xffffffff),
                                         (int)(on ^ s.enable_pin.invert)},
                                    &cmd, &err))
        return;
    if (getenv("KL_TRACE_SD"))
        fprintf(stderr, "enable %s %d at pt %.3f (est %.3f)\n", s.name.c_str(),
                (int)on, print_time, mcu_.estimated_print_time(monotonic()));
    mcu_.serial().raw_send(cmd, s.enable_last_clock, clock, nullptr);
    s.enable_last_clock = clock;
}

void
Printer::motor_enable(double print_time)
{
    bool any = false;
    for (auto &s : steppers_)
        if (!s->enabled) {
            send_enable(*s, print_time, true);
            any = true;
        }
    if (any && !tmcs_.empty())
        tmc_enable_pending_ = true;
}

void
Printer::motor_off(std::string *err)
{
    (void)err;
    toolhead_->dwell(DISABLE_STALL_TIME);
    double pt = toolhead_->get_last_move_time();
    for (auto &s : steppers_)
        if (s->enabled)
            send_enable(*s, pt, false);
    for (auto &t : tmcs_)
        t->do_disable();
    // kinematics clear_homing_state("xyz")
    for (auto &l : limits_) {
        l[0] = 1.;
        l[1] = -1.;
    }
    toolhead_->dwell(DISABLE_STALL_TIME);
}

/****************************************************************
 * G-code commands
 ****************************************************************/

void
Printer::register_commands()
{
    using namespace std::placeholders;
    auto reg = [this](const char *name, bool (Printer::*fn)(GCodeCommand &,
                                                            std::string *)) {
        gcode_.register_command(name, std::bind(fn, this, _1, _2));
    };
    reg("G1", &Printer::cmd_G1);
    reg("G0", &Printer::cmd_G1);
    reg("G4", &Printer::cmd_G4);
    reg("G28", &Printer::cmd_G28);
    reg("QUERY_ENDSTOPS", &Printer::cmd_QUERY_ENDSTOPS);
    reg("M119", &Printer::cmd_QUERY_ENDSTOPS);
    reg("G92", &Printer::cmd_G92);
    reg("M114", &Printer::cmd_M114);
    reg("M220", &Printer::cmd_M220);
    reg("SET_VELOCITY_LIMIT", &Printer::cmd_SET_VELOCITY_LIMIT);
    reg("SET_KINEMATIC_POSITION", &Printer::cmd_SET_KINEMATIC_POSITION);
    reg("GET_POSITION", &Printer::cmd_GET_POSITION);
    gcode_.register_command("G90", [this](GCodeCommand &, std::string *) {
        gm_.absolute_coord = true;
        return true;
    });
    gcode_.register_command("G91", [this](GCodeCommand &, std::string *) {
        gm_.absolute_coord = false;
        return true;
    });
    gcode_.register_command("M82", [this](GCodeCommand &, std::string *) {
        gm_.absolute_extrude = true;
        return true;
    });
    gcode_.register_command("M83", [this](GCodeCommand &, std::string *) {
        gm_.absolute_extrude = false;
        return true;
    });
    gcode_.register_command("G20", [](GCodeCommand &, std::string *err) {
        *err = "Machine does not support G20 (inches) command";
        return false;
    });
    gcode_.register_command("G21", [](GCodeCommand &, std::string *) {
        return true;
    });
    gcode_.register_command("M400", [this](GCodeCommand &, std::string *) {
        wait_moves();
        return true;
    });
    auto m18 = [this](GCodeCommand &, std::string *err) {
        motor_off(err);
        return true;
    };
    gcode_.register_command("M18", m18);
    gcode_.register_command("M84", m18);
    gcode_.register_command("M204", [this](GCodeCommand &g, std::string *err) {
        double accel;
        if (g.has("S")) {
            if (!g.get_float("S", &accel, err, nullptr, NAN, NAN, 0.))
                return false;
        } else if (g.has("P") && g.has("T")) {
            double p, t;
            if (!g.get_float("P", &p, err, nullptr, NAN, NAN, 0.)
                || !g.get_float("T", &t, err, nullptr, NAN, NAN, 0.))
                return false;
            accel = std::min(p, t);
        } else {
            return true;
        }
        toolhead_->max_accel = accel;
        toolhead_->update_limits();
        return true;
    });
    gcode_.register_command("M112", [this](GCodeCommand &, std::string *) {
        mcu_.emergency_stop();
        return true;
    });
}

bool
Printer::cmd_G1(GCodeCommand &g, std::string *err)
{
    // gcode_move.py cmd_G1()
    if (!is_ready()) {
        *err = state_message();
        return false;
    }
    const char *axes = "XYZ";
    for (int pos = 0; pos < 3; pos++) {
        std::string a(1, axes[pos]);
        if (!g.has(a))
            continue;
        double v;
        if (!g.get_float(a, &v, err)) {
            *err = "Unable to parse move '" + g.get_commandline() + "'";
            return false;
        }
        if (!gm_.absolute_coord)
            gm_.last_position[pos] += v;
        else
            gm_.last_position[pos] = v + gm_.base_position[pos];
    }
    if (g.has("E")) {
        double v;
        if (!g.get_float("E", &v, err))
            return false;
        double ne = (!gm_.absolute_coord || !gm_.absolute_extrude)
            ? gm_.last_position[3] + v * gm_.extrude_factor
            : v * gm_.extrude_factor + gm_.base_position[3];
        if (ne != gm_.last_position[3]) {
            *err = "Extrude when no extruder present";
            return false;
        }
    }
    if (g.has("F")) {
        double f;
        if (!g.get_float("F", &f, err))
            return false;
        if (f <= 0.) {
            *err = "Invalid speed in '" + g.get_commandline() + "'";
            return false;
        }
        gm_.speed = f * gm_.speed_factor;
    }
    // Enable the motors at the start of this move (stepper_enable)
    if (std::any_of(steppers_.begin(), steppers_.end(),
                    [](const std::unique_ptr<PrinterStepper> &s) {
                        return !s->enabled;
                    }))
        toolhead_->register_lookahead_callback(
            [this](double pt) { motor_enable(pt); });
    double t0 = monotonic();
    bool moved = toolhead_->move(gm_.last_position, gm_.speed, err);
    last_move_submit_ = monotonic();
    stats_.move_s += last_move_submit_ - t0;
    if (!moved)
        return false;
    throttle();
    return true;
}

bool
Printer::cmd_G4(GCodeCommand &g, std::string *err)
{
    double def = 0., p;
    if (!g.get_float("P", &p, err, &def, 0.))
        return false;
    toolhead_->dwell(p / 1000.);
    throttle();
    return true;
}

bool
Printer::cmd_G92(GCodeCommand &g, std::string *err)
{
    const char *axes = "XYZE";
    bool any = false;
    for (int pos = 0; pos < 4; pos++) {
        std::string a(1, axes[pos]);
        if (!g.has(a))
            continue;
        double offset;
        if (!g.get_float(a, &offset, err))
            return false;
        if (pos == 3)
            offset *= gm_.extrude_factor;
        gm_.base_position[pos] = gm_.last_position[pos] - offset;
        any = true;
    }
    if (!any)
        for (int pos = 0; pos < 4; pos++)
            gm_.base_position[pos] = gm_.last_position[pos];
    return true;
}

bool
Printer::cmd_M114(GCodeCommand &, std::string *)
{
    double p[4];
    for (int i = 0; i < 4; i++)
        p[i] = gm_.last_position[i] - gm_.base_position[i];
    gcode_.respond_raw(fmt("X:%.3f Y:%.3f Z:%.3f E:%.3f", p[0], p[1], p[2],
                           p[3]));
    return true;
}

bool
Printer::cmd_M220(GCodeCommand &g, std::string *err)
{
    double def = 100., v;
    if (!g.get_float("S", &v, err, &def, NAN, NAN, 0.))
        return false;
    v /= 60. * 100.;
    gm_.speed = gm_.speed / gm_.speed_factor * v;
    gm_.speed_factor = v;
    return true;
}

bool
Printer::cmd_SET_VELOCITY_LIMIT(GCodeCommand &g, std::string *err)
{
    ToolHead &th = *toolhead_;
    double v;
    if (g.has("VELOCITY")) {
        if (!g.get_float("VELOCITY", &v, err, nullptr, NAN, NAN, 0.))
            return false;
        th.max_velocity = v;
    }
    if (g.has("ACCEL")) {
        if (!g.get_float("ACCEL", &v, err, nullptr, NAN, NAN, 0.))
            return false;
        th.max_accel = v;
    }
    if (g.has("SQUARE_CORNER_VELOCITY")) {
        if (!g.get_float("SQUARE_CORNER_VELOCITY", &v, err, nullptr, 0.))
            return false;
        th.square_corner_velocity = v;
    }
    if (g.has("MINIMUM_CRUISE_RATIO")) {
        if (!g.get_float("MINIMUM_CRUISE_RATIO", &v, err, nullptr, 0., NAN,
                         NAN, 1.))
            return false;
        th.min_cruise_ratio = v;
    }
    th.update_limits();
    gcode_.respond_info(fmt("max_velocity: %.6f\nmax_accel: %.6f\n"
                            "minimum_cruise_ratio: %.6f\n"
                            "square_corner_velocity: %.6f",
                            th.max_velocity, th.max_accel, th.min_cruise_ratio,
                            th.square_corner_velocity));
    return true;
}

bool
Printer::cmd_SET_KINEMATIC_POSITION(GCodeCommand &g, std::string *err)
{
    // force_move.py
    toolhead_->get_last_move_time();
    const double *cur = toolhead_->commanded_pos();
    double pos[3];
    const char *axes = "XYZ";
    for (int i = 0; i < 3; i++)
        if (!g.get_float(std::string(1, axes[i]), &pos[i], err, &cur[i]))
            return false;
    std::string homed;
    if (!g.get("SET_HOMED", &homed, err, "xyz"))
        return false;
    for (auto &c : homed)
        c = (char)tolower((unsigned char)c);
    std::string clear;
    if (!g.get("CLEAR_HOMED", &clear, err, ""))
        return false;
    set_position(pos, homed.c_str());
    for (char c : clear) {
        int i = tolower((unsigned char)c) - 'x';
        if (i >= 0 && i < 3) {
            limits_[i][0] = 1.;
            limits_[i][1] = -1.;
        }
    }
    return true;
}

bool
Printer::cmd_GET_POSITION(GCodeCommand &, std::string *err)
{
    toolhead_->get_last_move_time();
    std::string mcu_pos = "mcu:", stepper_pos = "stepper:";
    for (auto &s : steppers_) {
        int64_t p;
        if (!query_mcu_position(*s, &p, err))
            return false;
        mcu_pos += fmt(" %s:%lld", s->name.c_str(), (long long)p);
        stepper_pos += fmt(" %s:%.6f", s->name.c_str(),
                           itersolve_get_commanded_pos(s->active_sk()));
    }
    const double *c = toolhead_->commanded_pos();
    gcode_.respond_info(mcu_pos + "\n" + stepper_pos + "\n"
                        + fmt("toolhead: X:%.6f Y:%.6f Z:%.6f", c[0], c[1],
                              c[2]));
    return true;
}

} // namespace kl
