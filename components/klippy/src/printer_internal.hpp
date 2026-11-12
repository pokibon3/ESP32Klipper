// Definitions of Printer's private module structs shared between the
// printer source files.
//
// This file may be distributed under the terms of the GNU GPLv3 license.
#pragma once

#include "klippy/printer.hpp"

namespace kl {

struct Printer::OutputPin {
    std::string name;
    PinParams pin;
    int oid = -1;
    syncemitter *se = nullptr;
    double cycle_time = 0.100, max_duration = 0., scale = 1.;
    double start_value = 0., shutdown_value = 0.; // raw (after invert)
    double last_value = 0.;                       // user value
    double last_print_time = 0.;
    uint64_t last_clock = 0;
    int64_t last_pwm = 0, default_value = 0;
    double pwm_max = 0.;
    int32_t set_cmd_tag = -1;
    bool pwm_tool = false; // [pwm_tool] (else [output_pin])
};

struct Printer::Macro {
    std::string name;   // as written in the section name
    std::string alias;  // command name (upper case)
    JTemplate tmpl;
    JDict variables;
    std::string rename_existing;
    bool in_script = false;
};

struct Printer::HomingOverride {
    double start_pos[3];
    bool has_start[3] = {false, false, false};
    std::string axes = "XYZ";
    JTemplate tmpl;
    GCodeDispatch::Handler prev_G28;
    bool in_script = false;
};

struct Printer::Sdcard {
    std::shared_ptr<FileSystem> fs;
    std::unique_ptr<GcodeFile> file;
    std::string file_path;
    size_t file_position = 0, file_size = 0, next_file_position = 0;
    bool work_active = false, work_started = false;
    bool must_pause_work = false, cmd_from_sd = false;
    // File data not yet run: rbuf[rpos:] (read in small chunks; lines are
    // taken out one at a time to keep the memory use low on the ESP32)
    std::string rbuf;
    size_t rpos = 0;
    std::string line; // the line being run (buffer reused between lines)
    void clear_input() {
        rbuf.clear();
        rbuf.shrink_to_fit();
        rpos = 0;
    }
};

struct Printer::PrintStats {
    PrintStatsInfo info;
    double print_start_time = -1., last_pause_time = -1.;
    double prev_pause_duration = 0., init_duration = 0.;
};

} // namespace kl
