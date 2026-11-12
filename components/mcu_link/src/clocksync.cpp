// Port of klippy/clocksync.py ClockSync (see clocksync.hpp).
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "mcu_link/clocksync.hpp"

#include <cmath>
#include <cstdio>
#include <thread>

#include "mcu_link/serialhdl.hpp"

namespace kl {

static constexpr double RTT_AGE = .000010 / (60. * 60.);
static constexpr double DECAY = 1. / 30.;
static constexpr double TRANSMIT_EXTRA = .001;
static constexpr double QUERY_INTERVAL = .9839;

bool
ClockSync::connect(SerialHdl &serial, std::string *err)
{
    serial_ = &serial;
    mcu_freq_ = serial.parser().get_constant_float("CLOCK_FREQ");
    if (mcu_freq_ <= 0.) {
        if (err)
            *err = "MCU did not report CLOCK_FREQ";
        return false;
    }
    MsgParams params;
    if (!serial.send_with_response("get_uptime", "uptime", &params, err))
        return false;
    {
        std::lock_guard<std::mutex> g(lock_);
        last_clock_ = (params.get("high") << 32) | (uint32_t)params.get("clock");
        clock_avg_ = (double)last_clock_;
        time_avg_ = params.sent_time;
        est_time_ = time_avg_;
        est_clock_ = clock_avg_;
        est_freq_ = mcu_freq_;
        prediction_variance_ = (.001 * mcu_freq_) * (.001 * mcu_freq_);
    }
    for (int i = 0; i < 8; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        last_prediction_time_ = -9999.;
        if (!serial.send_with_response("get_clock", "clock", &params, err))
            return false;
        handle_clock(params);
    }
    serial.register_response("clock", -1,
                             [this](const MsgParams &p) { handle_clock(p); });
    next_query_time_ = 0.;
    return true;
}

void
ClockSync::tick(double eventtime)
{
    if (!serial_ || eventtime < next_query_time_)
        return;
    serial_->send("get_clock");
    {
        std::lock_guard<std::mutex> g(lock_);
        queries_pending_++;
    }
    next_query_time_ = eventtime + QUERY_INTERVAL;
}

bool
ClockSync::update_regression(double sent_time, double clock)
{
    double old_freq = est_freq_;
    double exp_clock = (sent_time - time_avg_) * old_freq + clock_avg_;
    double clock_diff2 = (clock - exp_clock) * (clock - exp_clock);
    if (clock_diff2 > 25. * prediction_variance_
        && clock_diff2 > (.000500 * mcu_freq_) * (.000500 * mcu_freq_)) {
        if (clock > exp_clock && sent_time < last_prediction_time_ + 10.)
            return false; // ignore outlier
        prediction_variance_ = (.001 * mcu_freq_) * (.001 * mcu_freq_);
    } else {
        last_prediction_time_ = sent_time;
        prediction_variance_ = (1. - DECAY)
            * (prediction_variance_ + clock_diff2 * DECAY);
    }
    double diff_sent_time = sent_time - time_avg_;
    time_avg_ += DECAY * diff_sent_time;
    time_variance_ = (1. - DECAY)
        * (time_variance_ + diff_sent_time * diff_sent_time * DECAY);
    double diff_clock = clock - clock_avg_;
    clock_avg_ += DECAY * diff_clock;
    clock_covariance_ = (1. - DECAY)
        * (clock_covariance_ + diff_sent_time * diff_clock * DECAY);
    return true;
}

void
ClockSync::update_best_rtt(double sent_time, double receive_time)
{
    double half_rtt = .5 * (receive_time - sent_time);
    double aged_rtt = (sent_time - min_rtt_time_) * RTT_AGE;
    if (half_rtt < min_half_rtt_ + aged_rtt) {
        min_half_rtt_ = half_rtt;
        min_rtt_time_ = sent_time;
    }
}

void
ClockSync::handle_clock(const MsgParams &params)
{
    double new_freq, conv_time;
    int64_t conv_clock;
    {
        std::lock_guard<std::mutex> g(lock_);
        queries_pending_ = 0;
        int64_t clock_delta = ((uint32_t)params.get("clock")
                               - (uint32_t)last_clock_) & 0xffffffff;
        int64_t clock = last_clock_ = last_clock_ + clock_delta;
        double sent_time = params.sent_time;
        if (sent_time == 0.)
            return;
        if (!update_regression(sent_time, (double)clock))
            return;
        new_freq = clock_covariance_ / time_variance_;
        double pred_stddev = std::sqrt(prediction_variance_);
        conv_time = time_avg_ + TRANSMIT_EXTRA;
        conv_clock = (int64_t)(clock_avg_ - 3. * pred_stddev);
        update_best_rtt(sent_time, params.receive_time);
        est_time_ = time_avg_ + min_half_rtt_;
        est_clock_ = clock_avg_;
        est_freq_ = new_freq;
    }
    serial_->set_clock_est(new_freq, conv_time, conv_clock);
}

int64_t
ClockSync::get_clock(double eventtime) const
{
    std::lock_guard<std::mutex> g(lock_);
    return (int64_t)(est_clock_ + (eventtime - est_time_) * est_freq_);
}

double
ClockSync::estimate_clock_systime(double reqclock) const
{
    std::lock_guard<std::mutex> g(lock_);
    return (reqclock - est_clock_) / est_freq_ + est_time_;
}

int64_t
ClockSync::clock32_to_clock64(uint32_t clock32) const
{
    std::lock_guard<std::mutex> g(lock_);
    int64_t clock_diff = (clock32 - (uint32_t)last_clock_) & 0xffffffff;
    clock_diff -= (clock_diff & 0x80000000) << 1;
    return last_clock_ + clock_diff;
}

std::string
ClockSync::dump_debug() const
{
    std::lock_guard<std::mutex> g(lock_);
    char buf[256];
    snprintf(buf, sizeof(buf),
             "clocksync state: mcu_freq=%d last_clock=%lld"
             " clock_est=(%.3f %.0f %.3f) min_half_rtt=%.6f",
             (int)mcu_freq_, (long long)last_clock_, est_time_, est_clock_,
             est_freq_, min_half_rtt_);
    return buf;
}

} // namespace kl
