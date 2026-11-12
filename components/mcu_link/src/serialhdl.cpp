// Port of klippy/serialhdl.py (see serialhdl.hpp).
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "mcu_link/serialhdl.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>

#include "chelper_api.h"

#ifdef ESP_PLATFORM
#include "esp_pthread.h"
#include "sdkconfig.h"
#endif

extern "C" void serialqueue_trim_debug(struct serialqueue *sq, int keep);

namespace kl {

// Messages kept in serialqueue's debug history (Klipper: 100 + 100, about
// 22 KB; kept in full on boards with PSRAM or off the ESP32)
#if defined(ESP_PLATFORM) && !CONFIG_SPIRAM
static constexpr int SERIALQUEUE_DEBUG_KEEP = 8;
#else
static constexpr int SERIALQUEUE_DEBUG_KEEP = 100;
#endif

#ifdef ESP_PLATFORM
// Stack sizes for the two threads created here.  serialqueue's
// background thread keeps a 4 KB buffer on its stack (kick_event) and was
// measured at ~5 KB peak; the response thread at ~1.3 KB.  Restores the
// caller's settings on scope exit.
struct ThreadCfgScope {
    esp_pthread_cfg_t saved;
    bool have_saved;
    ThreadCfgScope(const char *name, int prio, size_t stack) {
        have_saved = esp_pthread_get_cfg(&saved) == ESP_OK;
        esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
        cfg.stack_size = stack;
        cfg.prio = prio;
        cfg.thread_name = name;
        esp_pthread_set_cfg(&cfg);
    }
    ~ThreadCfgScope() {
        if (have_saved)
            esp_pthread_set_cfg(&saved);
    }
};
#endif

SerialHdl::SerialHdl(const char *name) : name_(name)
{
    default_queue_ = serialqueue_alloc_commandqueue();
}

SerialHdl::~SerialHdl()
{
    disconnect();
    serialqueue_free_commandqueue(default_queue_);
}

command_queue *
SerialHdl::alloc_command_queue()
{
    return serialqueue_alloc_commandqueue();
}

void
SerialHdl::free_command_queue(command_queue *cq)
{
    serialqueue_free_commandqueue(cq);
}

bool
SerialHdl::connect_fd(int fd, std::string *err, double timeout)
{
    disconnect();
    char sqname[16] = {};
    snprintf(sqname, sizeof(sqname), "serialq %s", name_.c_str());
    {
#ifdef ESP_PLATFORM
        ThreadCfgScope tcs("serialq", 20, 6656);
#endif
        sq_ = serialqueue_alloc(fd, 'u', 0, sqname);
        if (!sq_) {
            if (err)
                *err = "serialqueue_alloc failed";
            return false;
        }
        serialqueue_trim_debug(sq_, SERIALQUEUE_DEBUG_KEEP);
#ifdef ESP_PLATFORM
        ThreadCfgScope tcs2("serialhdl", 19, 3072);
#endif
        bg_ = std::thread([this] { bg_thread(); });
    }
    {
        std::lock_guard<std::mutex> g(notify_lock_);
        can_wait_acks_ = true;
    }
    std::string data;
    if (!get_identify_data(&data, err, timeout)) {
        disconnect();
        return false;
    }
    std::string json;
    if (!zlib_inflate((const uint8_t *)data.data(), data.size(), &json)) {
        if (err)
            *err = "Unable to decompress the MCU data dictionary";
        disconnect();
        return false;
    }
    MessageParser np;
    if (!np.process_identify(json, err)) {
        disconnect();
        return false;
    }
    {
        std::lock_guard<std::recursive_mutex> g(handlers_lock_);
        parser_ = std::move(np);
    }
    double wire_freq = parser_.get_constant_float("SERIAL_BAUD", 0.);
    if (wire_freq > 0.)
        serialqueue_set_wire_frequency(sq_, wire_freq);
    int64_t rw = parser_.get_constant_int("RECEIVE_WINDOW", 0);
    if (rw > 0)
        serialqueue_set_receive_window(sq_, (int)rw);
    return true;
}

bool
SerialHdl::get_identify_data(std::string *data, std::string *err,
                             double timeout)
{
    double deadline = get_monotonic() + timeout;
    data->clear();
    while (get_monotonic() < deadline) {
        char msg[64];
        snprintf(msg, sizeof(msg), "identify offset=%u count=%d",
                 (unsigned)data->size(), 40);
        MsgParams params;
        std::string qerr;
        if (!send_with_response(msg, "identify_response", &params, &qerr)) {
            continue;
        }
        if ((size_t)params.get("offset") == data->size()) {
            std::string chunk = params.get_str("data");
            if (chunk.empty())
                return true;
            *data += chunk;
        }
    }
    if (err)
        *err = "Timeout on connect (no identify response)";
    return false;
}

void
SerialHdl::disconnect()
{
    if (sq_) {
        last_stats_ = stats();
        serialqueue_exit(sq_);
        if (bg_.joinable())
            bg_.join();
        serialqueue_free(sq_);
        sq_ = nullptr;
    }
    abort_wait_acks();
    std::lock_guard<std::mutex> g(notify_lock_);
    pending_.clear();
    can_wait_acks_ = true;
}

void
SerialHdl::bg_thread()
{
    pull_queue_message pqm;
    for (;;) {
        serialqueue_pull(sq_, &pqm);
        if (pqm.len < 0)
            break;
        if (pqm.notify_id) {
            std::lock_guard<std::mutex> g(notify_lock_);
            auto it = pending_.find(pqm.notify_id);
            if (it != pending_.end()) {
                it->second.done = it->second.ok = true;
                it->second.sent_time = pqm.sent_time;
                it->second.receive_time = pqm.receive_time;
                notify_cv_.notify_all();
            }
            continue;
        }
        std::lock_guard<std::recursive_mutex> g(handlers_lock_);
        MsgParams params;
        std::string err;
        if (!parser_.parse(pqm.msg, pqm.len, &params, &err)) {
            fprintf(stderr, "%s: bad message: %s\n", name_.c_str(), err.c_str());
            continue;
        }
        params.sent_time = pqm.sent_time;
        params.receive_time = pqm.receive_time;
        int oid = params.has("oid") ? (int)params.get("oid") : -1;
        auto it = handlers_.find({params.name, oid});
        if (it != handlers_.end()) {
            it->second(params);
        } else if (default_handler_) {
            default_handler_(params);
        } else if (params.name == "#output") {
            fprintf(stderr, "%s: %s\n", name_.c_str(), params.output.c_str());
        }
    }
}

void
SerialHdl::register_response(const std::string &name, int oid, Handler h)
{
    std::lock_guard<std::recursive_mutex> g(handlers_lock_);
    if (h)
        handlers_[{name, oid}] = std::move(h);
    else
        handlers_.erase({name, oid});
}

void
SerialHdl::set_default_handler(Handler h)
{
    std::lock_guard<std::recursive_mutex> g(handlers_lock_);
    default_handler_ = std::move(h);
}

void
SerialHdl::raw_send(const std::vector<uint8_t> &cmd, uint64_t minclock,
                    uint64_t reqclock, command_queue *cq)
{
    if (!sq_)
        return;
    serialqueue_send(sq_, cq ? cq : default_queue_,
                     const_cast<uint8_t *>(cmd.data()), (int)cmd.size(),
                     minclock, reqclock, 0);
}

bool
SerialHdl::raw_send_wait_ack(const std::vector<uint8_t> &cmd,
                             uint64_t minclock, uint64_t reqclock,
                             command_queue *cq, double *sent_time,
                             double *receive_time, double timeout)
{
    if (!sq_)
        return false;
    uint64_t nid;
    {
        std::lock_guard<std::mutex> g(notify_lock_);
        if (!can_wait_acks_)
            return false;
        nid = ++last_notify_id_;
        pending_[nid] = Notify();
    }
    serialqueue_send(sq_, cq ? cq : default_queue_,
                     const_cast<uint8_t *>(cmd.data()), (int)cmd.size(),
                     minclock, reqclock, nid);
    std::unique_lock<std::mutex> lk(notify_lock_);
    bool done = wait_with_hook(lk, notify_cv_, timeout,
                               [&] { return pending_[nid].done; });
    Notify n = pending_[nid];
    pending_.erase(nid);
    if (!done || !n.ok)
        return false;
    if (sent_time)
        *sent_time = n.sent_time;
    if (receive_time)
        *receive_time = n.receive_time;
    return true;
}

namespace {
// Per-thread identity that also works for plain FreeRTOS tasks
// (std::this_thread::get_id() asserts outside pthreads on ESP-IDF)
const void *
this_thread_tag()
{
    static thread_local char tag;
    return &tag;
}
} // namespace

void
SerialHdl::set_wait_hook(std::function<void()> hook)
{
    wait_hook_ = std::move(hook);
    hook_thread_ = this_thread_tag();
}

template <typename Pred>
bool
SerialHdl::wait_with_hook(std::unique_lock<std::mutex> &lk,
                          std::condition_variable &cv, double timeout,
                          Pred pred)
{
    bool use_hook = wait_hook_ && !in_hook_
                    && this_thread_tag() == hook_thread_;
    if (!use_hook)
        return cv.wait_for(lk, std::chrono::duration<double>(timeout), pred);
    double deadline = get_monotonic() + timeout;
    while (!pred()) {
        double left = deadline - get_monotonic();
        if (left <= 0.)
            return pred();
        if (cv.wait_for(lk, std::chrono::duration<double>(std::min(left, 0.005)),
                        pred))
            return true;
        lk.unlock();
        in_hook_ = true;
        wait_hook_();
        in_hook_ = false;
        lk.lock();
    }
    return true;
}

void
SerialHdl::abort_wait_acks()
{
    std::lock_guard<std::mutex> g(notify_lock_);
    can_wait_acks_ = false;
    for (auto &kv : pending_)
        kv.second.done = true; // ok stays false
    notify_cv_.notify_all();
}

bool
SerialHdl::send(const std::string &text, std::string *err)
{
    std::vector<uint8_t> cmd;
    if (!parser_.create_command(text, &cmd, err))
        return false;
    raw_send(cmd, 0, 0, default_queue_);
    return true;
}

bool
SerialHdl::query(const std::vector<uint8_t> &cmd, const std::string &resp,
                 int oid, MsgParams *out, std::string *err, command_queue *cq,
                 uint64_t minclock, uint64_t reqclock)
{
    // SerialRetryCommand.get_response()
    struct Slot {
        std::mutex m;
        bool have = false;
        MsgParams p;
    };
    auto slot = std::make_shared<Slot>();
    register_response(resp, oid, [slot](const MsgParams &p) {
        std::lock_guard<std::mutex> g(slot->m);
        slot->p = p;
        slot->have = true;
    });
    int retries = 5;
    double retry_delay = .010;
    bool ok = false;
    for (;;) {
        bool acked = raw_send_wait_ack(cmd, minclock, reqclock, cq, nullptr,
                                       nullptr);
        {
            std::lock_guard<std::mutex> g(slot->m);
            if (slot->have) {
                *out = slot->p;
                ok = true;
                break;
            }
        }
        if (!acked && !can_wait_acks_) {
            if (err)
                *err = "No communication with MCU";
            break;
        }
        if (retries-- <= 0) {
            if (err)
                *err = "Unable to obtain '" + resp + "' response";
            break;
        }
        std::this_thread::sleep_for(std::chrono::duration<double>(retry_delay));
        retry_delay *= 2.;
    }
    register_response(resp, oid, Handler());
    return ok;
}

bool
SerialHdl::query_async(const std::vector<uint8_t> &cmd, const std::string &resp,
                       int oid, MsgParams *out, std::string *err,
                       command_queue *cq, uint64_t minclock)
{
    static constexpr double TIMEOUT_TIME = 5.0, RETRY_TIME = 0.500;
    struct Slot {
        std::mutex m;
        std::condition_variable cv;
        bool have = false;
        double min_query_time;
        MsgParams p;
    };
    auto slot = std::make_shared<Slot>();
    slot->min_query_time = get_monotonic();
    register_response(resp, oid, [slot](const MsgParams &p) {
        // Only responses to queries sent after we started
        std::lock_guard<std::mutex> g(slot->m);
        if (!slot->have && p.sent_time >= slot->min_query_time) {
            slot->p = p;
            slot->have = true;
            slot->cv.notify_all();
        }
    });
    bool ok = false;
    if (raw_send_wait_ack(cmd, minclock, minclock, cq, nullptr, nullptr)) {
        {
            std::lock_guard<std::mutex> g(slot->m);
            slot->min_query_time = 0.;
        }
        double first = get_monotonic();
        for (;;) {
            std::unique_lock<std::mutex> lk(slot->m);
            if (wait_with_hook(lk, slot->cv, RETRY_TIME,
                               [&] { return slot->have; })) {
                *out = slot->p;
                ok = true;
                break;
            }
            lk.unlock();
            if (get_monotonic() > first + TIMEOUT_TIME || !can_wait_acks_) {
                if (err)
                    *err = "Timeout on wait for '" + resp + "' response";
                break;
            }
            raw_send(cmd, minclock, minclock, cq);
        }
    } else if (err) {
        *err = "No communication with MCU";
    }
    register_response(resp, oid, Handler());
    return ok;
}

bool
SerialHdl::send_with_response(const std::string &text, const std::string &resp,
                              MsgParams *out, std::string *err, int oid)
{
    std::vector<uint8_t> cmd;
    {
        std::lock_guard<std::recursive_mutex> g(handlers_lock_);
        if (!parser_.create_command(text, &cmd, err))
            return false;
    }
    return query(cmd, resp, oid, out, err);
}

void
SerialHdl::set_clock_est(double freq, double conv_time, int64_t conv_clock)
{
    if (sq_)
        serialqueue_set_clock_est(sq_, freq, conv_time, (uint64_t)conv_clock);
}

std::string
SerialHdl::dump_debug()
{
    // klippy serialhdl.py dump_debug(): the last messages sent / received
    if (!sq_)
        return "";
    std::string out = "Dumping serial stats: " + stats() + "\n";
    std::vector<pull_queue_message> q(SERIALQUEUE_DEBUG_KEEP);
    char line[160];
    for (int sent = 1; sent >= 0; sent--) {
        int n = serialqueue_extract_old(sq_, sent, q.data(), (int)q.size());
        snprintf(line, sizeof(line), "Dumping %s queue %d messages\n",
                 sent ? "send" : "receive", n);
        out += line;
        for (int i = 0; i < n; i++) {
            const auto &m = q[i];
            snprintf(line, sizeof(line), "%s %02d %f %f %02d: ",
                     sent ? "Sent" : "Receive:", i, m.receive_time,
                     m.sent_time, m.len);
            out += line;
            auto cmds = parser_.dump(m.msg, m.len);
            for (size_t j = 0; j < cmds.size(); j++)
                out += (j ? ", " : "") + cmds[j];
            out += "\n";
        }
    }
    return out;
}

std::string
SerialHdl::stats()
{
    if (!sq_)
        return "";
    char buf[512];
    serialqueue_get_stats(sq_, buf, sizeof(buf));
    return buf;
}

} // namespace kl
