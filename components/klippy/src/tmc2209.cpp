// Port of klippy/extras/tmc2209.py and helpers (see tmc2209.hpp).
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "klippy/tmc2209.hpp"

#include <cmath>
#include <cstdio>
#include <map>

#include "chelper_api.h"
#include "mcu_link/mcu.hpp"

namespace kl {

static constexpr double TMC_FREQUENCY = 12000000.;
static constexpr double MAX_CURRENT = 2.000;
static constexpr int TMC_BAUD_RATE = 40000;

/****************************************************************
 * Register and field tables (tmc2208.py / tmc2209.py)
 ****************************************************************/

static const std::map<std::string, int> REGISTERS = {
    {"GCONF", 0x00}, {"GSTAT", 0x01}, {"IFCNT", 0x02}, {"SLAVECONF", 0x03},
    {"OTP_PROG", 0x04}, {"OTP_READ", 0x05}, {"IOIN", 0x06},
    {"FACTORY_CONF", 0x07}, {"IHOLD_IRUN", 0x10}, {"TPOWERDOWN", 0x11},
    {"TSTEP", 0x12}, {"TPWMTHRS", 0x13}, {"TCOOLTHRS", 0x14},
    {"VACTUAL", 0x22}, {"SGTHRS", 0x40}, {"SG_RESULT", 0x41},
    {"COOLCONF", 0x42}, {"MSCNT", 0x6a}, {"MSCURACT", 0x6b},
    {"CHOPCONF", 0x6c}, {"DRV_STATUS", 0x6f}, {"PWMCONF", 0x70},
    {"PWM_SCALE", 0x71}, {"PWM_AUTO", 0x72},
};

static const char *READ_REGISTERS[] = {
    "GCONF", "GSTAT", "IFCNT", "OTP_READ", "IOIN", "FACTORY_CONF", "TSTEP",
    "MSCNT", "MSCURACT", "CHOPCONF", "DRV_STATUS", "PWMCONF", "PWM_SCALE",
    "PWM_AUTO", "SG_RESULT",
};

struct FieldDef {
    const char *reg;
    uint32_t mask;
};

static const std::map<std::string, FieldDef> FIELDS = {
    // GCONF
    {"i_scale_analog", {"GCONF", 0x01}}, {"internal_rsense", {"GCONF", 1u << 1}},
    {"en_spreadcycle", {"GCONF", 1u << 2}}, {"shaft", {"GCONF", 1u << 3}},
    {"index_otpw", {"GCONF", 1u << 4}}, {"index_step", {"GCONF", 1u << 5}},
    {"pdn_disable", {"GCONF", 1u << 6}}, {"mstep_reg_select", {"GCONF", 1u << 7}},
    {"multistep_filt", {"GCONF", 1u << 8}}, {"test_mode", {"GCONF", 1u << 9}},
    // GSTAT
    {"reset", {"GSTAT", 0x01}}, {"drv_err", {"GSTAT", 1u << 1}},
    {"uv_cp", {"GSTAT", 1u << 2}},
    {"ifcnt", {"IFCNT", 0xff}},
    {"senddelay", {"SLAVECONF", 0x0fu << 8}},
    // IHOLD_IRUN
    {"ihold", {"IHOLD_IRUN", 0x1f}}, {"irun", {"IHOLD_IRUN", 0x1fu << 8}},
    {"iholddelay", {"IHOLD_IRUN", 0x0fu << 16}},
    {"tpowerdown", {"TPOWERDOWN", 0xff}},
    {"tstep", {"TSTEP", 0xfffff}},
    {"tpwmthrs", {"TPWMTHRS", 0xfffff}},
    {"tcoolthrs", {"TCOOLTHRS", 0xfffff}},
    {"vactual", {"VACTUAL", 0xffffff}},
    {"mscnt", {"MSCNT", 0x3ff}},
    // CHOPCONF
    {"toff", {"CHOPCONF", 0x0f}}, {"hstrt", {"CHOPCONF", 0x07u << 4}},
    {"hend", {"CHOPCONF", 0x0fu << 7}}, {"tbl", {"CHOPCONF", 0x03u << 15}},
    {"vsense", {"CHOPCONF", 1u << 17}}, {"mres", {"CHOPCONF", 0x0fu << 24}},
    {"intpol", {"CHOPCONF", 1u << 28}}, {"dedge", {"CHOPCONF", 1u << 29}},
    {"diss2g", {"CHOPCONF", 1u << 30}}, {"diss2vs", {"CHOPCONF", 1u << 31}},
    // DRV_STATUS
    {"otpw", {"DRV_STATUS", 0x01}}, {"ot", {"DRV_STATUS", 1u << 1}},
    {"s2ga", {"DRV_STATUS", 1u << 2}}, {"s2gb", {"DRV_STATUS", 1u << 3}},
    {"s2vsa", {"DRV_STATUS", 1u << 4}}, {"s2vsb", {"DRV_STATUS", 1u << 5}},
    {"ola", {"DRV_STATUS", 1u << 6}}, {"olb", {"DRV_STATUS", 1u << 7}},
    {"t120", {"DRV_STATUS", 1u << 8}}, {"t143", {"DRV_STATUS", 1u << 9}},
    {"t150", {"DRV_STATUS", 1u << 10}}, {"t157", {"DRV_STATUS", 1u << 11}},
    {"cs_actual", {"DRV_STATUS", 0x1fu << 16}},
    {"stealth", {"DRV_STATUS", 1u << 30}}, {"stst", {"DRV_STATUS", 1u << 31}},
    // PWMCONF
    {"pwm_ofs", {"PWMCONF", 0xff}}, {"pwm_grad", {"PWMCONF", 0xffu << 8}},
    {"pwm_freq", {"PWMCONF", 0x03u << 16}},
    {"pwm_autoscale", {"PWMCONF", 1u << 18}},
    {"pwm_autograd", {"PWMCONF", 1u << 19}},
    {"freewheel", {"PWMCONF", 0x03u << 20}}, {"pwm_reg", {"PWMCONF", 0xfu << 24}},
    {"pwm_lim", {"PWMCONF", 0xfu << 28}},
    // COOLCONF (tmc2209.py)
    {"semin", {"COOLCONF", 0x0f}}, {"seup", {"COOLCONF", 0x03u << 5}},
    {"semax", {"COOLCONF", 0x0fu << 8}}, {"sedn", {"COOLCONF", 0x03u << 13}},
    {"seimin", {"COOLCONF", 1u << 15}},
    {"sgthrs", {"SGTHRS", 0xff}},
    {"sg_result", {"SG_RESULT", 0x3ff}},
};

static int
ffs32(uint32_t mask)
{
    int n = 0;
    while (mask && !(mask & 1)) {
        mask >>= 1;
        n++;
    }
    return n;
}

static uint32_t
field_mask(const std::string &f, const char **reg)
{
    auto it = FIELDS.find(f);
    if (it == FIELDS.end())
        return 0;
    if (reg)
        *reg = it->second.reg;
    return it->second.mask;
}

/****************************************************************
 * TmcUartBus (tmc_uart.py MCU_TMC_uart_bitbang)
 ****************************************************************/

static uint8_t
calc_crc8(const uint8_t *data, int len)
{
    uint8_t crc = 0;
    for (int i = 0; i < len; i++) {
        uint8_t b = data[i];
        for (int j = 0; j < 8; j++) {
            if ((crc >> 7) ^ (b & 0x01))
                crc = (uint8_t)((crc << 1) ^ 0x07);
            else
                crc = (uint8_t)(crc << 1);
            b >>= 1;
        }
    }
    return crc;
}

static std::vector<uint8_t>
add_serial_bits(const std::vector<uint8_t> &data)
{
    std::vector<uint8_t> out((data.size() * 10 + 7) / 8, 0);
    for (size_t i = 0; i < data.size(); i++) {
        uint32_t b = ((uint32_t)data[i] << 1) | 0x200;
        for (int k = 0; k < 10; k++)
            if (b & (1u << k)) {
                size_t pos = i * 10 + k;
                out[pos / 8] |= (uint8_t)(1 << (pos % 8));
            }
    }
    return out;
}

static std::vector<uint8_t>
encode_read(uint8_t sync, uint8_t addr, uint8_t reg)
{
    std::vector<uint8_t> msg = {sync, addr, reg};
    msg.push_back(calc_crc8(msg.data(), 3));
    return add_serial_bits(msg);
}

static std::vector<uint8_t>
encode_write(uint8_t sync, uint8_t addr, uint8_t reg, uint32_t val)
{
    std::vector<uint8_t> msg = {sync, addr, reg, (uint8_t)(val >> 24),
                                (uint8_t)(val >> 16), (uint8_t)(val >> 8),
                                (uint8_t)val};
    msg.push_back(calc_crc8(msg.data(), 7));
    return add_serial_bits(msg);
}

static bool
decode_read(uint8_t reg, const std::string &data, uint32_t *val)
{
    if (data.size() != 10)
        return false;
    // Bits of byte k of the reply start at bit 10*k+1 of the stream
    auto bits = [&](int pos) {
        uint32_t v = 0;
        for (int b = 0; b < 8; b++) {
            int p = pos + b;
            v |= (uint32_t)(((uint8_t)data[p / 8] >> (p % 8)) & 1) << b;
        }
        return v;
    };
    uint32_t v = (bits(31) << 24) | (bits(41) << 16) | (bits(51) << 8)
                 | bits(61);
    std::vector<uint8_t> enc = encode_write(0x05, 0xff, reg, v);
    if (std::string(enc.begin(), enc.end()) != data)
        return false;
    *val = v;
    return true;
}

TmcUartBus::TmcUartBus(Mcu &mcu, std::string rx_pin, int pullup,
                       std::string tx_pin)
    : mcu_(mcu), rx_pin_(std::move(rx_pin)), tx_pin_(std::move(tx_pin)),
      pullup_(pullup)
{
    oid_ = mcu_.create_oid();
    mcu_.register_config_callback([this] { build_config(); });
}

void
TmcUartBus::build_config()
{
    int64_t bit_ticks = mcu_.seconds_to_clock(1. / TMC_BAUD_RATE);
    char buf[128];
    snprintf(buf, sizeof(buf),
             "config_tmcuart oid=%d rx_pin=%s pull_up=%d tx_pin=%s bit_time=%d",
             oid_, rx_pin_.c_str(), pullup_, tx_pin_.c_str(), (int)bit_ticks);
    mcu_.add_config_cmd(buf);
}

bool
TmcUartBus::send(const std::vector<uint8_t> &data, int read_len,
                 std::string *out, std::string *err)
{
    std::lock_guard<std::mutex> g(lock_);
    if (!cq_)
        cq_ = mcu_.serial().alloc_command_queue();
    const MsgFormat *f = mcu_.lookup_command(
        "tmcuart_send oid=%c write=%*s read=%c", err);
    if (!f)
        return false;
    std::vector<uint8_t> cmd;
    if (!mcu_.parser().encode(*f, {oid_, std::string(data.begin(), data.end()),
                                   read_len}, &cmd, err))
        return false;
    MsgParams p;
    // lookup_query_command(..., is_async=True): the response comes after the
    // UART transfer, i.e. after the ack
    if (!mcu_.serial().query_async(cmd, "tmcuart_response", oid_, &p, err, cq_))
        return false;
    *out = p.get_str("read");
    return true;
}

bool
TmcUartBus::reg_read(int addr, int reg, uint32_t *val, std::string *err)
{
    std::string data;
    if (!send(encode_read(0xf5, (uint8_t)addr, (uint8_t)reg), 10, &data, err))
        return false;
    if (!decode_read((uint8_t)reg, data, val)) {
        if (err)
            *err = "bad tmc uart reply";
        return false;
    }
    return true;
}

bool
TmcUartBus::reg_write(int addr, int reg, uint32_t val, std::string *err)
{
    std::string data;
    return send(encode_write(0xf5, (uint8_t)addr, (uint8_t)(reg | 0x80), val),
                0, &data, err);
}

/****************************************************************
 * Tmc2209
 ****************************************************************/

uint32_t
Tmc2209::field(const std::string &name) const
{
    const char *reg;
    uint32_t mask = field_mask(name, &reg);
    for (const auto &r : regs_)
        if (r.first == reg)
            return (r.second & mask) >> ffs32(mask);
    return 0;
}

uint32_t
Tmc2209::set_field(const std::string &name, uint32_t value)
{
    // tmc.py FieldHelper.set_field(): registers keep first-set order
    const char *reg;
    uint32_t mask = field_mask(name, &reg);
    for (auto &r : regs_)
        if (r.first == reg) {
            r.second = (r.second & ~mask) | ((value << ffs32(mask)) & mask);
            return r.second;
        }
    uint32_t v = (value << ffs32(mask)) & mask;
    regs_.emplace_back(reg, v);
    return v;
}

bool
Tmc2209::set_config_field(ConfigSection &sec, const std::string &f,
                          int64_t def)
{
    uint32_t mask = field_mask(f, nullptr);
    uint32_t maxval = mask >> ffs32(mask);
    std::string opt = "driver_" + f; // option names are case-insensitive
    int64_t v;
    if (maxval == 1)
        v = sec.getboolean(opt, def != 0);
    else
        v = sec.getint(opt, def, 0, maxval);
    set_field(f, (uint32_t)v);
    return true;
}

// tmc2130.py TMCCurrentHelper
static int
current_bits(double sense_resistor, double current, bool vsense)
{
    double vref = vsense ? 0.18 : 0.32;
    int cs = (int)(32. * (sense_resistor + 0.020) * current * std::sqrt(2.)
                   / vref + .5) - 1;
    return std::max(0, std::min(31, cs));
}

static double
current_from_bits(double sense_resistor, int cs, bool vsense)
{
    double vref = vsense ? 0.18 : 0.32;
    return (cs + 1) * vref / (32. * (sense_resistor + 0.020) * std::sqrt(2.));
}

void
Tmc2209::set_current(double run_current, double hold_current)
{
    run_current_ = run_current;
    hold_current_ = hold_current;
    bool vsense = true;
    int irun = current_bits(sense_resistor_, run_current, true);
    if (irun == 31) {
        double cur = current_from_bits(sense_resistor_, irun, true);
        if (cur < run_current) {
            int irun2 = current_bits(sense_resistor_, run_current, false);
            double cur2 = current_from_bits(sense_resistor_, irun2, false);
            if (std::fabs(run_current - cur2) < std::fabs(run_current - cur)) {
                vsense = false;
                irun = irun2;
            }
        }
    }
    int ihold = current_bits(sense_resistor_, std::min(hold_current, run_current),
                             vsense);
    set_field("vsense", vsense);
    set_field("ihold", (uint32_t)ihold);
    set_field("irun", (uint32_t)irun);
}

// tmc.py TMCtstepHelper()
static uint32_t
tstep_threshold(double step_dist, uint32_t mres, double velocity)
{
    if (velocity <= 0.)
        return 0xfffff;
    double step_dist_256 = step_dist / (1 << mres);
    int64_t threshold = (int64_t)(TMC_FREQUENCY * step_dist_256 / velocity + .5);
    return (uint32_t)std::max<int64_t>(0, std::min<int64_t>(0xfffff, threshold));
}

Tmc2209 *
Tmc2209::create(ConfigSection sec, ConfigSection ssec, Mcu &mcu,
                std::vector<TmcUartBus *> &buses,
                std::vector<std::unique_ptr<TmcUartBus>> &owned,
                std::string *err)
{
    std::unique_ptr<Tmc2209> t(new Tmc2209());
    const std::string &sname = sec.get_name();
    t->stepper_name_ = sname.substr(sname.find(' ') + 1);
    t->name_ = sname.substr(sname.rfind(' ') + 1);

    // tmc_uart.py lookup_tmc_uart_bitbang()
    std::string rx = sec.get("uart_pin");
    int pullup = 0;
    if (!rx.empty() && (rx[0] == '^' || rx[0] == '~')) {
        pullup = rx[0] == '^' ? 1 : -1;
        rx = rx.substr(1);
    }
    std::string tx = sec.get("tx_pin", rx);
    if (sec.has("select_pins")) {
        *err = "TMC select_pins is not supported (" + sname + ")";
        return nullptr;
    }
    t->addr_ = (int)sec.getint("uart_address", 0, 0, 3);
    for (TmcUartBus *b : buses)
        if (b->rx_pin() == rx) {
            if (b->tx_pin() != tx) {
                *err = "Shared TMC uarts must use the same pins";
                return nullptr;
            }
            t->bus_ = b;
        }
    if (!t->bus_) {
        owned.emplace_back(new TmcUartBus(mcu, rx, pullup, tx));
        t->bus_ = owned.back().get();
        buses.push_back(t->bus_);
    }

    // tmc2209.py TMC2209.__init__ (same order of set_field calls)
    t->set_field("pdn_disable", 1);
    t->set_field("senddelay", 2);
    // TMCVirtualPinHelper
    t->diag_pin_ = sec.get("diag_pin", "");
    // TMCCurrentHelper
    double run_current = sec.getfloat("run_current", 0., NAN, MAX_CURRENT, 0.);
    if (!sec.has("run_current"))
        sec.get("run_current");
    double hold_current = sec.getfloat("hold_current", MAX_CURRENT, NAN,
                                       MAX_CURRENT, 0.);
    t->req_hold_current_ = hold_current;
    t->sense_resistor_ = sec.getfloat("sense_resistor", 0.110, NAN, NAN, 0.);
    t->set_current(run_current, hold_current);
    // TMCMicrostepHelper
    static const std::map<int64_t, uint32_t> steps = {
        {256, 0}, {128, 1}, {64, 2}, {32, 3}, {16, 4}, {8, 5}, {4, 6},
        {2, 7}, {1, 8}};
    int64_t microsteps = ssec.getint("microsteps", 16);
    auto ms = steps.find(microsteps);
    if (ms == steps.end()) {
        *err = "Invalid microsteps in [" + ssec.get_name() + "]";
        return nullptr;
    }
    t->set_field("mres", ms->second);
    t->set_field("intpol", sec.getboolean("interpolate", true));
    t->set_field("mstep_reg_select", 1);
    // TMCStealthchopHelper / TMCVcoolthrsHelper
    double rotation_distance = ssec.getfloat("rotation_distance", 0.);
    double full_steps = (double)ssec.getint("full_steps_per_rotation", 200);
    t->step_dist_ = rotation_distance / (full_steps * (double)microsteps);
    uint32_t mres = t->field("mres");
    bool en_pwm_mode = false;
    uint32_t tpwmthrs = 0xfffff;
    if (sec.has("stealthchop_threshold")) {
        double v = sec.getfloat("stealthchop_threshold", 0., 0.);
        en_pwm_mode = true;
        tpwmthrs = tstep_threshold(t->step_dist_, mres, v);
    }
    t->set_field("tpwmthrs", tpwmthrs);
    t->set_field("en_spreadcycle", !en_pwm_mode);
    uint32_t tcoolthrs = 0;
    if (sec.has("coolstep_threshold"))
        tcoolthrs = tstep_threshold(t->step_dist_, mres,
                                    sec.getfloat("coolstep_threshold", 0., 0.));
    t->set_field("tcoolthrs", tcoolthrs);
    // Config fields with their defaults
    t->set_config_field(sec, "multistep_filt", 1);
    t->set_config_field(sec, "toff", 3);
    t->set_config_field(sec, "hstrt", 5);
    t->set_config_field(sec, "hend", 0);
    t->set_config_field(sec, "tbl", 2);
    t->set_config_field(sec, "semin", 0);
    t->set_config_field(sec, "seup", 0);
    t->set_config_field(sec, "semax", 0);
    t->set_config_field(sec, "sedn", 0);
    t->set_config_field(sec, "seimin", 0);
    t->set_config_field(sec, "iholddelay", 8);
    t->set_config_field(sec, "pwm_ofs", 36);
    t->set_config_field(sec, "pwm_grad", 14);
    t->set_config_field(sec, "pwm_freq", 1);
    t->set_config_field(sec, "pwm_autoscale", 1);
    t->set_config_field(sec, "pwm_autograd", 1);
    t->set_config_field(sec, "freewheel", 0);
    t->set_config_field(sec, "pwm_reg", 8);
    t->set_config_field(sec, "pwm_lim", 12);
    t->set_config_field(sec, "tpowerdown", 20);
    t->set_config_field(sec, "sgthrs", 0);
    return t.release();
}

bool
Tmc2209::get_register(const std::string &reg_name, uint32_t *val,
                      std::string *err)
{
    // tmc_uart.py MCU_TMC_uart._do_get_register(): up to 5 tries
    std::lock_guard<std::recursive_mutex> g(reg_lock_);
    int reg = REGISTERS.at(reg_name);
    std::string e;
    for (int retry = 0; retry < 5; retry++)
        if (bus_->reg_read(addr_, reg, val, &e))
            return true;
    if (err)
        *err = "Unable to read tmc uart '" + name_ + "' register " + reg_name;
    return false;
}

bool
Tmc2209::set_register(const std::string &reg_name, uint32_t val,
                      std::string *err)
{
    // MCU_TMC_uart.set_register(): write, then confirm IFCNT advanced
    std::lock_guard<std::recursive_mutex> g(reg_lock_);
    int reg = REGISTERS.at(reg_name);
    for (int retry = 0; retry < 5; retry++) {
        uint32_t v;
        if (ifcnt_ < 0) {
            if (!get_register("IFCNT", &v, err))
                return false;
            ifcnt_ = v & 0xff;
        }
        int64_t prev = ifcnt_;
        std::string e;
        bus_->reg_write(addr_, reg, val, &e);
        if (!get_register("IFCNT", &v, err))
            return false;
        ifcnt_ = v & 0xff;
        if (ifcnt_ == ((prev + 1) & 0xff))
            return true;
    }
    if (err)
        *err = "Unable to write tmc uart '" + name_ + "' register " + reg_name;
    return false;
}

bool
Tmc2209::init_registers(std::string *err)
{
    for (size_t i = 0; i < regs_.size(); i++)
        if (!set_register(regs_[i].first, regs_[i].second, err))
            return false;
    return true;
}

void
Tmc2209::handle_connect(bool step_both_edge, std::string *log)
{
    if (step_both_edge)
        set_field("dedge", 1);
    std::string err;
    if (!init_registers(&err) && log)
        *log = "TMC " + name_ + " failed to init: " + err;
}

bool
Tmc2209::query_register(const char *reg, uint32_t mask, uint32_t err_mask,
                        bool try_clear, uint32_t *cleared, std::string *err)
{
    // tmc.py TMCErrorCheck._query_register() (no cs_actual check on 2209)
    int count = 0;
    for (;;) {
        uint32_t val;
        if (!get_register(reg, &val, err)) {
            if (++count < 3)
                continue;
            return false;
        }
        (void)mask;
        if (!(val & err_mask))
            return true;
        if (++count >= 3) {
            char buf[64];
            snprintf(buf, sizeof(buf), "%s: %08x", reg, (unsigned)val);
            if (err)
                *err = "TMC '" + stepper_name_ + "' reports error: " + buf;
            return false;
        }
        if (try_clear) {
            try_clear = false;
            *cleared |= val & err_mask;
            if (!set_register(reg, val & err_mask, err))
                return false;
        }
    }
}

bool
Tmc2209::do_enable(std::string *err)
{
    // TMCCommandHelper._do_enable() + TMCErrorCheck.start_checks()
    if (!init_registers(err))
        return false;
    uint32_t drv_err = field_mask("ot", nullptr) | field_mask("s2ga", nullptr)
        | field_mask("s2gb", nullptr) | field_mask("s2vsa", nullptr)
        | field_mask("s2vsb", nullptr);
    uint32_t cleared = 0;
    if (!query_register("DRV_STATUS", 0, drv_err, false, &cleared, err)
        || !query_register("GSTAT", 0, 0xffffffff, true, &cleared, err))
        return false;
    checks_active_ = true;
    next_check_ = get_monotonic() + 1.;
    return true;
}

void
Tmc2209::do_disable()
{
    checks_active_ = false;
}

bool
Tmc2209::periodic_check(double now, std::string *err)
{
    if (!checks_active_ || now < next_check_)
        return true;
    next_check_ = now + 1.;
    uint32_t drv_err = field_mask("ot", nullptr) | field_mask("s2ga", nullptr)
        | field_mask("s2gb", nullptr) | field_mask("s2vsa", nullptr)
        | field_mask("s2vsb", nullptr);
    uint32_t cleared = 0;
    if (!query_register("DRV_STATUS", 0, drv_err, false, &cleared, err)
        || !query_register("GSTAT", 0, 0xffffffff, false, &cleared, err)) {
        checks_active_ = false;
        return false;
    }
    return true;
}

void
Tmc2209::virtual_set_field(const std::string &f, uint32_t v)
{
    // TMCVirtualPinHelper._set_field()
    bool seen = false;
    for (auto &ps : prev_state_)
        if (ps.first == f) {
            ps.second = field(f);
            seen = true;
        }
    if (!seen)
        prev_state_.emplace_back(f, field(f));
    const char *reg;
    field_mask(f, &reg);
    uint32_t val = set_field(f, v);
    for (auto &d : dirty_regs_)
        if (d.first == reg) {
            d.second = val;
            return;
        }
    dirty_regs_.emplace_back(reg, val);
}

bool
Tmc2209::send_dirty(std::string *err)
{
    bool ok = true;
    for (auto &d : dirty_regs_)
        if (ok && !set_register(d.first, d.second, err))
            ok = false;
    dirty_regs_.clear();
    return ok;
}

bool
Tmc2209::homing_move_begin(std::string *err)
{
    // TMC2209 has no en_pwm_mode: StallGuard4 works in stealthChop
    virtual_set_field("tpwmthrs", 0);
    virtual_set_field("en_spreadcycle", 0);
    if (field("tcoolthrs") == 0)
        virtual_set_field("tcoolthrs", 0xfffff);
    return send_dirty(err);
}

bool
Tmc2209::homing_move_end(std::string *err)
{
    std::vector<std::pair<std::string, uint32_t>> prev;
    prev.swap(prev_state_);
    for (auto &ps : prev)
        virtual_set_field(ps.first, ps.second);
    prev_state_.clear();
    return send_dirty(err);
}

void
Tmc2209::register_commands(GCodeDispatch &gcode,
                           std::function<double()> get_last_move_time)
{
    gcode.register_mux_command(
        "SET_TMC_CURRENT", "STEPPER", name_,
        [this, &gcode, get_last_move_time](GCodeCommand &g, std::string *err) {
            int irun = (int)field("irun"), ihold = (int)field("ihold");
            bool vsense = field("vsense");
            double cur = current_from_bits(sense_resistor_, irun, vsense);
            double hold = current_from_bits(sense_resistor_, ihold, vsense);
            if (g.has("CURRENT") || g.has("HOLDCURRENT")) {
                double run = cur, hreq = req_hold_current_;
                if (g.has("CURRENT")
                    && !g.get_float("CURRENT", &run, err, nullptr, 0.,
                                    MAX_CURRENT))
                    return false;
                if (g.has("HOLDCURRENT")
                    && !g.get_float("HOLDCURRENT", &hreq, err, nullptr, NAN,
                                    MAX_CURRENT, 0.))
                    return false;
                get_last_move_time();
                req_hold_current_ = hreq;
                bool old_vsense = field("vsense");
                set_current(run, hreq);
                if (field("vsense") != old_vsense) {
                    uint32_t v = set_field("vsense", field("vsense"));
                    if (!set_register("CHOPCONF", v, err))
                        return false;
                }
                if (!set_register("IHOLD_IRUN", set_field("irun", field("irun")),
                                  err))
                    return false;
                vsense = field("vsense");
                cur = current_from_bits(sense_resistor_, (int)field("irun"),
                                        vsense);
                hold = current_from_bits(sense_resistor_, (int)field("ihold"),
                                         vsense);
            }
            char buf[96];
            snprintf(buf, sizeof(buf), "Run Current: %0.2fA Hold Current: %0.2fA",
                     cur, hold);
            gcode.respond_info(buf);
            return true;
        },
        "Set the current of a TMC driver");
    gcode.register_mux_command(
        "SET_TMC_FIELD", "STEPPER", name_,
        [this, get_last_move_time](GCodeCommand &g, std::string *err) {
            std::string f;
            if (!g.get("FIELD", &f, err))
                return false;
            for (auto &c : f)
                c = (char)tolower((unsigned char)c);
            const char *reg;
            if (!field_mask(f, &reg)) {
                *err = "Unknown field name '" + f + "'";
                return false;
            }
            int64_t value;
            if (g.has("VELOCITY")) {
                double v;
                if (!g.get_float("VELOCITY", &v, err, nullptr, 0.))
                    return false;
                value = tstep_threshold(step_dist_, field("mres"), v);
            } else if (!g.get_int("VALUE", &value, err)) {
                return false;
            }
            uint32_t rv = set_field(f, (uint32_t)value);
            get_last_move_time();
            return set_register(reg, rv, err);
        },
        "Set a register field of a TMC driver");
    gcode.register_mux_command(
        "INIT_TMC", "STEPPER", name_,
        [this, get_last_move_time](GCodeCommand &, std::string *err) {
            get_last_move_time();
            return init_registers(err);
        },
        "Initialize TMC stepper driver registers");
    gcode.register_mux_command(
        "DUMP_TMC", "STEPPER", name_,
        [this, &gcode](GCodeCommand &, std::string *err) {
            std::string out = "========== Write-only registers ==========";
            char buf[64];
            for (const auto &r : regs_) {
                bool readable = false;
                for (const char *rr : READ_REGISTERS)
                    readable = readable || r.first == rr;
                if (readable)
                    continue;
                snprintf(buf, sizeof(buf), "\n%-11s %08x", (r.first + ":").c_str(),
                         (unsigned)r.second);
                out += buf;
            }
            out += "\n========== Queried registers ==========";
            for (const char *rr : READ_REGISTERS) {
                uint32_t v;
                if (!get_register(rr, &v, err))
                    return false;
                snprintf(buf, sizeof(buf), "\n%-11s %08x",
                         (std::string(rr) + ":").c_str(), (unsigned)v);
                out += buf;
            }
            gcode.respond_info(out);
            return true;
        },
        "Read and display TMC stepper driver registers");
}

} // namespace kl
