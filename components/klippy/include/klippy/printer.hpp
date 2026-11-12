// The printer object: builds the host side of Klipper from printer.cfg
// and runs G-code on it.  Ports (subsets of) klippy.py, pins.py,
// stepper.py, stepper_enable.py, kinematics/corexy.py, gcode_move.py,
// extras/pwm_tool.py, extras/output_pin.py, extras/input_shaper.py and
// extras/force_move.py.
//
// Threading: everything runs on one task (the "klippy" task).  Call
// service() regularly while idle; long operations (moves, M400) call it
// themselves while they wait for the MCU.
//
// This file may be distributed under the terms of the GNU GPLv3 license.
#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "chelper_api.h"
#include "klippy/configfile.hpp"
#include "klippy/files.hpp"
#include "klippy/gcode.hpp"
#include "klippy/jinja.hpp"
#include "klippy/mcu_endstop.hpp"
#include "klippy/tmc2209.hpp"
#include "klippy_lite/motion.hpp"
#include "mcu_link/mcu.hpp"

namespace kl {

// pins.py: "[^|~][!][chip:]pin"
struct PinParams {
    std::string chip = "mcu";
    std::string pin;
    bool invert = false;
    int pullup = 0; // 1 = pull-up, -1 = pull-down
};
bool parse_pin(const std::string &desc, bool can_invert, bool can_pullup,
               PinParams *out, std::string *err);

// Buffering for this port (see m1_bench / README for the measurements)
struct PrinterOptions {
    double buffer_time_high = 0.4;
    MotionQueuingConfig queuing = [] {
        MotionQueuingConfig c;
        c.history_expire = 0.25; // homing reads step history at trigger time
        c.bgflush_sg_low = 0.10;
        c.bgflush_sg_high = 0.20;
        return c;
    }();
    size_t lookahead_max_moves = 32;
    // Keep printer.cfg (sections and raw options) after loading for the
    // "configfile" status object (Moonraker/Mainsail).  Off on the CYD to
    // save RAM.
    bool keep_config = false;
};

class Printer;

// stepper.py MCU_stepper + the [stepper_*] rail options
struct PrinterStepper {
    std::string name;            // "stepper_x"
    int oid = -1;
    PinParams step_pin, dir_pin;
    double rotation_distance = 0., step_dist = 0.;
    double step_pulse_duration = -1.; // < 0: not set
    bool req_step_both_edge = false, step_both_edge = false;
    syncemitter *se = nullptr;
    stepcompress *sc = nullptr;
    stepper_kinematics *kin_sk = nullptr;   // kinematics (corexy/cartesian)
    stepper_kinematics *shaper_sk = nullptr; // input shaper wrapper
    double mcu_position_offset = 0.;
    // stepper_enable.py
    bool has_enable = false;
    PinParams enable_pin;
    int enable_oid = -1;
    bool enabled = false;
    uint64_t enable_last_clock = 0;
    // rail
    double position_min = 0., position_max = 0., position_endstop = 0.;
    double homing_speed = 5., second_homing_speed = 2.5;
    double homing_retract_speed = 5., homing_retract_dist = 5.;
    bool homing_positive_dir = false;
    std::string endstop_pin;
    McuEndstop *endstop = nullptr;  // rail endstop (homing)
    Tmc2209 *virtual_tmc = nullptr; // set for tmc virtual_endstop rails
    int32_t reset_cmd_tag = -1;
    std::string short_name() const { return name.substr(8); } // "x"

