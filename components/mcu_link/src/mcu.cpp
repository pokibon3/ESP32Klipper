// Port of the connection/config parts of klippy/mcu.py (see mcu.hpp).
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "mcu_link/mcu.hpp"

#include <cstdio>

#include "chelper_api.h"

namespace kl {

std::function<void(const char *)> debug_stage_hook;

uint32_t
crc32_ieee(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xffffffff;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xedb88320 & -(crc & 1));
    }
    return ~crc;
}

Mcu::Mcu(const char *name) : name_(name), serial_(name) {}

Mcu::~Mcu()
{
    disconnect();
}

void
Mcu::add_config_cmd(const std::string &cmd, bool is_init, bool on_restart)
{
    if (is_init)
        init_cmds_.push_back(cmd);
    else if (on_restart)
        restart_cmds_.push_back(cmd);
    else
        config_cmds_.push_back(cmd);
}

int32_t
Mcu::command_tag(const std::string &format) const
{
    const MsgFormat *mf = lookup_command(format);
    return mf ? mf->msgid : -1;
}

void
Mcu::finalize_config()
{
    if (finalized_)
        return;
    for (auto &cb : config_callbacks_)
        cb();
    finalized_ = true;
    config_cmds_.insert(config_cmds_.begin(),
                        "allocate_oids count=" + std::to_string(oid_count_));
    // Pin names are passed through to the MCU enumeration as-is (the
    // "pins" module of klippy only resolves aliases, unused here).
    std::string encoded;
    for (size_t i = 0; i < config_cmds_.size(); i++) {
        if (i)
            encoded += '\n';
        encoded += config_cmds_[i];
    }
    config_crc_ = crc32_ieee(encoded.data(), encoded.size());
    config_cmds_.push_back("finalize_config crc=" + std::to_string(config_crc_));
}

bool
Mcu::send_get_config(MsgParams *out, std::string *err)
{
    if (!serial_.send_with_response("get_config", "config", out, err))
        return false;
    if (is_shutdown_) {
        if (err)
            *err = "MCU '" + name_ + "' error during config: " + shutdown_msg_;
        return false;
    }
    if (out->get("is_shutdown")) {
        if (err)
            *err = "Can not update MCU '" + name_ + "' config as it is shutdown";
        return false;
    }
    return true;
}

void
Mcu::handle_shutdown(const MsgParams &p)
{
    if (is_shutdown_.exchange(true))
        return;
    shutdown_msg_ = p.get_str("static_string_id");
    double pt = p.has("clock")
        ? clocksync_.clock_to_print_time(
              (double)clocksync_.clock32_to_clock64((uint32_t)p.get("clock")))
        : -1.;
    fprintf(stderr, "MCU '%s' shutdown: %s (%s) at print_time %.3f\n",
            name_.c_str(), shutdown_msg_.c_str(), p.name.c_str(), pt);
    // The dump needs more stack than the serial thread has: tick() prints it
    dump_pending_ = true;
    if (shutdown_cb_)
        shutdown_cb_(shutdown_msg_);
}

bool
Mcu::connect(const std::string &port, int baud, std::string *err)
{
    debug_stage("start");
    fd_ = mcu_open_serial(port, baud, err);
    if (fd_ < 0)
        return false;
    debug_stage("serial port open");
    if (!serial_.connect_fd(fd_, err)) {
        disconnect();
        return false;
    }
    debug_stage("identify (serialqueue + data dictionary)");
    if (!clocksync_.connect(serial_, err)) {
        disconnect();
        return false;
    }
    debug_stage("clock sync");
    auto sh = [this](const MsgParams &p) { handle_shutdown(p); };
    serial_.register_response("shutdown", -1, sh);
    serial_.register_response("is_shutdown", -1, sh);
    serial_.register_response("starting", -1, [this](const MsgParams &) {
        if (!is_shutdown_)
            fprintf(stderr, "MCU '%s' spontaneous restart\n", name_.c_str());
    });

    // mcu.py MCU._connect()
    MsgParams cfg;
    if (!send_get_config(&cfg, err))
        return false;
    std::vector<std::string> cmds;
    finalize_config();
    if (!cfg.get("is_config")) {
        cmds = config_cmds_;
        cmds.insert(cmds.end(), init_cmds_.begin(), init_cmds_.end());
    } else {
        if ((uint32_t)cfg.get("crc") != config_crc_) {
            if (err)
                *err = "MCU '" + name_ + "' CRC does not match config"
                       " (restart the MCU)";
            return false;
        }
        cmds = restart_cmds_;
        cmds.insert(cmds.end(), init_cmds_.begin(), init_cmds_.end());
    }
    for (const auto &c : cmds) {
        std::string e;
        if (!serial_.send(c, &e)) {
            if (err)
                *err = "MCU '" + name_ + "': " + e + " (" + c + ")";
            return false;
        }
    }
    debug_stage("config commands sent");
    if (!send_get_config(&cfg, err))
        return false;
    if (!cfg.get("is_config")) {
        if (err)
            *err = "Unable to configure MCU '" + name_ + "'";
        return false;
    }
    move_count_ = (int)cfg.get("move_count");
    fprintf(stderr, "Configured MCU '%s' (%d moves), %zu commands, %s\n",
            name_.c_str(), move_count_, parser().message_count(),
            parser().version().c_str());
    return true;
}

void
Mcu::tick(double eventtime)
{
    clocksync_.tick(eventtime);
    if (dump_pending_.exchange(false))
        fprintf(stderr, "%s\n%s", clocksync_.dump_debug().c_str(),
                serial_.dump_debug().c_str());
    if (!clocksync_.is_active() && !is_timeout_ && serial_.connected()) {
        is_timeout_ = true;
        fprintf(stderr, "Lost communication with MCU '%s'\n", name_.c_str());
        serial_.abort_wait_acks();
        if (shutdown_cb_)
            shutdown_cb_("Lost communication with MCU");
    }
}

void
Mcu::emergency_stop()
{
    serial_.send("emergency_stop");
}

bool
Mcu::reset()
{
    if (!lookup_command("reset"))
        return false;
    serial_.send("reset");
    return true;
}

void
Mcu::disconnect()
{
    serial_.disconnect();
    if (fd_ >= 0) {
        mcu_close_serial(fd_);
        fd_ = -1;
    }
}

} // namespace kl
