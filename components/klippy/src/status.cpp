// Status objects for the API (klippy get_status() of the objects Mainsail
// reads), and calls into the klippy task from other tasks.
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "klippy/printer.hpp"
#include "printer_internal.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdlib>

namespace kl {

namespace {

const void *
this_thread_tag()
{
    static thread_local char tag;
    return &tag;
}

JValue
num(double v)
{
    return JValue::number(v);
}

JValue
list4(const double *p, double e)
{
    return JValue::make_list({num(p[0]), num(p[1]), num(p[2]), num(e)});
}

JValue
list3(const double *p)
{
    return JValue::make_list({num(p[0]), num(p[1]), num(p[2])});
}

// configfile "settings": options parsed like Klipper does for the types it
// knows (numbers and booleans); everything else stays a string
JValue
setting_value(const std::string &raw)
{
    if (raw.empty())
        return JValue::str(raw);
    char *end = nullptr;
    double d = strtod(raw.c_str(), &end);
    if (end && *end == '\0') {
        bool integral = raw.find_first_of(".eE") == std::string::npos;
        return integral ? JValue::integer((int64_t)d) : JValue::number(d);
    }
    if (raw == "True" || raw == "true")
        return JValue::boolean(true);
    if (raw == "False" || raw == "false")
        return JValue::boolean(false);
    return JValue::str(raw);
}

} // namespace

/****************************************************************
 * Calls from other tasks
 ****************************************************************/

struct Printer::KlippyCall {
    std::function<void()> fn;
    std::mutex lock;
    std::condition_variable cv;
    bool started = false;   // klippy took it (it will run to the end)
    bool cancelled = false; // the caller gave up before klippy took it
    bool done = false;
};

bool
Printer::run_on_klippy(std::function<void()> fn, double timeout)
{
    if (this_thread_tag() == klippy_thread_ || !klippy_thread_) {
        fn();
        return true;
    }
    auto c = std::make_shared<KlippyCall>();
    c->fn = std::move(fn);
    {
        std::lock_guard<std::mutex> g(calls_lock_);
        calls_.push_back(c);
    }
    std::unique_lock<std::mutex> g(c->lock);
    if (c->cv.wait_for(g, std::chrono::duration<double>(timeout),
                       [&] { return c->done; }))
        return true;
    // Timed out.  fn usually refers to the caller's locals, so it must not
    // run after we return: drop it if klippy has not taken it yet, else
    // wait for it to finish.
    if (!c->started) {
        c->cancelled = true;
        return false;
    }
    c->cv.wait(g, [&] { return c->done; });
    return true;
}

void
Printer::run_calls()
{
    klippy_thread_ = this_thread_tag();
    for (;;) {
        std::shared_ptr<KlippyCall> c;
        {
            std::lock_guard<std::mutex> g(calls_lock_);
            if (calls_.empty())
                return;
            c = calls_.front();
            calls_.pop_front();
        }
        {
            std::lock_guard<std::mutex> g(c->lock);
            if (c->cancelled)
                continue;
            c->started = true;
        }
        c->fn();
        {
            std::lock_guard<std::mutex> g(c->lock);
            c->done = true;
        }
        c->cv.notify_all();
    }
}

/****************************************************************
 * Status objects
 ****************************************************************/

std::string
Printer::klippy_state() const
{
    if (!shutdown_msg_.empty() || mcu_.is_shutdown())
        return "shutdown";
    return ready_ ? "ready" : "startup";
}

std::vector<std::string>
Printer::object_names()
{
    std::vector<std::string> n = {
        "webhooks",      "configfile",   "gcode",        "toolhead",
        "gcode_move",    "motion_report", "idle_timeout", "system_stats",
        "stepper_enable"};
    if (pstats_)
        n.push_back("print_stats");
    if (sd_)
        n.push_back("virtual_sdcard");
    if (has_pause_resume_)
        n.push_back("pause_resume");
    n.push_back("display_status");
    for (auto &op : output_pins_)
        n.push_back((op->pwm_tool ? "pwm_tool " : "output_pin ") + op->name);
    for (auto &t : tmcs_)
        n.push_back("tmc2209 " + t->stepper_name());
    for (auto &m : macros_)
        n.push_back("gcode_macro " + m->name);
    return n;
}

bool
Printer::status_more(const std::string &name, JValue *out)
{
    JValue d = JValue::make_dict();
    JDict &o = *d.dict;
    if (name == "webhooks") {
        std::string st = klippy_state();
        o.set("state", JValue::str(st));
        std::string msg = st == "ready" ? "Printer is ready" : state_message();
        o.set("state_message", JValue::str(msg));
    } else if (name == "configfile") {
        JValue cfg = JValue::make_dict(), set = JValue::make_dict();
        for (const auto &sec : config_copy_) {
            JValue c = JValue::make_dict(), st = JValue::make_dict();
            for (const auto &kv : sec.second) {
                c.dict->set(kv.first, JValue::str(kv.second));
                st.dict->set(kv.first, setting_value(kv.second));
            }
            cfg.dict->set(sec.first, c);
            // Klipper lowercases section names in settings
            std::string lname = sec.first;
            for (auto &ch : lname)
                ch = (char)tolower((unsigned char)ch);
            set.dict->set(lname, st);
        }
        o.set("config", cfg);
        o.set("settings", set);
        o.set("save_config_pending", JValue::boolean(false));
        o.set("save_config_pending_items", JValue::make_dict());
        JList w;
        for (const auto &s : warnings_) {
            JValue e = JValue::make_dict();
            e.dict->set("type", JValue::str("deprecated_option"));
            e.dict->set("message", JValue::str(s));
            w.push_back(e);
        }
        o.set("warnings", JValue::make_list(w));
    } else if (name == "gcode") {
        JValue cmds = JValue::make_dict();
        for (const auto &c : gcode_.command_help()) {
            JValue h = JValue::make_dict();
            if (!c.second.empty())
                h.dict->set("help", JValue::str(c.second));
            cmds.dict->set(c.first, h);
        }
        o.set("commands", cmds);
    } else if (name == "toolhead" && toolhead_) {
        const double *c = toolhead_->commanded_pos();
        o.set("position", list4(c, 0.));
        std::string homed;
        for (int i = 0; i < 3; i++)
            if (limits_[i][0] <= limits_[i][1])
                homed += (char)('x' + i);
        o.set("homed_axes", JValue::str(homed));
        o.set("axis_minimum", list4(axes_min_, 0.));
        o.set("axis_maximum", list4(axes_max_, 0.));
        o.set("max_velocity", num(toolhead_->max_velocity));
        o.set("max_accel", num(toolhead_->max_accel));
        o.set("minimum_cruise_ratio", num(toolhead_->min_cruise_ratio));
        o.set("square_corner_velocity",
              num(toolhead_->square_corner_velocity));
        o.set("print_time", num(toolhead_->print_time()));
        o.set("estimated_print_time",
              num(ready_ ? mcu_.estimated_print_time(monotonic()) : 0.));
        o.set("stalls", JValue::integer((int64_t)toolhead_->runouts()));
        o.set("extruder", JValue::str(""));
    } else if (name == "gcode_move") {
        double gp[3], ho[3];
        for (int i = 0; i < 3; i++) {
            gp[i] = gm_.last_position[i] - gm_.base_position[i];
            ho[i] = gm_.base_position[i] - gm_.homing_position[i];
        }
        o.set("speed_factor", num(gm_.speed_factor * 60.));
        o.set("speed", num(gm_.speed / gm_.speed_factor));
        o.set("extrude_factor", num(gm_.extrude_factor));
        o.set("absolute_coordinates", JValue::boolean(gm_.absolute_coord));
        o.set("absolute_extrude", JValue::boolean(gm_.absolute_extrude));
        o.set("homing_origin", list4(ho, 0.));
        o.set("position", list4(gm_.last_position, gm_.last_position[3]));
        o.set("gcode_position", list4(gp, 0.));
    } else if (name == "motion_report") {
        PrinterSnapshot s = snapshot();
        o.set("live_position", list4(s.live_pos, 0.));
        o.set("live_velocity", num(s.live_velocity));
        o.set("live_extruder_velocity", num(0.));
        JList st;
        for (auto &p : steppers_)
            st.push_back(JValue::str(p->name));
        o.set("steppers", JValue::make_list(st));
        o.set("trapq", JValue::make_list({JValue::str("toolhead")}));
    } else if (name == "idle_timeout") {
        bool printing = running_cmd_ || (sd_ && sd_->work_active);
        o.set("state", JValue::str(printing ? "Printing" : "Ready"));
        o.set("printing_time", num(0.));
    } else if (name == "system_stats") {
        o.set("sysload", num(0.));
        o.set("cputime", num(monotonic()));
        o.set("memavail", JValue::integer(0));
    } else if (name == "stepper_enable") {
        JValue st = JValue::make_dict();
        for (auto &p : steppers_)
            st.dict->set(p->name, JValue::boolean(p->enabled));
        o.set("steppers", st);
    } else if (name == "print_stats" && pstats_) {
        PrintStatsInfo ps = print_stats();
        o.set("filename", JValue::str(ps.filename));
        o.set("total_duration", num(ps.total_duration));
        o.set("print_duration", num(ps.print_duration));
        o.set("filament_used", num(0.));
        o.set("state", JValue::str(ps.state));
        o.set("message", JValue::str(ps.message));
        JValue info = JValue::make_dict();
        info.dict->set("total_layer", JValue::none());
        info.dict->set("current_layer", JValue::none());
        o.set("info", info);
    } else if (name == "virtual_sdcard" && sd_) {
        SdcardInfo s = sdcard_status();
        o.set("file_path", s.file_path.empty() ? JValue::none()
                                               : JValue::str(s.file_path));
        o.set("progress", num(s.progress));
        o.set("is_active", JValue::boolean(s.is_active));
        o.set("file_position", JValue::integer((int64_t)s.file_position));
        o.set("file_size", JValue::integer((int64_t)s.file_size));
    } else if (name == "pause_resume" && has_pause_resume_) {
        o.set("is_paused", JValue::boolean(is_paused_));
    } else if (name == "display_status") {
        o.set("progress", num(sd_ ? sdcard_status().progress : 0.));
        o.set("message", JValue::none());
    } else if (name.compare(0, 8, "tmc2209 ") == 0) {
        Tmc2209 *t = nullptr;
        for (auto &x : tmcs_)
            if ("tmc2209 " + x->stepper_name() == name)
                t = x.get();
        if (!t)
            return false;
        o.set("mcu_phase_offset", JValue::none());
        o.set("phase_offset_position", JValue::none());
        o.set("run_current", num(t->run_current()));
        o.set("hold_current", num(t->hold_current()));
        o.set("temperature", JValue::none());
        o.set("drv_status", JValue::none());
    } else {
        return false;
    }
    *out = d;
    return true;
}

} // namespace kl
