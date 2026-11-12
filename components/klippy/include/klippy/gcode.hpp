// Port of klippy/gcode.py: G-code line parsing and command dispatch.
//
// "G1 X10 Y20" style commands (letter + number parameters) and extended
// commands ("SET_PIN PIN=LASER VALUE=1") are parsed like Klipper does.
// Handlers report errors by returning false with a message (Klipper's
// gcmd.error), which aborts the rest of a script.
//
// This file may be distributed under the terms of the GNU GPLv3 license.
#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace kl {

class GCodeCommand {
public:
    using ParamList = std::vector<std::pair<std::string, std::string>>;
    GCodeCommand(std::string command, std::string commandline,
                 ParamList params)
        : command_(std::move(command)), commandline_(std::move(commandline)),
          ordered_(std::move(params)) {}

    const std::string &get_command() const { return command_; }
    const std::string &get_commandline() const { return commandline_; }
    // Parameters in the order they appear (Python dict order).  There are
    // only a few per command, so lookups are linear.
    const ParamList &ordered_params() const { return ordered_; }
    const std::string *find(const std::string &name) const {
        for (const auto &kv : ordered_)
            if (kv.first == name)
                return &kv.second;
        return nullptr;
    }
    bool has(const std::string &name) const { return find(name) != nullptr; }
    // Text after the command name (M117 / RESPOND style raw arguments)
    std::string get_raw_command_parameters() const;

    // Getters return false (and set *err) on a missing/invalid value, like
    // gcmd.get_float() raising gcmd.error
    bool get(const std::string &name, std::string *out, std::string *err,
             const char *def = nullptr) const;
    bool get_float(const std::string &name, double *out, std::string *err,
                   const double *def = nullptr, double minval = NAN_,
                   double maxval = NAN_, double above = NAN_,
                   double below = NAN_) const;
    bool get_int(const std::string &name, int64_t *out, std::string *err,
                 const int64_t *def = nullptr) const;

    static constexpr double NAN_ = __builtin_nan("");

private:
    std::string command_, commandline_;
    ParamList ordered_;
};

class GCodeDispatch {
public:
    using Handler = std::function<bool(GCodeCommand &, std::string *err)>;
    using Responder = std::function<void(const std::string &)>;

    void register_command(const std::string &cmd, Handler h,
                          const std::string &desc = "");
    // SET_PIN PIN=<value> style commands shared by several objects
    void register_mux_command(const std::string &cmd, const std::string &key,
                              const std::string &value, Handler h,
                              const std::string &desc = "");
    bool has_command(const std::string &cmd) const;
    // All commands with their help text (gcode.py get_command_help())
    std::vector<std::pair<std::string, std::string>> command_help() const;
    Handler get_handler(const std::string &cmd) const;
    // gcode_macro rename_existing: move a handler to a new name
    void rename_command(const std::string &cmd, const std::string &new_name);

    // Run one line.  Returns false on error (message in *err).
    bool run_line(const std::string &line, std::string *err);
    // Run several lines (stops at the first error)
    bool run_script(const std::string &script, std::string *err);

    void set_responder(Responder r) { responder_ = std::move(r); }
    // Also sees every response (status for the UI); not replaced by
    // set_responder()
    void set_monitor(Responder r) { monitor_ = std::move(r); }
    void respond_info(const std::string &msg) const;
    void respond_raw(const std::string &msg) const;

    // Parse a line into a command (empty command = nothing to run)
    static bool parse_line(const std::string &line, std::string *cmd,
                           GCodeCommand::ParamList *params,
                           std::string *commandline);
    bool in_script_depth() const { return depth_ > 0; }
    // Diagnostics: time spent parsing lines (seconds) and lines parsed
    double parse_s = 0.;
    uint64_t parsed = 0;
    std::function<double()> clock;

private:
    std::map<std::string, Handler> handlers_;
    std::map<std::string, std::string> help_;
    struct Mux {
        std::string key;
        std::map<std::string, Handler> values;
    };
    std::map<std::string, Mux> mux_;
    Responder responder_, monitor_;
    int depth_ = 0;
};

} // namespace kl
