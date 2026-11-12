// Port of klippy/clocksync.py ClockSync: estimates the MCU clock from
// periodic get_clock queries (decaying linear regression of clock vs.
// host send time), and converts between print_time and MCU clock.
//
// This file may be distributed under the terms of the GNU GPLv3 license.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>

namespace kl {

class SerialHdl;
struct MsgParams;

class ClockSync {
public:
    // Initial sync (blocks ~0.4 s).  Afterwards call tick() periodically
    // (about once a second) from the host's main loop.
    bool connect(SerialHdl &serial, std::string *err);
    // Send a get_clock query if one is due (clocksync._get_clock_event)
    void tick(double eventtime);

    int64_t print_time_to_clock(double print_time) const {
        return (int64_t)(print_time * mcu_freq_);
    }
    double clock_to_print_time(double clock) const { return clock / mcu_freq_; }
    int64_t get_clock(double eventtime) const;
    double estimate_clock_systime(double reqclock) const;
    double estimated_print_time(double eventtime) const {
        return clock_to_print_time((double)get_clock(eventtime));
    }
    int64_t clock32_to_clock64(uint32_t clock32) const;
    bool is_active() const { return queries_pending_ <= 4; }
    double mcu_freq() const { return mcu_freq_; }
    std::string dump_debug() const;

private:
    void handle_clock(const MsgParams &params);
    bool update_regression(double sent_time, double clock);
    void update_best_rtt(double sent_time, double receive_time);

    SerialHdl *serial_ = nullptr;
    mutable std::mutex lock_;
    double mcu_freq_ = 1.;
    int queries_pending_ = 0;
    int64_t last_clock_ = 0;
    double time_avg_ = 0., time_variance_ = 0.;
    double clock_avg_ = 0., clock_covariance_ = 0.;
    double prediction_variance_ = 0., last_prediction_time_ = 0.;
    double min_half_rtt_ = 999999999.9, min_rtt_time_ = 0.;
    double est_time_ = 0., est_clock_ = 0., est_freq_ = 0.; // clock_est
    double next_query_time_ = 0.;
};

} // namespace kl
