// Port of klippy/mcu.py MCU_endstop / TriggerDispatch / MCU_trsync (see
// mcu_endstop.hpp).
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "klippy/mcu_endstop.hpp"

#include <cstdio>
#include <cstdlib>

#include "chelper_api.h"
#include "klippy/printer.hpp"
#include "mcu_link/mcu.hpp"

namespace kl {

// mcu.py TRSYNC_SINGLE_MCU_TIMEOUT
static constexpr double TRSYNC_SINGLE_MCU_TIMEOUT = 0.250;

McuEndstop::McuEndstop(Mcu &mcu, std::string pin, int pullup, bool invert,
                       std::function<void(PrinterStepper &)> note_homing_end)
    : mcu_(mcu), pin_(std::move(pin)), pullup_(pullup), invert_(invert),
      note_homing_end_(std::move(note_homing_end))
{
    oid_ = mcu_.create_oid();
    trsync_oid_ = mcu_.create_oid();
    td_ = trdispatch_alloc();
    mcu_.register_config_callback([this] { build_config(); });
}

McuEndstop::~McuEndstop()
{
    free(tdm_);
    free(td_);
}

void
McuEndstop::add_stepper(PrinterStepper *s)
{
    for (PrinterStepper *e : steppers_)
        if (e == s)
            return;
    steppers_.push_back(s);
}

void
McuEndstop::build_config()
{
    char buf[200];
    // MCU_endstop._build_config()
    snprintf(buf, sizeof(buf), "config_endstop oid=%d pin=%s pull_up=%d", oid_,
             pin_.c_str(), pullup_);
    mcu_.add_config_cmd(buf);
    snprintf(buf, sizeof(buf), "endstop_home oid=%d clock=0 sample_ticks=0"
             " sample_count=0 rest_ticks=0 pin_value=0 trsync_oid=0"
             " trigger_reason=0", oid_);
    mcu_.add_config_cmd(buf, false, true);
    // MCU_trsync._build_config()
    snprintf(buf, sizeof(buf), "config_trsync oid=%d", trsync_oid_);
    mcu_.add_config_cmd(buf);
    snprintf(buf, sizeof(buf), "trsync_start oid=%d report_clock=0"
             " report_ticks=0 expire_reason=0", trsync_oid_);
    mcu_.add_config_cmd(buf, false, true);
    cq_ = mcu_.serial().alloc_command_queue();
    tdm_ = trdispatch_mcu_alloc(
        td_, mcu_.serial().get_serialqueue(), cq_, trsync_oid_,
        mcu_.command_tag("trsync_set_timeout oid=%c clock=%u"),
        mcu_.command_tag("trsync_trigger oid=%c reason=%c"),
        mcu_.command_tag("trsync_state oid=%c can_trigger=%c"
                         " trigger_reason=%c clock=%u"));
}

bool
McuEndstop::send(const char *fmt, const std::vector<int64_t> &args,
                 uint64_t minclock, uint64_t reqclock, std::string *err)
{
    const MsgFormat *f = mcu_.lookup_command(fmt, err);
    if (!f)
        return false;
    std::vector<MsgArg> a(args.begin(), args.end());
    std::vector<uint8_t> cmd;
    if (!mcu_.parser().encode(*f, a, &cmd, err))
        return false;
    mcu_.serial().raw_send(cmd, minclock, reqclock, cq_);
    return true;
}

void
McuEndstop::handle_trsync_state(const MsgParams &p)
{
    // MCU_trsync._handle_trsync_state() (serial thread)
    if (!p.get("can_trigger")) {
        int expected = 0;
        int reason = (int)p.get("trigger_reason");
        state_.compare_exchange_strong(
            expected, reason >= REASON_COMMS_TIMEOUT ? 2 : 1);
        return;
    }
    int64_t end = home_end_clock_.load();
    if (end >= 0) {
        int64_t clock = mcu_.clocksync().clock32_to_clock64(
            (uint32_t)p.get("clock"));
        if (clock >= end) {
            home_end_clock_ = -1;
            std::string err;
            send("trsync_trigger oid=%c reason=%c",
                 {trsync_oid_, REASON_PAST_END_TIME}, 0, 0, &err);
        }
    }
}

bool
McuEndstop::home_start(double print_time, double sample_time, int sample_count,
                       double rest_time, bool triggered, std::string *err)
{
    uint64_t clock = (uint64_t)mcu_.print_time_to_clock(print_time);
    if (getenv("KL_TRACE_SD"))
        fprintf(stderr, "home_start %s at pt %.3f (est %.3f)\n", pin_.c_str(),
                print_time, mcu_.estimated_print_time(get_monotonic()));
    rest_ticks_ = mcu_.print_time_to_clock(print_time + rest_time) - clock;
    // TriggerDispatch.start() -> MCU_trsync.start()
    state_ = 0;
    home_end_clock_ = -1;
    double expire_timeout = TRSYNC_SINGLE_MCU_TIMEOUT;
    int64_t expire_ticks = mcu_.seconds_to_clock(expire_timeout);
    uint64_t expire_clock = clock + expire_ticks;
    int64_t report_ticks = mcu_.seconds_to_clock(expire_timeout * .3);
    uint64_t report_clock = clock; // report_offset 0 (single MCU)
    int64_t min_extend_ticks = (int64_t)(report_ticks * .8 + .5);
    trdispatch_mcu_setup(tdm_, clock, expire_clock, expire_ticks,
                         min_extend_ticks);
    mcu_.serial().register_response(
        "trsync_state", trsync_oid_,
        [this](const MsgParams &p) { handle_trsync_state(p); });
    if (!send("trsync_start oid=%c report_clock=%u report_ticks=%u"
              " expire_reason=%c",
              {trsync_oid_, (int64_t)(report_clock & 0xffffffff), report_ticks,
               REASON_COMMS_TIMEOUT}, 0, clock, err))
        return false;
    for (PrinterStepper *s : steppers_)
        if (!send("stepper_stop_on_trigger oid=%c trsync_oid=%c",
                  {s->oid, trsync_oid_}, 0, 0, err))
            return false;
    if (!send("trsync_set_timeout oid=%c clock=%u",
              {trsync_oid_, (int64_t)(expire_clock & 0xffffffff)}, 0, clock,
              err))
        return false;
    trdispatch_start(td_, REASON_HOST_REQUEST);
    // MCU_endstop.home_start(): endstop_home
    return send("endstop_home oid=%c clock=%u sample_ticks=%u sample_count=%c"
                " rest_ticks=%u pin_value=%c trsync_oid=%c trigger_reason=%c",
                {oid_, (int64_t)(clock & 0xffffffff),
                 mcu_.seconds_to_clock(sample_time), sample_count, rest_ticks_,
                 (int64_t)(triggered ^ invert_), trsync_oid_,
                 REASON_ENDSTOP_HIT}, 0, clock, err);
}

bool
McuEndstop::home_wait(double home_end_time,
                      const std::function<void(double)> &pause,
                      double *trigger_time, std::string *err)
{
    // TriggerDispatch.wait_end(): trigger "past end time" once the MCU
    // reports a clock after the end of the move, then wait
    home_end_clock_ = mcu_.print_time_to_clock(home_end_time);
    while (state_ == 0 && !mcu_.is_shutdown())
        pause(0.005);
    send("endstop_home oid=%c clock=%u sample_ticks=%u sample_count=%c"
         " rest_ticks=%u pin_value=%c trsync_oid=%c trigger_reason=%c",
         {oid_, 0, 0, 0, 0, 0, 0, 0}, 0, 0, err);
    // TriggerDispatch.stop() -> MCU_trsync.stop()
    trdispatch_stop(td_);
    mcu_.serial().register_response("trsync_state", trsync_oid_, nullptr);
    std::vector<uint8_t> cmd;
    const MsgFormat *f = mcu_.lookup_command("trsync_trigger oid=%c reason=%c",
                                             err);
    if (!f || !mcu_.parser().encode(*f, {MsgArg(trsync_oid_), MsgArg((int)REASON_HOST_REQUEST)},
                                    &cmd, err))
        return false;
    MsgParams p;
    if (!mcu_.serial().query(cmd, "trsync_state", trsync_oid_, &p, err, cq_))
        return false;
    for (PrinterStepper *s : steppers_)
        note_homing_end_(*s);
    int64_t res = p.get("trigger_reason");
    if (res >= REASON_COMMS_TIMEOUT) {
        *err = "Communication timeout during homing";
        return false;
    }
    if (res != REASON_ENDSTOP_HIT) {
        *trigger_time = 0.;
        return true;
    }
    // MCU_endstop.home_wait(): trigger time from the endstop state
    std::string qerr;
    if (!mcu_.serial().send_with_response(
            "endstop_query_state oid=" + std::to_string(oid_), "endstop_state",
            &p, err, oid_))
        return false;
    int64_t next_clock = mcu_.clocksync().clock32_to_clock64(
        (uint32_t)p.get("next_clock"));
    *trigger_time = mcu_.clocksync().clock_to_print_time(
        (double)(next_clock - rest_ticks_));
    return true;
}

bool
McuEndstop::query_endstop(double print_time, int *triggered, std::string *err)
{
    std::vector<uint8_t> cmd;
    const MsgFormat *f = mcu_.lookup_command("endstop_query_state oid=%c", err);
    if (!f || !mcu_.parser().encode(*f, {oid_}, &cmd, err))
        return false;
    MsgParams p;
    uint64_t clock = (uint64_t)mcu_.print_time_to_clock(print_time);
    if (!mcu_.serial().query(cmd, "endstop_state", oid_, &p, err, cq_, clock))
        return false;
    *triggered = (int)(p.get("pin_value") ^ invert_);
    return true;
}

} // namespace kl
