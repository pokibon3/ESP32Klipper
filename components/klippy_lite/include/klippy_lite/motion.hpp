// Port of klippy/toolhead.py (Move, LookAheadQueue, ToolHead) and
// klippy/extras/motion_queuing.py (PrinterMotionQueuing) to C++.
//
// Only what the M1 benchmark needs is ported: XY(Z) kinematic moves, dwell,
// set_position, lookahead callbacks, and the step generation flush logic.
// Names and constants follow the Python sources so the two can be compared
// side by side.  The Python originals are Copyright (C) 2016-2025 Kevin
// O'Connor and others.
//
// This file may be distributed under the terms of the GNU GPLv3 license.
#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "chelper_api.h"

namespace kl {

// toolhead.py
constexpr double LOOKAHEAD_FLUSH_TIME = 0.150;
constexpr double BUFFER_TIME_HIGH = 1.0;
constexpr double BUFFER_TIME_START = 0.250;

// motion_queuing.py
constexpr double BGFLUSH_LOW_TIME = 0.200;
constexpr double BGFLUSH_HIGH_TIME = 0.400;
constexpr double BGFLUSH_SG_LOW_TIME = 0.450;
constexpr double BGFLUSH_SG_HIGH_TIME = 0.700;
constexpr double BGFLUSH_EXTRA_TIME = 0.250;
constexpr double MOVE_HISTORY_EXPIRE = 30.;
constexpr double MIN_KIN_TIME = 0.100;
constexpr double STEPCOMPRESS_FLUSH_TIME = 0.050;
constexpr double SDS_CHECK_TIME = 0.001;
constexpr double DRIP_SEGMENT_TIME = 0.050;
constexpr double DRIP_TIME = 0.100;

using TimingCallback = std::function<void(double)>;

// Statistics collected around steppersyncmgr_gen_steps()
struct GenStats {
    uint64_t calls = 0;
    uint64_t total_us = 0;
    uint64_t max_us = 0;          // longest single gen_steps call
    double max_load = 0.;         // max (cpu time / motion time covered)
    double covered_time = 0.;     // motion time covered by gen_steps calls
};

// Buffering depth.  The defaults are Klipper's values.  They size the
// memory used for queued motion, which matters on the CYD (no PSRAM):
//  - history_expire: seconds of past trapq/stepcompress history kept.
//    Klipper keeps 30 s (~1.2-1.7 MB for card_opt.gcode).
//  - bgflush_sg_low/high: step generation runs this far ahead of the MCU.
//    Must stay below ToolHeadConfig::buffer_time_high, otherwise step
//    generation catches up with the planned moves and forces stops.
struct MotionQueuingConfig {
    double history_expire = MOVE_HISTORY_EXPIRE;
    double bgflush_sg_low = BGFLUSH_SG_LOW_TIME;
    double bgflush_sg_high = BGFLUSH_SG_HIGH_TIME;
};

class MotionQueuing {
public:
    explicit MotionQueuing(const MotionQueuingConfig &cfg = {});
    ~MotionQueuing();
    MotionQueuing(const MotionQueuing &) = delete;
    MotionQueuing &operator=(const MotionQueuing &) = delete;

    trapq *allocate_trapq();
    steppersync *mcu_steppersync() { return ss_; }
    syncemitter *allocate_syncemitter(const char *name, bool alloc_sc);
    // sq = nullptr: no MCU (benchmark sink)
    void setup_mcu_movequeue(int move_count, double mcu_freq,
                             serialqueue *sq = nullptr);
    void register_flush_callback(std::function<void(double, double)> cb,
                                 bool can_add_trapq);
    void add_stepper_kinematics(stepper_kinematics *sk) { sks_.push_back(sk); }
    void check_step_generation_scan_windows();
    double get_kin_flush_delay() const { return kin_flush_delay_; }
    double calc_step_gen_restart(double est_print_time) const;
    void note_mcu_movequeue_activity(double mq_time, bool is_step_gen = true);
    void flush_all_steps();
    // Body of PrinterMotionQueuing._flush_handler() for a given estimated
    // mcu print time.  Returns true if any steps were generated/flushed.
    bool flush_handler(double est_print_time);
    void advance_flush_time(double want_flush_time,
                            double want_step_gen_time = 0.);
    // motion_queuing.py _await_flush_time(): don't flush too far ahead of
    // the MCU (no-op without the real-time hooks below)
    void await_flush_time(double want_flush_time);
    // Homing "drip" moves: flush [start_time, end_time] in short segments
    // until triggered() returns true (motion_queuing.drip_update_time)
    void drip_update_time(double start_time, double end_time,
                          const std::function<bool()> &triggered);
    bool drip_active() const { return drip_active_; }
    // Expire all remaining moves of a trapq into its history
    void wipe_trapq(trapq *tq);

    // Real-time hooks (left unset by the benchmark, which has no MCU):
    // estimated MCU print time now, and "sleep while keeping the host loop
    // (clock sync) running" for the given seconds.
    std::function<double()> estimated_print_time;
    std::function<void(double)> pause;

