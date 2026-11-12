// Port of klippy/toolhead.py and klippy/extras/motion_queuing.py.
// See motion.hpp.
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "klippy_lite/motion.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "klippy_lite/platform.hpp"

namespace kl {

/****************************************************************
 * MotionQueuing (motion_queuing.py PrinterMotionQueuing)
 ****************************************************************/

MotionQueuing::MotionQueuing(const MotionQueuingConfig &cfg)
    : cfg_(cfg)
{
    ssm_ = steppersyncmgr_alloc();
    ss_ = steppersyncmgr_alloc_steppersync(ssm_);
}

MotionQueuing::~MotionQueuing()
{
    steppersyncmgr_free(ssm_);
    for (trapq *tq : trapqs_)
        trapq_free(tq);
}

trapq *
MotionQueuing::allocate_trapq()
{
    trapq *tq = trapq_alloc();
    trapqs_.push_back(tq);
    return tq;
}

syncemitter *
MotionQueuing::allocate_syncemitter(const char *name, bool alloc_sc)
{
    char tname[16] = {};
    snprintf(tname, sizeof(tname), "%s", name);
    return steppersync_alloc_syncemitter(ss_, tname, alloc_sc ? 1 : 0);
}

void
MotionQueuing::setup_mcu_movequeue(int move_count, double mcu_freq,
                                   serialqueue *sq)
{
    steppersync_setup_movequeue(ss_, sq, move_count);
    steppersync_set_time(ss_, 0., mcu_freq);
}

void
MotionQueuing::register_flush_callback(std::function<void(double, double)> cb,
                                       bool can_add_trapq)
{
    if (can_add_trapq)
        flush_callbacks_.insert(flush_callbacks_.begin(), std::move(cb));
    else
        flush_callbacks_.push_back(std::move(cb));
}

void
MotionQueuing::check_step_generation_scan_windows()
{
    double kfd = SDS_CHECK_TIME;
    for (stepper_kinematics *sk : sks_) {
        kfd = std::max({kfd, itersolve_get_gen_steps_pre_active(sk),
                        itersolve_get_gen_steps_post_active(sk)});
    }
    kin_flush_delay_ = kfd;
}

double
MotionQueuing::calc_step_gen_restart(double est_print_time) const
{
    double kin_time = std::max(est_print_time + MIN_KIN_TIME,
                               last_step_gen_time_);
    return kin_time + kin_flush_delay_;
}

void
MotionQueuing::note_mcu_movequeue_activity(double mq_time, bool is_step_gen)
{
    if (is_step_gen) {
        mq_time += kin_flush_delay_;
        need_step_gen_time_ = std::max(need_step_gen_time_, mq_time);
    }
    need_flush_time_ = std::max(need_flush_time_, mq_time);
}

void
MotionQueuing::advance_flush_time(double want_flush_time,
                                  double want_step_gen_time)
{
    if (failed_)
        return;
    double flush_time = std::max({want_flush_time, last_flush_time_,
                                  want_step_gen_time - STEPCOMPRESS_FLUSH_TIME});
    double step_gen_time = std::max({want_step_gen_time, last_step_gen_time_,
                                     flush_time});
    // Invoke flush callbacks (if any)
    for (auto &cb : flush_callbacks_)
        cb(flush_time, step_gen_time);
    // Determine maximum history to keep
    double trapq_free_time = step_gen_time - kin_flush_delay_;
    double clear_history_time = std::max(clear_history_time_,
                                         trapq_free_time - cfg_.history_expire);
    // Generate stepper movement and transmit
    uint64_t t0 = now_us();
    syncemitter *ret = steppersyncmgr_gen_steps(ssm_, flush_time,
                                                step_gen_time,
                                                clear_history_time);
    uint64_t dt = now_us() - t0;
    double covered = step_gen_time - last_step_gen_time_;
    if (getenv("BENCH_TRACE") && covered > 0.3)
        fprintf(stderr, "trace gen covered %.3f s (flush %.3f -> %.3f,"
                " sg %.3f -> %.3f) %llu us\n", covered, last_flush_time_,
                flush_time, last_step_gen_time_, step_gen_time,
                (unsigned long long)dt);
    gen_stats_.calls++;
    gen_stats_.total_us += dt;
    gen_stats_.max_us = std::max(gen_stats_.max_us, dt);
    if (covered > 0.010) {
        gen_stats_.covered_time += covered;
        gen_stats_.max_load = std::max(gen_stats_.max_load,
                                       dt * .000001 / covered);
    }
    if (ret) {
        fprintf(stderr, "Internal error in stepcompress (flush=%.6f gen=%.6f)\n",
                flush_time, step_gen_time);
        failed_ = true;
        return;
    }
    last_flush_time_ = flush_time;
    last_step_gen_time_ = step_gen_time;
    // Move processed trapq entries to history list, and expire old history
    for (trapq *tq : trapqs_)
        trapq_finalize_moves(tq, trapq_free_time, clear_history_time);
}

void
MotionQueuing::flush_all_steps()
{
    double flush_time = need_step_gen_time_;
    await_flush_time(flush_time);
    advance_flush_time(flush_time);
}

void
MotionQueuing::await_flush_time(double want_flush_time)
{
    if (!pause || !estimated_print_time)
        return;
    for (;;) {
        if (last_flush_time_ >= want_flush_time)
            return;
        double wait = want_flush_time - BGFLUSH_HIGH_TIME
                      - estimated_print_time();
        if (wait <= 0.)
            return;
        pause(std::min(1., wait));
    }
}

void
MotionQueuing::drip_update_time(double start_time, double end_time,
                                const std::function<bool()> &triggered)
{
    await_flush_time(start_time);
    // Disable background flushing (flush_handler) during the drip
    drip_active_ = true;
    advance_flush_time(start_time - SDS_CHECK_TIME, start_time);
    // Flush in segments until the trigger signal
    double flush_time = start_time;
    while (flush_time < end_time) {
        if (triggered())
            break;
        if (estimated_print_time && pause) {
            double wait_time = flush_time - estimated_print_time() - DRIP_TIME;
            if (wait_time > 0.) {
                // Pause before sending more steps (wake early on trigger)
                pause(std::min(wait_time, 0.005));
                continue;
            }
        }
        flush_time = std::min(flush_time + DRIP_SEGMENT_TIME, end_time);
        note_mcu_movequeue_activity(flush_time);
        advance_flush_time(flush_time - SDS_CHECK_TIME, flush_time);
    }
    drip_active_ = false;
    advance_flush_time(flush_time + kin_flush_delay_);
}

void
MotionQueuing::wipe_trapq(trapq *tq)
{
    trapq_finalize_moves(tq, 1e300, 0.);
}

bool
MotionQueuing::flush_handler(double est_print_time)
{
    double prev_flush = last_flush_time_, prev_sg = last_step_gen_time_;
    double aggr_sg_time = need_step_gen_time_ - 2. * kin_flush_delay_;
    if (last_step_gen_time_ < aggr_sg_time) {
        // Actively stepping - want more aggressive flushing
        double want_sg_time = est_print_time + cfg_.bgflush_sg_high;
        double batch_time = cfg_.bgflush_sg_high - cfg_.bgflush_sg_low;
        double next_batch_time = last_step_gen_time_ + batch_time;
        if (next_batch_time > est_print_time) {
            // Improve run-to-run reproducibility by batching from last
            if (next_batch_time > want_sg_time + 0.005)
                // Delay flushing until next wakeup
                next_batch_time = last_step_gen_time_;
            want_sg_time = next_batch_time;
        }
        want_sg_time = std::min(want_sg_time, aggr_sg_time);
        // Flush motion queues (if needed)
        if (want_sg_time > last_step_gen_time_)
            advance_flush_time(0., want_sg_time);
    } else {
        // Not stepping (or only step remnants) - use relaxed flushing
        double want_flush_time = est_print_time + BGFLUSH_HIGH_TIME;
        double max_flush_time = need_flush_time_ + BGFLUSH_EXTRA_TIME;
        want_flush_time = std::min(want_flush_time, max_flush_time);
        // Flush motion queues (if needed)
        if (want_flush_time > last_flush_time_)
            advance_flush_time(want_flush_time);
    }
    return last_flush_time_ != prev_flush || last_step_gen_time_ != prev_sg;
}

/****************************************************************
 * Move (toolhead.py Move)
 ****************************************************************/

Move::Move(const ToolHead &th, const double start[3], const double end[3],
           double speed)
{
    accel = th.max_accel;
    junction_deviation = th.junction_deviation;
    double velocity = std::min(speed, th.max_velocity);
    double sum = 0.;
    for (int i = 0; i < 3; i++) {
        start_pos[i] = start[i];
        end_pos[i] = end[i];
        axes_d[i] = end[i] - start[i];
        sum += axes_d[i] * axes_d[i];
    }
    move_d = std::sqrt(sum);
    // Extrude-only moves do not exist in this port (no extruder axis);
    // ToolHead::move() drops zero length moves before constructing a Move.
    double inv_move_d = 1. / move_d;
    for (int i = 0; i < 3; i++)
        axes_r[i] = axes_d[i] * inv_move_d;
    min_move_t = move_d / velocity;
    max_start_v2 = 0.;
    max_cruise_v2 = velocity * velocity;
    delta_v2 = 2.0 * move_d * accel;
    next_junction_v2 = 999999999.9;
    max_mcr_start_v2 = 0.;
    mcr_delta_v2 = 2.0 * move_d * th.mcr_pseudo_accel;
}

void
Move::limit_speed(double speed, double new_accel)
{
    double speed2 = speed * speed;
    if (speed2 < max_cruise_v2) {
        max_cruise_v2 = speed2;
        min_move_t = move_d / speed;
    }
    accel = std::min(accel, new_accel);
    delta_v2 = 2.0 * move_d * accel;
    mcr_delta_v2 = std::min(mcr_delta_v2, delta_v2);
}

void
Move::limit_next_junction_speed(double speed)
{
    next_junction_v2 = std::min(next_junction_v2, speed * speed);
}

void
Move::calc_junction(const Move &prev)
{
    // DummyExtruder.calc_junction() returns move.max_cruise_v2
    double ea_v2 = max_cruise_v2;
    double max_start = std::min({max_cruise_v2, prev.max_cruise_v2,
                                 prev.next_junction_v2,
                                 prev.max_start_v2 + prev.delta_v2, ea_v2});
    // Find max velocity using "approximated centripetal velocity"
    double junction_cos_theta = -(axes_r[0] * prev.axes_r[0]
                                  + axes_r[1] * prev.axes_r[1]
                                  + axes_r[2] * prev.axes_r[2]);
    double sin_theta_d2 = std::sqrt(std::max(0.5 * (1.0 - junction_cos_theta),
                                             0.));
    double cos_theta_d2 = std::sqrt(std::max(0.5 * (1.0 + junction_cos_theta),
                                             0.));
    double one_minus_sin_theta_d2 = 1. - sin_theta_d2;
    if (one_minus_sin_theta_d2 > 0. && cos_theta_d2 > 0.) {
        double R_jd = sin_theta_d2 / one_minus_sin_theta_d2;
        double move_jd_v2 = R_jd * junction_deviation * accel;
        double pmove_jd_v2 = R_jd * prev.junction_deviation * prev.accel;
        // Approximated circle must contact moves no further than mid-move
        double quarter_tan_theta_d2 = .25 * sin_theta_d2 / cos_theta_d2;
        double move_centripetal_v2 = delta_v2 * quarter_tan_theta_d2;
        double pmove_centripetal_v2 = prev.delta_v2 * quarter_tan_theta_d2;
        max_start = std::min({max_start, move_jd_v2, pmove_jd_v2,
                              move_centripetal_v2, pmove_centripetal_v2});
    }
    // Apply limits
    max_start_v2 = max_start;
    max_mcr_start_v2 = std::min(max_start,
                                prev.max_mcr_start_v2 + prev.mcr_delta_v2);
}

void
Move::set_junction(double start_v2, double cruise_v2, double end_v2)
{
    // Determine accel, cruise, and decel portions of the move distance
    double half_inv_accel = .5 / accel;
    double accel_d = (cruise_v2 - start_v2) * half_inv_accel;
    double decel_d = (cruise_v2 - end_v2) * half_inv_accel;
    double cruise_d = move_d - accel_d - decel_d;
    // Determine move velocities
    start_v = std::sqrt(start_v2);
    cruise_v = std::sqrt(cruise_v2);
    end_v = std::sqrt(end_v2);
    // Determine time spent in each portion of move (time is the
    // distance divided by average velocity)
    accel_t = accel_d / ((start_v + cruise_v) * 0.5);
    cruise_t = cruise_d / cruise_v;
    decel_t = decel_d / ((end_v + cruise_v) * 0.5);
}

/****************************************************************
 * LookAheadQueue (toolhead.py LookAheadQueue)
 ****************************************************************/

void
LookAheadQueue::reset()
{
    queue_.clear();
    junction_flush_ = LOOKAHEAD_FLUSH_TIME;
}

std::vector<std::unique_ptr<Move>>
LookAheadQueue::flush(bool lazy)
{
    junction_flush_ = LOOKAHEAD_FLUSH_TIME;
    bool update_flush_count = lazy;
    int flush_count = (int)queue_.size();
    struct JunctionInfo {
        double start_v2, cruise_v2, next_start_v2;
        bool have_cruise;
    };
    std::vector<JunctionInfo> junction_info(flush_count);
    // Traverse queue from last to first move and determine maximum
    // junction speed assuming the robot comes to a complete stop
    // after the last move.
    double next_start_v2 = 0., next_mcr_start_v2 = 0., peak_cruise_v2 = 0.;
    int pending_cv2_assign = 0;
    for (int i = flush_count - 1; i >= 0; i--) {
        Move &move = *queue_[i];
        double reachable_start_v2 = next_start_v2 + move.delta_v2;
        double start_v2 = std::min(move.max_start_v2, reachable_start_v2);
        double cruise_v2 = 0.;
        bool have_cruise = false;
        pending_cv2_assign += 1;
        double reach_mcr_start_v2 = next_mcr_start_v2 + move.mcr_delta_v2;
        double mcr_start_v2 = std::min(move.max_mcr_start_v2,
                                       reach_mcr_start_v2);
        if (mcr_start_v2 < reach_mcr_start_v2) {
            // It's possible for this move to accelerate
            if (mcr_start_v2 + move.mcr_delta_v2 > next_mcr_start_v2
                || pending_cv2_assign > 1) {
                // This move can both accel and decel, or this is a
                // full accel move followed by a full decel move
                if (update_flush_count && peak_cruise_v2 != 0.) {
                    flush_count = i + pending_cv2_assign;
                    update_flush_count = false;
                }
                peak_cruise_v2 = (mcr_start_v2 + reach_mcr_start_v2) * .5;
            }
            cruise_v2 = std::min({(start_v2 + reachable_start_v2) * .5,
                                  move.max_cruise_v2, peak_cruise_v2});
            have_cruise = true;
            pending_cv2_assign = 0;
        }
        junction_info[i] = {start_v2, cruise_v2, next_start_v2, have_cruise};
        next_start_v2 = start_v2;
        next_mcr_start_v2 = mcr_start_v2;
    }
    std::vector<std::unique_ptr<Move>> res;
    if (update_flush_count || !flush_count)
        return res;
    // Traverse queue in forward direction to propagate cruise_v2
    double prev_cruise_v2 = 0.;
    for (int i = 0; i < flush_count; i++) {
        JunctionInfo &ji = junction_info[i];
        double cruise_v2 = ji.cruise_v2;
        if (!ji.have_cruise)
            // This move can't accelerate - propagate cruise_v2 from previous
            cruise_v2 = std::min(prev_cruise_v2, ji.start_v2);
        queue_[i]->set_junction(std::min(ji.start_v2, cruise_v2), cruise_v2,
                                std::min(ji.next_start_v2, cruise_v2));
        prev_cruise_v2 = cruise_v2;
    }
    // Remove processed moves from the queue
    res.reserve(flush_count);
    for (int i = 0; i < flush_count; i++) {
        res.push_back(std::move(queue_.front()));
        queue_.pop_front();
    }
    return res;
}

bool
LookAheadQueue::add_move(std::unique_ptr<Move> move)
{
    queue_.push_back(std::move(move));
    max_size_ = std::max(max_size_, queue_.size());
    if (queue_.size() == 1)
        return false;
    Move &m = *queue_.back();
    m.calc_junction(*queue_[queue_.size() - 2]);
    junction_flush_ -= m.min_move_t;
    // Check if enough moves have been queued to reach the target flush time.
    return junction_flush_ <= 0. || (max_moves_ && queue_.size() >= max_moves_);
}

/****************************************************************
 * ToolHead (toolhead.py ToolHead)
 ****************************************************************/

ToolHead::ToolHead(MotionQueuing &mq, const ToolHeadConfig &cfg)
    : max_velocity(cfg.max_velocity), max_accel(cfg.max_accel),
      square_corner_velocity(cfg.square_corner_velocity),
      min_cruise_ratio(cfg.minimum_cruise_ratio),
      buffer_time_high(cfg.buffer_time_high), mq_(mq)
{
    lookahead_.set_flush_time(buffer_time_high);
    lookahead_.set_max_moves(cfg.lookahead_max_moves);
    calc_junction_deviation();
    mq_.register_flush_callback(
        [this](double ft, double sgt) { handle_step_flush(ft, sgt); }, true);
    trapq_ = mq_.allocate_trapq();
}

void
ToolHead::calc_junction_deviation()
{
    double scv2 = square_corner_velocity * square_corner_velocity;
    junction_deviation = scv2 * (std::sqrt(2.) - 1.) / max_accel;
    mcr_pseudo_accel = max_accel * (1. - min_cruise_ratio);
}

void
ToolHead::advance_move_time(double next_print_time)
{
    print_time_ = std::max(print_time_, next_print_time);
}

void
ToolHead::calc_print_time()
{
    double est_print_time = estimated_print_time();
    double kin_time = mq_.calc_step_gen_restart(est_print_time);
    double min_print_time = std::max(est_print_time + BUFFER_TIME_START,
                                     kin_time);
    if (min_print_time > print_time_)
        print_time_ = min_print_time;
}

void
ToolHead::process_lookahead(bool lazy)
{
    std::vector<std::unique_ptr<Move>> moves = lookahead_.flush(lazy);
    if (moves.empty())
        return;
    // Resync print_time if necessary
    if (special_state_ != QueuingState::Main) {
        // Transition from "NeedPrime"/"Priming" state to main state
        special_state_ = QueuingState::Main;
        calc_print_time();
    }
    // Queue moves into trapezoid motion queue (trapq)
    double next_move_time = print_time_;
    for (auto &move : moves) {
        trapq_append(trapq_, next_move_time,
                     move->accel_t, move->cruise_t, move->decel_t,
                     move->start_pos[0], move->start_pos[1], move->start_pos[2],
                     move->axes_r[0], move->axes_r[1], move->axes_r[2],
                     move->start_v, move->cruise_v, move->accel);
        next_move_time = (next_move_time + move->accel_t + move->cruise_t
                          + move->decel_t);
        for (auto &cb : move->timing_callbacks)
            cb(next_move_time);
    }
    // Generate steps for moves
    advance_move_time(next_move_time);
    mq_.note_mcu_movequeue_activity(next_move_time);
}

void
ToolHead::flush_lookahead(bool is_runout)
{
    (void)is_runout;
    // Transit from "NeedPrime"/"Priming"/main state to "NeedPrime"
    process_lookahead();
    special_state_ = QueuingState::NeedPrime;
    lookahead_.set_flush_time(buffer_time_high);
}

void
ToolHead::handle_step_flush(double flush_time, double step_gen_time)
{
    (void)flush_time;
    if (special_state_ != QueuingState::Main)
        return;
    // In "main" state - flush lookahead if buffer runs low
    double kin_flush_delay = mq_.get_kin_flush_delay();
    if (step_gen_time >= print_time_ - kin_flush_delay - 0.001) {
        if (!lookahead_.is_empty())
            runouts_++;
        flush_lookahead(true);
    }
}

void
ToolHead::flush_step_generation()
{
    flush_lookahead();
    mq_.flush_all_steps();
}

double
ToolHead::get_last_move_time()
{
    if (special_state_ != QueuingState::Main) {
        flush_lookahead();
        calc_print_time();
    } else {
        process_lookahead();
    }
    return print_time_;
}

void
ToolHead::set_position(const double newpos[3])
{
    flush_step_generation();
    trapq_set_position(trapq_, print_time_, newpos[0], newpos[1], newpos[2]);
    for (int i = 0; i < 3; i++)
        commanded_pos_[i] = newpos[i];
    if (kin_set_position) {
        kin_set_position(newpos);
        return;
    }
    for (stepper_kinematics *sk : sks_)
        itersolve_set_position(sk, newpos[0], newpos[1], newpos[2]);
}

bool
ToolHead::move(const double newpos[3], double speed, std::string *err)
{
    double d2 = 0.;
    for (int i = 0; i < 3; i++) {
        double d = newpos[i] - commanded_pos_[i];
        d2 += d * d;
    }
    // Same threshold as Move.__init__ (move_d < .000000001 has no XYZ motion)
    if (std::sqrt(d2) < .000000001)
        return true;
    auto move = std::make_unique<Move>(*this, commanded_pos_, newpos, speed);
    // kinematics check_move(): limits and per-axis speed limits
    if (check_move && !check_move(*move, err))
        return false;
    for (int i = 0; i < 3; i++)
        commanded_pos_[i] = move->end_pos[i];
    move_count_++;
    bool want_flush = lookahead_.add_move(std::move(move));
    if (want_flush)
        process_lookahead(true);
    return true;
}

bool
ToolHead::drip_move(const double newpos[3], double speed,
                    const std::function<bool()> &triggered, std::string *err)
{
    // toolhead.py drip_move()
    double d2 = 0.;
    for (int i = 0; i < 3; i++) {
        double d = newpos[i] - commanded_pos_[i];
        d2 += d * d;
    }
    std::unique_ptr<Move> move;
    if (std::sqrt(d2) >= .000000001) {
        move = std::make_unique<Move>(*this, commanded_pos_, newpos, speed);
        if (check_move && !check_move(*move, err))
            return false;
    }
    // Make sure stepper movement doesn't start before nominal start time
    dwell(mq_.get_kin_flush_delay());
    process_lookahead();
    // _drip_load_trapq()
    if (move) {
        for (int i = 0; i < 3; i++)
            commanded_pos_[i] = move->end_pos[i];
        lookahead_.add_move(std::move(move));
    }
    std::vector<std::unique_ptr<Move>> moves = lookahead_.flush();
    calc_print_time();
    double start_time = print_time_, end_time = print_time_;
    for (auto &m : moves) {
        trapq_append(trapq_, end_time, m->accel_t, m->cruise_t, m->decel_t,
                     m->start_pos[0], m->start_pos[1], m->start_pos[2],
                     m->axes_r[0], m->axes_r[1], m->axes_r[2],
                     m->start_v, m->cruise_v, m->accel);
        end_time += m->accel_t + m->cruise_t + m->decel_t;
    }
    lookahead_.reset();
    // Transmit move in "drip" mode
    mq_.drip_update_time(start_time, end_time, triggered);
    // Move finished; cleanup any remnants on trapq
    mq_.wipe_trapq(trapq_);
    return true;
}

void
ToolHead::dwell(double delay)
{
    flush_lookahead();
    double next_print_time = get_last_move_time() + std::max(0., delay);
    advance_move_time(next_print_time);
}

void
ToolHead::register_lookahead_callback(TimingCallback cb)
{
    Move *last_move = lookahead_.get_last();
    if (!last_move) {
        cb(get_last_move_time());
        return;
    }
    last_move->timing_callbacks.push_back(std::move(cb));
}

} // namespace kl