    stepper_kinematics *active_sk() const {
        return shaper_sk ? shaper_sk : kin_sk;
    }
    int64_t get_mcu_position() const;
};

// print_stats.py status
struct PrintStatsInfo {
    std::string filename, state = "standby", message;
    double total_duration = 0., print_duration = 0.;
};

// virtual_sdcard.py status
struct SdcardInfo {
    bool is_active = false;
    std::string file_path;
    size_t file_position = 0, file_size = 0;
    double progress = 0.;
};

// Status for a local UI or an API task: a copy made by the klippy task a
// few times per second (Printer::snapshot() may be called from any task)
struct PrinterSnapshot {
    uint32_t seq = 0;          // increases on every update
    bool ready = false;        // klippy state "ready"
    std::string state_message; // startup / shutdown reason
    PrintStatsInfo print;      // print_stats
    double progress = 0.;      // virtual_sdcard
    bool sd_active = false;
    double live_pos[3] = {0., 0., 0.}; // motion_report live_position
    double live_velocity = 0.;
    double gcode_pos[3] = {0., 0., 0.}; // gcode_move gcode_position
    std::string homed_axes;
    double axis_min[3] = {0., 0., 0.}, axis_max[3] = {0., 0., 0.};
    std::string pen_pin;  // first [pwm_tool]/[output_pin] (pen solenoid)
    double pen_value = 0.;
    double speed_factor = 100.; // M220 (percent)
    bool busy = false;    // a submitted command is running
    size_t queued = 0;    // submitted commands waiting
    std::string last_error; // last "!! " response
    uint32_t error_seq = 0; // increases with every "!! " response
};

// gcode_move.py state
// Host CPU accounting (diagnostics)
struct PrinterStats {
    double wait_s = 0.;       // sleeping while waiting for the MCU
    double render_s = 0.;     // rendering G-code macro templates
    uint64_t renders = 0;
    double flush_s = 0.;      // background step flushing (step generation)
    double move_s = 0.;       // toolhead.move (lookahead, trapq)
    double tick_s = 0.;       // clock sync etc.
};

struct GCodeMoveState {
    bool absolute_coord = true, absolute_extrude = true;
    double base_position[4] = {0., 0., 0., 0.};
    double last_position[4] = {0., 0., 0., 0.};
    double homing_position[4] = {0., 0., 0., 0.};
    double speed = 25.;
    double speed_factor = 1. / 60.;
    double extrude_factor = 1.;
};

class Printer {
public:
    struct Sdcard;      // virtual_sdcard state (printer_internal.hpp)
    struct PrintStats;  // print_stats state
    explicit Printer(const PrinterOptions &opt = PrinterOptions());
    ~Printer();
    Printer(const Printer &) = delete;
    Printer &operator=(const Printer &) = delete;

    // Config phase: parse printer.cfg and create all objects.  Unsupported
    // sections/options are collected in warnings().
    bool load_config(const std::string &text, std::string *err,
                     const std::string &filename = "printer.cfg");
    const std::vector<std::string> &warnings() const { return warnings_; }

    // Connect to the MCU (port from the caller; [mcu] serial is a Linux
    // path) and send the configuration.  Ends in the "ready" state.
    bool connect(const std::string &port, int baud, std::string *err);
    // [mcu] serial / baud from printer.cfg (ESP32: "uart:<n>:<tx>:<rx>")
    const std::string &mcu_serial() const { return mcu_serial_; }
    int mcu_baud() const { return mcu_baud_; }
    // FIRMWARE_RESTART, MCU side: ask the MCU to reset itself (klippy
    // task).  The host must then restart and connect again.
    bool reset_mcu();
    void disconnect();
    bool is_ready() const { return ready_ && !mcu_.is_shutdown(); }
    std::string state_message() const;

    // G-code
    GCodeDispatch &gcode() { return gcode_; }
    bool run_script(const std::string &script, std::string *err) {
        return gcode_.run_script(script, err);
    }

    // Host loop: clock sync, background step flushing, lookahead priming
    void service();
    // toolhead.wait_moves() (M400)
    void wait_moves();

    // Main loop of the klippy task: runs submitted commands, the SD card
    // print (virtual_sdcard work timer) and background work.  Call it
    // continuously; one call takes at most ~50 ms plus one G-code line.
    void poll();
    // Queue a G-code script from another task (UI, API).  Runs in poll()
    // between file lines (gcode_mutex in klippy); done() is called there.
    void submit_script(std::string script,
                       std::function<void(bool ok, const std::string &err)>
                           done = nullptr);
    // Storage for virtual_sdcard ([virtual_sdcard] path is ignored)
    void set_filesystem(std::shared_ptr<FileSystem> fs);
    PrintStatsInfo print_stats();
    SdcardInfo sdcard_status();
    std::shared_ptr<FileSystem> filesystem();
    // Latest status copy (any task)
    PrinterSnapshot snapshot();

