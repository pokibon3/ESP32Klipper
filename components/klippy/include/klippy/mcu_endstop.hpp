// Port of klippy/mcu.py MCU_endstop, TriggerDispatch and MCU_trsync for a
// single MCU: an endstop pin that stops its steppers on the MCU (trsync)
// during homing moves.  Trigger messages are handled by Klipper's
// trdispatch.c in the serial thread, like in klippy.
//
// This file may be distributed under the terms of the GNU GPLv3 license.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

struct trdispatch;
struct trdispatch_mcu;
struct command_queue;

namespace kl {

class Mcu;
struct MsgParams;
struct PrinterStepper;

class McuEndstop {
public:
    enum { REASON_ENDSTOP_HIT = 1, REASON_HOST_REQUEST = 2,
           REASON_PAST_END_TIME = 3, REASON_COMMS_TIMEOUT = 4 };

    // pin: MCU pin name; note_homing_end is called for each stepper when
    // the homing move ends (stepper.note_homing_end)
    McuEndstop(Mcu &mcu, std::string pin, int pullup, bool invert,
               std::function<void(PrinterStepper &)> note_homing_end);
    ~McuEndstop();

    void add_stepper(PrinterStepper *s);
    const std::vector<PrinterStepper *> &steppers() const { return steppers_; }

    // MCU_endstop.home_start()
    bool home_start(double print_time, double sample_time, int sample_count,
                    double rest_time, bool triggered, std::string *err);
    // Completion state (trigger_completion.test())
    bool done() const { return state_ != 0; }
    // MCU_endstop.home_wait(): wait for the trigger (or the end of the
    // move), stop, and return the trigger print time (0 = not triggered)
    bool home_wait(double home_end_time,
                   const std::function<void(double)> &pause,
                   double *trigger_time, std::string *err);
    // MCU_endstop.query_endstop()
    bool query_endstop(double print_time, int *triggered, std::string *err);

private:
    void build_config();
    void handle_trsync_state(const MsgParams &p);
    bool send(const char *fmt, const std::vector<int64_t> &args,
              uint64_t minclock, uint64_t reqclock, std::string *err);

    Mcu &mcu_;
    std::string pin_;
    int pullup_;
    bool invert_;
    std::function<void(PrinterStepper &)> note_homing_end_;
    int oid_, trsync_oid_;
    command_queue *cq_ = nullptr;
    trdispatch *td_ = nullptr;
    trdispatch_mcu *tdm_ = nullptr;
    std::vector<PrinterStepper *> steppers_;
    int64_t rest_ticks_ = 0;
    // 0 = waiting, 1 = triggered/ended, 2 = failure (comms timeout)
    std::atomic<int> state_{1};
    std::atomic<int64_t> home_end_clock_{-1};
};

} // namespace kl
