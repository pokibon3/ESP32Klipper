// Homing: port of klippy/extras/homing.py (HomingMove, Homing, G28),
// the homing parts of stepper.py (PrinterRail endstops, note_homing_end,
// _query_mcu_position) and kinematics/corexy.py home().
//
// Endstops are real pins ("^!gpio25") or TMC2209 virtual endstops
// ("tmc2209_stepper_x:virtual_endstop", i.e. sensorless homing on the
// driver's DIAG pin).
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <cmath>
#include <cstdio>

#include "chelper_api.h"
#include "klippy/printer.hpp"

namespace kl {

static constexpr double HOMING_START_DELAY = 0.001;
static constexpr double ENDSTOP_SAMPLE_TIME = .000015;
static constexpr int ENDSTOP_SAMPLE_COUNT = 4;

/****************************************************************
 * Setup (stepper.py PrinterRail, corexy.py __init__)
 ****************************************************************/

bool
Printer::setup_endstops(std::string *err)
{
    for (auto &sp : steppers_) {
        PrinterStepper &s = *sp;
        if (s.endstop_pin.empty()) {
            *err = "Option 'endstop_pin' in section '" + s.name
                   + "' must be specified";
            return false;
        }
        PinParams pp;
        if (!parse_pin(s.endstop_pin, true, true, &pp, err))
            return false;
        std::string pin = pp.pin;
        int pullup = pp.pullup;
        bool invert = pp.invert;
        if (pp.chip != "mcu") {
            // tmc.py TMCVirtualPinHelper: "<driver>_<stepper>:virtual_endstop"
            Tmc2209 *tmc = nullptr;
            for (auto &t : tmcs_)
                if (pp.chip == "tmc2209_" + t->stepper_name())
                    tmc = t.get();
            if (!tmc) {
                *err = "Unknown pin chip name '" + pp.chip + "'";
                return false;
            }
            if (pp.pin != "virtual_endstop") {
                *err = "tmc virtual endstop only useful as endstop";
                return false;
            }
            if (pp.invert || pp.pullup) {
                *err = "Can not pullup/invert tmc virtual pin";
                return false;
            }
            if (tmc->diag_pin().empty()) {
                *err = "tmc virtual endstop requires diag pin config";
                return false;
            }
            PinParams dp;
            if (!parse_pin(tmc->diag_pin(), true, true, &dp, err))
                return false;
            pin = dp.pin;
            pullup = dp.pullup;
            invert = dp.invert;
            s.virtual_tmc = tmc;
        }
        endstops_.emplace_back(new McuEndstop(
            mcu_, pin, pullup, invert,
            [this](PrinterStepper &st) { note_homing_end(st); }));
        s.endstop = endstops_.back().get();
        s.endstop->add_stepper(&s);
    }
    // corexy.py: X and Y endstops stop both A and B steppers
    if (kinematics_ == "corexy") {
        steppers_[0]->endstop->add_stepper(steppers_[1].get());
        steppers_[1]->endstop->add_stepper(steppers_[0].get());
    }
    return true;
}

/****************************************************************
 * Stepper position tracking (stepper.py)
 ****************************************************************/

bool
Printer::sync_mcu_position(PrinterStepper &s, std::string *err)
{
    // MCU_stepper._query_mcu_position()
    MsgParams p;
    if (!mcu_.serial().send_with_response(
            "stepper_get_position oid=" + std::to_string(s.oid),
            "stepper_position", &p, err, s.oid))
        return false;
    int64_t last_pos = p.get("pos");
    if (s.dir_pin.invert)
        last_pos = -last_pos;
    double print_time = mcu_.estimated_print_time(p.receive_time);
    uint64_t clock = (uint64_t)mcu_.print_time_to_clock(print_time);
    stepcompress_set_last_position(s.sc, clock, last_pos);
    // _set_mcu_position()
    s.mcu_position_offset = last_pos * s.step_dist
                            - itersolve_get_commanded_pos(s.active_sk());
    return true;
}

void
Printer::note_homing_end(PrinterStepper &s)
{
    // MCU_stepper.note_homing_end()
    stepcompress_reset(s.sc, 0);
    uint32_t data[3] = {(uint32_t)s.reset_cmd_tag, (uint32_t)s.oid, 0};
    syncemitter_queue_msg(s.se, 0, data, 3);
    std::string err;
    if (!sync_mcu_position(s, &err))
        fprintf(stderr, "note_homing_end %s: %s\n", s.name.c_str(),
                err.c_str());
}

void
Printer::calc_position(const double spos[3], double out[3]) const
{
    if (kinematics_ == "corexy") {
        out[0] = .5 * (spos[0] + spos[1]);
        out[1] = .5 * (spos[0] - spos[1]);
    } else {
        out[0] = spos[0];
        out[1] = spos[1];
    }
    out[2] = spos[2];
}

/****************************************************************
 * HomingMove (homing.py)
 ****************************************************************/

bool
Printer::homing_move(PrinterStepper &rail, const double movepos[3],
                     double speed, bool check_triggered, bool *no_movement,
                     std::string *err)
{
    McuEndstop &es = *rail.endstop;
    ToolHead &th = *toolhead_;
    std::string error;
    // homing:homing_move_begin (TMC virtual endstop setup)
    if (rail.virtual_tmc && !rail.virtual_tmc->homing_move_begin(err))
        return false;
    th.flush_step_generation();
    struct StepperPos {
        PrinterStepper *s;
        int64_t start_pos, halt_pos = 0, trig_pos = 0;
    };
    std::vector<StepperPos> spos;
    for (PrinterStepper *s : es.steppers())
        spos.push_back({s, s->get_mcu_position()});
    double print_time = th.get_last_move_time();
    // _calc_endstop_rate()
    const double *startpos = th.commanded_pos();
    double d2 = 0.;
    for (int i = 0; i < 3; i++)
        d2 += (movepos[i] - startpos[i]) * (movepos[i] - startpos[i]);
    double move_t = std::sqrt(d2) / speed, max_steps = 0.;
    for (PrinterStepper *s : es.steppers()) {
        stepper_kinematics *sk = s->active_sk();
        double a = itersolve_calc_position_from_coord(sk, startpos[0],
                                                      startpos[1], startpos[2]);
        double b = itersolve_calc_position_from_coord(sk, movepos[0],
                                                      movepos[1], movepos[2]);
        max_steps = std::max(max_steps, std::fabs(a - b) / s->step_dist);
    }
    double rest_time = max_steps <= 0. ? .001 : move_t / max_steps;
    if (!es.home_start(print_time, ENDSTOP_SAMPLE_TIME, ENDSTOP_SAMPLE_COUNT,
                       rest_time, true, err))
        return false;
    th.dwell(HOMING_START_DELAY);
    std::string merr;
    if (!th.drip_move(movepos, speed, [&es] { return es.done(); }, &merr))
        error = "Error during homing move: " + merr;
    double move_end_print_time = th.get_last_move_time();
    double trigger_time = 0.;
    std::string werr;
    if (!es.home_wait(move_end_print_time, [this](double s) { pause(s); },
                      &trigger_time, &werr)) {
        if (error.empty())
            error = "Error during homing " + rail.short_name() + ": " + werr;
    } else if (trigger_time <= 0. && check_triggered && error.empty()) {
        error = "No trigger on " + rail.short_name() + " after full movement";
    }
    th.flush_step_generation();
    double tt = trigger_time > 0. ? trigger_time : move_end_print_time;
    bool any_over = false;
    for (auto &p : spos) {
        // StepperPosition.note_home_end()
        p.halt_pos = p.s->get_mcu_position();
        p.trig_pos = stepcompress_find_past_position(
            p.s->sc, (uint64_t)mcu_.print_time_to_clock(tt));
        any_over = any_over || p.halt_pos != p.trig_pos;
    }
    // Not a probe: the toolhead is at movepos, corrected for overshoot
    double haltpos[3] = {movepos[0], movepos[1], movepos[2]};
    if (any_over) {
        th.set_position(movepos);
        double kin_spos[3];
        for (int i = 0; i < 3; i++)
            kin_spos[i] = itersolve_get_commanded_pos(steppers_[i]->active_sk());
        for (auto &p : spos)
            for (int i = 0; i < 3; i++)
                if (steppers_[i].get() == p.s)
                    kin_spos[i] += (p.halt_pos - p.trig_pos) * p.s->step_dist;
        calc_position(kin_spos, haltpos);
    }
    th.set_position(haltpos);
    // homing:homing_move_end
    std::string eerr;
    if (rail.virtual_tmc && !rail.virtual_tmc->homing_move_end(&eerr)
        && error.empty())
        error = eerr;
    if (no_movement) {
        *no_movement = false;
        for (auto &p : spos)
            if (p.start_pos == p.trig_pos)
                *no_movement = true;
    }
    if (!error.empty()) {
        *err = error;
        return false;
    }
    return true;
}

/****************************************************************
 * Homing (homing.py Homing._do_home_rails, corexy.py home)
 ****************************************************************/

bool
Printer::home_axis(int axis, std::string *err)
{
    PrinterStepper &rail = *steppers_[axis];
    ToolHead &th = *toolhead_;
    // corexy.home(): homepos / forcepos
    double movepos = rail.position_endstop;
    double forcepos = movepos;
    if (rail.homing_positive_dir)
        forcepos -= 1.5 * (rail.position_endstop - rail.position_min);
    else
        forcepos += 1.5 * (rail.position_max - rail.position_endstop);
    // _set_start_position(): mark the axis homed at forcepos
    double startpos[3], homepos[3];
    for (int i = 0; i < 3; i++)
        startpos[i] = homepos[i] = th.commanded_pos()[i];
    startpos[axis] = forcepos;
    homepos[axis] = movepos;
    const char homing_axes[2] = {(char)('x' + axis), 0};
    set_position(startpos, homing_axes);
    if (!homing_move(rail, homepos, rail.homing_speed, true, nullptr, err))
        return false;
    if (rail.homing_retract_dist > 0.) {
        // _retract_move()
        double axes_d = movepos - forcepos;
        double retract_r = std::min(1., rail.homing_retract_dist
                                        / std::fabs(axes_d));
        double retractpos[3] = {homepos[0], homepos[1], homepos[2]};
        retractpos[axis] = movepos - axes_d * retract_r;
        if (!th.move(retractpos, rail.homing_retract_speed, err))
            return false;
        double sp[3] = {retractpos[0], retractpos[1], retractpos[2]};
        sp[axis] = retractpos[axis] - axes_d * retract_r;
        th.set_position(sp);
        bool no_movement = false;
        if (!homing_move(rail, homepos, rail.second_homing_speed, true,
                         &no_movement, err))
            return false;
        if (no_movement) {
            *err = "Endstop " + rail.short_name()
                   + " still triggered after retract";
            return false;
        }
    }
    th.flush_step_generation();
    // gcode_move: toolhead:set_position
    for (int i = 0; i < 3; i++)
        gm_.last_position[i] = th.commanded_pos()[i];
    return true;
}

bool
Printer::cmd_G28(GCodeCommand &g, std::string *err)
{
    if (!is_ready()) {
        *err = state_message();
        return false;
    }
    std::vector<int> axes;
    const char *names = "XYZ";
    for (int i = 0; i < 3; i++)
        if (g.has(std::string(1, names[i])))
            axes.push_back(i);
    if (axes.empty())
        axes = {0, 1, 2};
    // Enable the motors (stepper_enable) before moving
    motor_enable(toolhead_->get_last_move_time());
    service();
    for (int axis : axes) {
        if (!home_axis(axis, err)) {
            if (!is_ready())
                *err = "Homing failed due to printer shutdown";
            std::string e2;
            motor_off(&e2);
            return false;
        }
    }
    return true;
}

bool
Printer::cmd_QUERY_ENDSTOPS(GCodeCommand &, std::string *err)
{
    double pt = toolhead_->get_last_move_time();
    std::string out;
    for (auto &s : steppers_) {
        int trig;
        if (!s->endstop->query_endstop(pt, &trig, err))
            return false;
        out += (out.empty() ? "" : " ") + s->short_name() + ":"
               + (trig ? "TRIGGERED" : "open");
    }
    gcode_.respond_raw(out);
    return true;
}

} // namespace kl