    // API (Moonraker) support.  get_status() / object_names() must run on
    // the klippy task: use run_on_klippy() from other tasks.
    JValue get_status(const std::string &name) { return status_object(name); }
    std::vector<std::string> object_names();
    // "ready", "startup", "shutdown" or "error" (webhooks state)
    std::string klippy_state() const;
    // Run fn on the klippy task (between G-code lines, or while it waits
    // for the MCU) and wait for it.  false = klippy did not start it within
    // timeout, and it never will (so fn may capture the caller's locals).
    bool run_on_klippy(std::function<void()> fn, double timeout = 5.);
    // Every G-code response (called on the klippy task)
    void add_response_listener(std::function<void(const std::string &)> fn) {
        response_listeners_.push_back(std::move(fn));
    }

    // Accessors (tests, status)
    Mcu &mcu() { return mcu_; }
    ToolHead &toolhead() { return *toolhead_; }
    MotionQueuing &motion_queuing() { return *mq_; }
    std::vector<std::unique_ptr<PrinterStepper>> &steppers() { return steppers_; }
    std::vector<std::unique_ptr<Tmc2209>> &tmcs() { return tmcs_; }
    PrinterStepper *lookup_stepper(const std::string &name);
    bool query_mcu_position(PrinterStepper &s, int64_t *pos, std::string *err);
    const GCodeMoveState &gcode_move() const { return gm_; }
    PrinterStats &stats() { return stats_; }
    double monotonic() const;

private:
    struct OutputPin;
    struct Macro;
    struct HomingOverride;

    struct ShaperParams {
        std::string type;
        double freq = 0., damping = 0.1;
        int n = 0;
        double A[5], T[5];
    };

    bool load_stepper(ConfigSection sec, char kin_axis, std::string *err);
    bool load_output_pin(ConfigSection sec, bool pwm_tool, std::string *err);
    bool load_input_shaper(ConfigSection sec, std::string *err);
    bool calc_shaper(ShaperParams &p, std::string *err);
    void apply_input_shaper();
    void build_stepper_config(PrinterStepper &s);
    void register_commands();
    void setup_toolhead_hooks();
    bool check_move(Move &m, std::string *err);
    void motor_enable(double print_time);
    void motor_off(std::string *err);
    void send_enable(PrinterStepper &s, double print_time, bool on);
    void throttle();
    // gcode_macro.py / homing_override.py
    bool load_macros(std::set<std::string> *handled, std::string *err);
    JDict template_context();
    JValue status_object(const std::string &name);
    bool run_template(const JTemplate &t, const std::string &name,
                      const std::vector<const JDict *> &layers,
                      std::string *err);
    std::unique_ptr<JDict> base_context_; // printer + action_* (built once)
    // reactor.pause(): sleep while keeping clock sync alive (no flushing)
    void pause(double seconds);
    bool setup_endstops(std::string *err);
    bool sync_mcu_position(PrinterStepper &s, std::string *err);
    void note_homing_end(PrinterStepper &s);
    // kinematics calc_position() from stepper commanded positions
    void calc_position(const double spos[3], double out[3]) const;
    bool home_axis(int axis, std::string *err);
    bool homing_move(PrinterStepper &rail, const double movepos[3],
                     double speed, bool check_triggered, bool *no_movement,
                     std::string *err);
    void invoke_shutdown(const std::string &msg);
    void set_position(const double pos[3], const char *homing_axes);

    // G-code handlers (gcode_move.py, toolhead.py, stepper_enable.py ...)
    bool cmd_G1(GCodeCommand &g, std::string *err);
    bool cmd_G28(GCodeCommand &g, std::string *err);
    bool cmd_QUERY_ENDSTOPS(GCodeCommand &g, std::string *err);
    bool cmd_G4(GCodeCommand &g, std::string *err);
    bool cmd_G92(GCodeCommand &g, std::string *err);
    bool cmd_M114(GCodeCommand &g, std::string *err);
    bool cmd_M220(GCodeCommand &g, std::string *err);
    bool cmd_SET_VELOCITY_LIMIT(GCodeCommand &g, std::string *err);
    bool cmd_SET_KINEMATIC_POSITION(GCodeCommand &g, std::string *err);
    bool cmd_GET_POSITION(GCodeCommand &g, std::string *err);

