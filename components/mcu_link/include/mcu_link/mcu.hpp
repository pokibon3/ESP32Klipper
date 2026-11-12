// Port of the connection/config parts of klippy/mcu.py: open the serial
// port, identify, clock sync, and send the printer configuration
// (allocate_oids ... finalize_config crc=...).
//
// This file may be distributed under the terms of the GNU GPLv3 license.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "mcu_link/clocksync.hpp"
#include "mcu_link/serialhdl.hpp"

namespace kl {

// Open a serial device for the MCU link.  On the host this is a tty/pty
// path; on the ESP32 "uart:<num>:<tx>:<rx>" sets up a UART (see
// mcu_port_esp.cpp).  Returns a non-blocking fd or -1.
int mcu_open_serial(const std::string &port, int baud, std::string *err);
void mcu_close_serial(int fd);

class Mcu {
public:
    explicit Mcu(const char *name = "mcu");
    ~Mcu();

    // --- configuration phase (before connect) ---
    int create_oid() { return oid_count_++; }
    // Callbacks run in finalize, before the commands are sent
    // (mcu.register_config_callback); they add config commands.
    void register_config_callback(std::function<void()> cb) {
        config_callbacks_.push_back(std::move(cb));
    }
    void add_config_cmd(const std::string &cmd, bool is_init = false,
                        bool on_restart = false);

    // --- connection ---
    // identify + clock sync + send config.  Blocks a few seconds.
    bool connect(const std::string &port, int baud, std::string *err);
    void disconnect();
    // Call regularly (every ~100 ms) from the host loop: clock queries
    // and communication timeout detection.
    void tick(double eventtime);

    // --- runtime ---
    SerialHdl &serial() { return serial_; }
    const MessageParser &parser() const { return serial_.parser(); }
    ClockSync &clocksync() { return clocksync_; }
    const MsgFormat *lookup_command(const std::string &format,
                                    std::string *err = nullptr) const {
        return parser().lookup_command(format, err);
    }
    int32_t command_tag(const std::string &format) const;
    int move_count() const { return move_count_; }
    double mcu_freq() const { return clocksync_.mcu_freq(); }
    int64_t seconds_to_clock(double t) const { return (int64_t)(t * mcu_freq()); }
    int64_t print_time_to_clock(double pt) const {
        return clocksync_.print_time_to_clock(pt);
    }
    double estimated_print_time(double eventtime) const {
        return clocksync_.estimated_print_time(eventtime);
    }
    bool is_shutdown() const { return is_shutdown_; }
    const std::string &shutdown_msg() const { return shutdown_msg_; }
    uint32_t config_crc() const { return config_crc_; }
    // Called (from the serial thread) when the MCU reports a shutdown
    void set_shutdown_callback(std::function<void(const std::string &)> cb) {
        shutdown_cb_ = std::move(cb);
    }
    // emergency_stop
    void emergency_stop();
    // FIRMWARE_RESTART (restart_method "command"): the MCU resets itself
    // and drops the connection.  False if its firmware has no "reset".
    bool reset();

private:
    bool send_get_config(MsgParams *out, std::string *err);
    void finalize_config();
    void handle_shutdown(const MsgParams &p);

    std::string name_;
    SerialHdl serial_;
    ClockSync clocksync_;
    int fd_ = -1;
    int oid_count_ = 0;
    std::vector<std::function<void()>> config_callbacks_;
    std::vector<std::string> config_cmds_, restart_cmds_, init_cmds_;
    bool finalized_ = false;
    uint32_t config_crc_ = 0;
    int move_count_ = 0;
    std::atomic<bool> is_shutdown_{false};
    std::atomic<bool> dump_pending_{false}; // shutdown debug dump for tick()
    std::string shutdown_msg_;
    std::function<void(const std::string &)> shutdown_cb_;
    bool is_timeout_ = false;
};

// Diagnostics: called with a stage name at points of the connect sequence
// (tests record heap use per stage).  Empty by default.
extern std::function<void(const char *)> debug_stage_hook;
inline void debug_stage(const char *stage) {
    if (debug_stage_hook)
        debug_stage_hook(stage);
}

// zlib.crc32 (used for the config CRC)
uint32_t crc32_ieee(const void *data, size_t len);

} // namespace kl
