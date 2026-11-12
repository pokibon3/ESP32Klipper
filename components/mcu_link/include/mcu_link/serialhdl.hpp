// Port of klippy/serialhdl.py: one serial session with a Klipper MCU.
//
// Wraps Klipper's chelper serialqueue (framing, sequence numbers, acks,
// retransmit, clock-scheduled sends - compiled unmodified) and adds the
// data dictionary download ("identify") and response dispatch.  A
// background thread pulls received messages and calls the registered
// handlers, like serialhdl.py's _bg_thread.
//
// This file may be distributed under the terms of the GNU GPLv3 license.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "mcu_link/msgproto.hpp"

struct serialqueue;
struct command_queue;

namespace kl {

class SerialHdl {
public:
    using Handler = std::function<void(const MsgParams &)>;

    explicit SerialHdl(const char *name = "mcu");
    ~SerialHdl();
    SerialHdl(const SerialHdl &) = delete;
    SerialHdl &operator=(const SerialHdl &) = delete;

    // Start a session on an open, non-blocking serial fd and download the
    // data dictionary ("u" = uart/pty).  Returns false on timeout.
    bool connect_fd(int fd, std::string *err, double timeout = 5.);
    void disconnect();
    bool connected() const { return sq_ != nullptr; }

    const MessageParser &parser() const { return parser_; }
    serialqueue *get_serialqueue() { return sq_; }
    command_queue *default_queue() { return default_queue_; }
    command_queue *alloc_command_queue();
    void free_command_queue(command_queue *cq);

    // Handlers are keyed by (response name, oid); oid -1 = no oid.
    // An empty handler removes the registration.
    void register_response(const std::string &name, int oid, Handler h);
    // Handler for responses without a specific registration
    void set_default_handler(Handler h);

    void raw_send(const std::vector<uint8_t> &cmd, uint64_t minclock,
                  uint64_t reqclock, command_queue *cq);
    // Send and wait until the MCU acknowledged it.  Returns the
    // send/receive times, or false when communication was aborted.
    bool raw_send_wait_ack(const std::vector<uint8_t> &cmd, uint64_t minclock,
                           uint64_t reqclock, command_queue *cq,
                           double *sent_time, double *receive_time,
                           double timeout = 5.);
    // Encode "name a=1 ..." and send on the default queue
    bool send(const std::string &text, std::string *err = nullptr);
    // serialhdl.send_with_response(): send a query and wait for the
    // named response (retried like SerialRetryCommand)
    bool send_with_response(const std::string &text, const std::string &resp,
                            MsgParams *out, std::string *err, int oid = -1);
    bool query(const std::vector<uint8_t> &cmd, const std::string &resp,
               int oid, MsgParams *out, std::string *err,
               command_queue *cq = nullptr, uint64_t minclock = 0,
               uint64_t reqclock = 0);

    // mcu.py RetryAsyncCommand: for commands whose response may arrive
    // after the ack (tmcuart_send).  Waits up to 0.5 s for the response
    // before resending, gives up after 5 s.
    bool query_async(const std::vector<uint8_t> &cmd, const std::string &resp,
                     int oid, MsgParams *out, std::string *err,
                     command_queue *cq = nullptr, uint64_t minclock = 0);

    void set_clock_est(double freq, double conv_time, int64_t conv_clock);
    // Work to keep doing while the calling (klippy) thread waits for an
    // MCU response: klippy's reactor keeps running its timers (step
    // flushing, clock sync) while a command waits; this is the
    // equivalent.  Only called on the thread that installed it.
    void set_wait_hook(std::function<void()> hook);
    // Wake up all waiters with an error (communication lost)
    void abort_wait_acks();
    std::string stats();
    // Serial stats and the last messages sent and received (for shutdowns)
    std::string dump_debug();
    // stats() of the last session, kept by disconnect() for diagnostics
    const std::string &last_stats() const { return last_stats_; }

private:
    void bg_thread();
    // Wait on cv until pred() or timeout, running the wait hook every few
    // milliseconds when on the hook's thread
    template <typename Pred>
    bool wait_with_hook(std::unique_lock<std::mutex> &lk,
                        std::condition_variable &cv, double timeout, Pred pred);
    std::function<void()> wait_hook_;
    const void *hook_thread_ = nullptr; // thread_local marker address
    bool in_hook_ = false;
    bool get_identify_data(std::string *data, std::string *err, double timeout);

    std::string name_;
    MessageParser parser_;
    serialqueue *sq_ = nullptr;
    command_queue *default_queue_ = nullptr;
    std::thread bg_;

    std::recursive_mutex handlers_lock_;
    std::map<std::pair<std::string, int>, Handler> handlers_;
    Handler default_handler_;

    struct Notify {
        bool done = false, ok = false;
        double sent_time = 0., receive_time = 0.;
    };
    std::mutex notify_lock_;
    std::condition_variable notify_cv_;
    std::map<uint64_t, Notify> pending_;
    uint64_t last_notify_id_ = 0;
    bool can_wait_acks_ = true;
    std::string last_stats_;
};

} // namespace kl