    PrinterOptions opt_;
    ConfigFile config_;
    std::vector<std::string> warnings_;
    Mcu mcu_;
    std::unique_ptr<MotionQueuing> mq_;
    std::unique_ptr<ToolHead> toolhead_;
    GCodeDispatch gcode_;
    GCodeMoveState gm_;
    PrinterStats stats_;
    void sleep_s(double s); // sleep, counted in stats_.wait_s
    std::vector<std::unique_ptr<PrinterStepper>> steppers_;
    std::vector<std::unique_ptr<OutputPin>> output_pins_;
    std::vector<std::unique_ptr<Tmc2209>> tmcs_;
    std::vector<std::unique_ptr<McuEndstop>> endstops_;
    std::vector<std::unique_ptr<Macro>> macros_;
    std::unique_ptr<HomingOverride> homing_override_;
    std::unique_ptr<Sdcard> sd_;
    std::unique_ptr<PrintStats> pstats_;
    bool has_pause_resume_ = false;
    bool is_paused_ = false, sd_paused_ = false, pause_command_sent_ = false;
    double recover_velocity_ = 50.;
    std::map<std::string, GCodeMoveState> saved_states_;
    struct Pending {
        std::string script;
        std::function<void(bool, const std::string &)> done;
    };
    std::mutex pending_lock_;
    std::deque<Pending> pending_;
    bool run_pending();
    std::atomic<bool> running_cmd_{false};
    // run_on_klippy() calls
    struct KlippyCall;
    std::mutex calls_lock_;
    std::deque<std::shared_ptr<KlippyCall>> calls_;
    const void *klippy_thread_ = nullptr;
    void run_calls();
    std::vector<std::function<void(const std::string &)>> response_listeners_;
    // printer.cfg as loaded (opt_.keep_config)
    std::vector<std::pair<std::string,
                          std::vector<std::pair<std::string, std::string>>>>
        config_copy_;
    bool status_more(const std::string &name, JValue *out);
    double print_time_start_ = -1.;
    std::string mcu_serial_;
    int mcu_baud_ = 250000;
    // Status copy for other tasks
    std::mutex snapshot_lock_;
    PrinterSnapshot snapshot_;
    double next_snapshot_ = 0.;
    std::string last_error_;
    uint32_t error_seq_ = 0;
    void publish_snapshot(double now);
    // virtual_sdcard.py / print_stats.py / pause_resume.py
    bool load_sdcard(std::set<std::string> *handled, std::string *err);
    void sd_work_slice();
    void sd_do_pause();
    bool sd_do_resume(std::string *err);
    void sd_do_cancel();
    bool sd_load_file(const std::string &name, std::string *err);
    std::vector<TmcUartBus *> tmc_buses_;
    std::vector<std::unique_ptr<TmcUartBus>> tmc_bus_owned_;
    bool tmc_enable_pending_ = false;
    bool in_flush_ = false; // re-entrancy guard for step flushing
    // TMC error checks run in a background thread (klippy: reactor timer)
    std::thread tmc_thread_;
    std::atomic<bool> tmc_thread_stop_{false};
    std::atomic<bool> tmc_error_{false};
    std::mutex tmc_error_lock_;
    std::string tmc_error_msg_;
    void tmc_check_thread();
    std::string shutdown_msg_;
    ShaperParams shaper_[2];
    bool have_shaper_ = false;
    // [printer]
    std::string kinematics_;
    double max_z_velocity_ = 0., max_z_accel_ = 0.;
    // corexy.py limits: (min, max) per axis; min > max = not homed
    double limits_[3][2] = {{1., -1.}, {1., -1.}, {1., -1.}};
    double axes_min_[3] = {0., 0., 0.}, axes_max_[3] = {0., 0., 0.};
    int reserved_move_slots_ = 0;
    bool ready_ = false;
    double last_move_submit_ = 0.;
};

} // namespace kl