    double last_flush_time() const { return last_flush_time_; }
    double last_step_gen_time() const { return last_step_gen_time_; }
    double need_flush_time() const { return need_flush_time_; }
    const GenStats &gen_stats() const { return gen_stats_; }
    bool failed() const { return failed_; }

private:
    steppersyncmgr *ssm_;
    steppersync *ss_;
    std::vector<trapq *> trapqs_;
    std::vector<stepper_kinematics *> sks_;
    std::vector<std::function<void(double, double)>> flush_callbacks_;
    MotionQueuingConfig cfg_;
    double kin_flush_delay_ = SDS_CHECK_TIME;
    double clear_history_time_ = 0.;
    double last_flush_time_ = 0., last_step_gen_time_ = 0.;
    double need_flush_time_ = 0., need_step_gen_time_ = 0.;
    GenStats gen_stats_;
    bool failed_ = false;
    bool drip_active_ = false;
};

class ToolHead;

struct Move {
    Move(const ToolHead &th, const double start[3], const double end[3],
         double speed);
    void limit_speed(double speed, double accel);
    void limit_next_junction_speed(double speed);
    void calc_junction(const Move &prev);
    void set_junction(double start_v2, double cruise_v2, double end_v2);

    double start_pos[3], end_pos[3], axes_d[3], axes_r[3];
    double accel, junction_deviation;
    double move_d, min_move_t;
    double max_start_v2, max_cruise_v2, delta_v2, next_junction_v2;
    double max_mcr_start_v2, mcr_delta_v2;
    double start_v = 0., cruise_v = 0., end_v = 0.;
    double accel_t = 0., cruise_t = 0., decel_t = 0.;
    std::vector<TimingCallback> timing_callbacks;
};

class LookAheadQueue {
public:
    void reset();
    void set_flush_time(double t) { junction_flush_ = t; }
    // ESP32 addition: also request a (lazy) flush once this many moves
    // are queued, bounding memory when short moves are queued fast.
    // 0 = no limit (Klipper behaviour).
    void set_max_moves(size_t n) { max_moves_ = n; }
    bool is_empty() const { return queue_.empty(); }
    Move *get_last() { return queue_.empty() ? nullptr : queue_.back().get(); }
    std::vector<std::unique_ptr<Move>> flush(bool lazy = false);
    bool add_move(std::unique_ptr<Move> move);
    size_t size() const { return queue_.size(); }
    size_t max_size() const { return max_size_; }

private:
    std::deque<std::unique_ptr<Move>> queue_;
    double junction_flush_ = LOOKAHEAD_FLUSH_TIME;
    size_t max_size_ = 0;
    size_t max_moves_ = 0;
};

struct ToolHeadConfig {
    double max_velocity = 500.;
    double max_accel = 15000.;
    double square_corner_velocity = 5.;
    double minimum_cruise_ratio = 0.5;
    // toolhead.py BUFFER_TIME_HIGH: how far G-code processing runs ahead
    // of the MCU, and the lookahead fill time after a stop.
    double buffer_time_high = BUFFER_TIME_HIGH;
    // ESP32 addition: see LookAheadQueue::set_max_moves (0 = no limit)
    size_t lookahead_max_moves = 0;
};

class ToolHead {
public:
    ToolHead(MotionQueuing &mq, const ToolHeadConfig &cfg);

    // Queue a move.  Returns false (with *err) if check_move rejects it.
    bool move(const double newpos[3], double speed, std::string *err = nullptr);
    void dwell(double delay);
    void set_position(const double newpos[3]);
    void register_lookahead_callback(TimingCallback cb);
    double get_last_move_time();
    void flush_step_generation();
    // Homing move: queue newpos and send it in "drip" mode until
    // triggered() (toolhead.drip_move).  Returns false if check_move
    // rejects it.
    bool drip_move(const double newpos[3], double speed,
                   const std::function<bool()> &triggered,
                   std::string *err = nullptr);
    // Flush the lookahead queue (toolhead._flush_lookahead); is_runout
    // marks an input underrun (priming timer / wait_moves)
    void flush_lookahead(bool is_runout = false);
    bool lookahead_empty() const { return lookahead_.is_empty(); }
    bool in_main_state() const { return special_state_ == QueuingState::Main; }

    const double *commanded_pos() const { return commanded_pos_; }
    double print_time() const { return print_time_; }
    trapq *get_trapq() { return trapq_; }
    void add_stepper_kinematics(stepper_kinematics *sk) { sks_.push_back(sk); }
    uint64_t move_count() const { return move_count_; }
    size_t lookahead_max() const { return lookahead_.max_size(); }
    // Times step generation caught up with planned motion and forced a
    // lookahead flush (motion stops that the G-code did not ask for).
    uint64_t runouts() const { return runouts_; }

    // Recompute derived limits after changing the values below
    // (SET_VELOCITY_LIMIT)
    void update_limits() { calc_junction_deviation(); }

    double max_velocity, max_accel, square_corner_velocity;
    double min_cruise_ratio, junction_deviation = 0., mcr_pseudo_accel = 0.;
    double buffer_time_high;
    // Kinematics set_position() hook: replaces the plain
    // itersolve_set_position() of each stepper (MCU_stepper.set_position
    // keeps the MCU step position by adjusting its offset)
    std::function<void(const double *)> kin_set_position;
    // Kinematics check_move() hook (limits, homed state, z speed)
    std::function<bool(Move &, std::string *)> check_move;
    // Replacement for mcu.estimated_print_time(reactor.monotonic())
    std::function<double()> estimated_print_time = [] { return 0.; };

private:
    enum class QueuingState { Main, NeedPrime, Priming };
    void calc_junction_deviation();
    void advance_move_time(double t);
    void calc_print_time();
    void process_lookahead(bool lazy = false);
    void handle_step_flush(double flush_time, double step_gen_time);

    MotionQueuing &mq_;
    trapq *trapq_;
    LookAheadQueue lookahead_;
    std::vector<stepper_kinematics *> sks_;
    double commanded_pos_[3] = {0., 0., 0.};
    double print_time_ = 0.;
    QueuingState special_state_ = QueuingState::NeedPrime;
    uint64_t move_count_ = 0;
    uint64_t runouts_ = 0;
};

} // namespace kl
