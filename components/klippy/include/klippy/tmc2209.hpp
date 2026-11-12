// Port of klippy/extras/tmc2209.py (+ tmc2208.py, tmc.py, tmc_uart.py and
// tmc2130.TMCCurrentHelper): TMC2209 drivers on a bit-banged MCU UART.
//
// Register values are computed from printer.cfg exactly like Klipper
// (same field defaults, current and threshold formulas, register write
// order) and written at connect and on every motor enable, each write
// verified through IFCNT.  DRV_STATUS/GSTAT are checked once a second
// while the motor is enabled.
//
// Not ported yet: phase offset tracking (endstop_phase), stallguard dump.
//
// This file may be distributed under the terms of the GNU GPLv3 license.
#pragma once

#include <cstdint>
#include <functional>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "klippy/configfile.hpp"
#include "klippy/gcode.hpp"

struct command_queue;

namespace kl {

class Mcu;
struct PrinterStepper;

// tmc_uart.py MCU_TMC_uart_bitbang: one per uart pin pair, shared by the
// drivers on it (SKR Pico: X, Y, Z, E on gpio9/gpio8, addresses 0-3)
class TmcUartBus {
public:
    TmcUartBus(Mcu &mcu, std::string rx_pin, int pullup, std::string tx_pin);
    void build_config();
    bool reg_read(int addr, int reg, uint32_t *val, std::string *err);
    bool reg_write(int addr, int reg, uint32_t val, std::string *err);
    const std::string &rx_pin() const { return rx_pin_; }
    const std::string &tx_pin() const { return tx_pin_; }

private:
    bool send(const std::vector<uint8_t> &data, int read_len,
              std::string *out, std::string *err);
    Mcu &mcu_;
    std::mutex lock_; // one transfer at a time (shared by the drivers)
    std::string rx_pin_, tx_pin_;
    int pullup_, oid_;
    command_queue *cq_ = nullptr;
};

class Tmc2209 {
public:
    // Reads [tmc2209 <stepper>]; stepper_sec is the stepper's section
    static Tmc2209 *create(ConfigSection sec, ConfigSection stepper_sec,
                           Mcu &mcu, std::vector<TmcUartBus *> &buses,
                           std::vector<std::unique_ptr<TmcUartBus>> &owned,
                           std::string *err);

    const std::string &stepper_name() const { return stepper_name_; }
    const std::string &diag_pin() const { return diag_pin_; }

    // klippy:connect (after the stepper config is known)
    void handle_connect(bool step_both_edge, std::string *log);
    // Motor enable/disable (stepper_enable state callback)
    bool do_enable(std::string *err);
    void do_disable();
    // tmc.py TMCVirtualPinHelper: homing with the virtual endstop needs
    // stealthChop and TCOOLTHRS so StallGuard reports on DIAG
    bool homing_move_begin(std::string *err);
    bool homing_move_end(std::string *err);
    // Periodic DRV_STATUS / GSTAT check; false = driver error.  Called
    // from a background thread (TMCErrorCheck timer in klippy)
    bool periodic_check(double now, std::string *err);
    void register_commands(GCodeDispatch &gcode,
                           std::function<double()> wait_moves_print_time);

    // Register access by name
    bool set_register(const std::string &reg, uint32_t val, std::string *err);
    bool get_register(const std::string &reg, uint32_t *val, std::string *err);
    uint32_t field(const std::string &name) const;
    uint32_t set_field(const std::string &name, uint32_t value);
    const std::vector<std::pair<std::string, uint32_t>> &registers() const {
        return regs_;
    }
    bool init_registers(std::string *err);
    // Requested currents (tmc.py get_status run_current / hold_current)
    double run_current() const { return run_current_; }
    double hold_current() const { return hold_current_; }
    uint32_t last_drv_status() const { return last_drv_status_; }

private:
    Tmc2209() = default;
    bool set_config_field(ConfigSection &sec, const std::string &field,
                          int64_t def);
    void set_current(double run_current, double hold_current);
    bool query_register(const char *reg, uint32_t mask, uint32_t err_mask,
                        bool try_clear, uint32_t *cleared, std::string *err);

    void virtual_set_field(const std::string &f, uint32_t v);
    bool send_dirty(std::string *err);
    std::vector<std::pair<std::string, uint32_t>> prev_state_, dirty_regs_;
    std::string stepper_name_, name_, diag_pin_;
    TmcUartBus *bus_ = nullptr;
    int addr_ = 0;
    std::vector<std::pair<std::string, uint32_t>> regs_; // write order
    double sense_resistor_ = 0.110, req_hold_current_ = 2.;
    double step_dist_ = 0.;
    double run_current_ = 0., hold_current_ = 0.;
    int64_t ifcnt_ = -1;
    // Register access may come from the G-code task and the background
    // check thread (MCU_TMC_uart.mutex in klippy)
    std::recursive_mutex reg_lock_;
    std::atomic<bool> checks_active_{false};
    std::atomic<double> next_check_{0.};
    uint32_t last_drv_status_ = 0;
};

} // namespace kl
