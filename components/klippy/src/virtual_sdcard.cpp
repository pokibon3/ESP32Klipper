// Printing from storage: port of klippy/extras/virtual_sdcard.py,
// print_stats.py and pause_resume.py, plus gcode_move.py
// SAVE_GCODE_STATE / RESTORE_GCODE_STATE and the klippy main loop.
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "klippy/printer.hpp"
#include "printer_internal.hpp"

namespace kl {

static std::string
lower(std::string s)
{
    for (auto &c : s)
        c = (char)tolower((unsigned char)c);
    return s;
}

/****************************************************************
 * print_stats.py
 ****************************************************************/

PrintStatsInfo
Printer::print_stats()
{
    if (!pstats_)
        return PrintStatsInfo();
    // print_stats.get_status()
    PrintStats &ps = *pstats_;
    double now = monotonic();
    double time_paused = ps.prev_pause_duration;
    if (ps.print_start_time >= 0.) {
        if (ps.last_pause_time >= 0.)
            time_paused += now - ps.last_pause_time;
        ps.info.total_duration = now - ps.print_start_time;
    }
    // Klipper starts print_duration at the first extrusion; a pen plotter
    // extrudes nothing, so count the whole time except pauses
    ps.info.print_duration = ps.info.total_duration - time_paused;
    return ps.info;
}

static void
note_start(Printer::PrintStats &ps, double now)
{
    if (ps.print_start_time < 0.) {
        ps.print_start_time = now;
    } else if (ps.last_pause_time >= 0.) {
        ps.prev_pause_duration += now - ps.last_pause_time;
        ps.last_pause_time = -1.;
    }
    ps.info.state = "printing";
    ps.info.message.clear();
}

static void
note_pause(Printer::PrintStats &ps, double now)
{
    if (ps.last_pause_time < 0.)
        ps.last_pause_time = now;
    if (ps.info.state != "error")
        ps.info.state = "paused";
}

static void
note_finish(Printer::PrintStats &ps, double now, const char *state,
            const std::string &msg = "")
{
    if (ps.print_start_time < 0.)
        return;
    ps.info.state = state;
    ps.info.message = msg;
    ps.info.total_duration = now - ps.print_start_time;
    ps.print_start_time = -1.;
}

static void
reset_stats(Printer::PrintStats &ps, const std::string &filename = "")
{
    ps = Printer::PrintStats();
    ps.info.filename = filename;
}

/****************************************************************
 * virtual_sdcard.py
 ****************************************************************/

SdcardInfo
Printer::sdcard_status()
{
    SdcardInfo s;
    if (!sd_)
        return s;
    s.is_active = sd_->work_active;
    s.file_path = sd_->file_path;
    s.file_position = sd_->file_position;
    s.file_size = sd_->file_size;
    if (sd_->file_size)
        s.progress = (double)sd_->file_position / sd_->file_size;
    return s;
}

std::shared_ptr<FileSystem>
Printer::filesystem()
{
    return sd_ ? sd_->fs : nullptr;
}

void
Printer::set_filesystem(std::shared_ptr<FileSystem> fs)
{
    if (sd_)
        sd_->fs = std::move(fs);
}

bool
Printer::sd_load_file(const std::string &filename, std::string *err)
{
    // virtual_sdcard._load_file(): case-insensitive name match
    std::string name = filename;
    if (!name.empty() && name[0] == '/')
        name = name.substr(1);
    if (!sd_->fs) {
        *err = "No storage available";
        return false;
    }
    std::string found;
    for (const auto &f : sd_->fs->list())
        if (lower(f.first) == lower(name))
            found = f.first;
    std::unique_ptr<GcodeFile> f = found.empty() ? nullptr
                                                 : sd_->fs->open(found);
    if (!f) {
        *err = "Unable to open file";
        return false;
    }
    size_t fsize = f->size();
    gcode_.respond_raw("File opened:" + name + " Size:" + std::to_string(fsize));
    gcode_.respond_raw("File selected");
    sd_->file = std::move(f);
    sd_->file_path = found;
    sd_->file_position = 0;
    sd_->file_size = fsize;
    reset_stats(*pstats_, name);
    return true;
}

void
Printer::sd_do_pause()
{
    if (!sd_->work_active)
        return;
    sd_->must_pause_work = true;
    if (!sd_->cmd_from_sd) {
        // Not inside a file line: the work loop is idle between lines, so
        // it stops right here (the work timer would exit next)
        sd_->work_active = false;
        note_pause(*pstats_, monotonic());
    }
}

bool
Printer::sd_do_resume(std::string *err)
{
    if (sd_->work_active) {
        *err = "SD busy";
        return false;
    }
    sd_->must_pause_work = false;
    sd_->work_active = true;
    sd_->work_started = false;
    return true;
}

void
Printer::sd_do_cancel()
{
    if (sd_->file) {
        sd_do_pause();
        sd_->file.reset();
        note_finish(*pstats_, monotonic(), "cancelled");
    }
    sd_->file_position = sd_->file_size = 0;
}

// File read size (klippy reads 8 KB; small reads keep the ESP32 heap free)
constexpr size_t kSdReadChunk = 1024;

void
Printer::sd_work_slice()
{
    // virtual_sdcard.work_handler(), run in slices from poll()
    Sdcard &sd = *sd_;
    if (!sd.work_started) {
        sd.work_started = true;
        if (!sd.file || !sd.file->seek(sd.file_position)) {
            sd.work_active = false;
            gcode_.respond_raw("!! Unable to seek file");
            return;
        }
        sd.clear_input();
        note_start(*pstats_, monotonic());
    }
    double start = monotonic();
    std::string error_message;
    bool finished = false;
    while (!sd.must_pause_work) {
        // Let other commands (gcode_mutex) and background work run
        {
            std::lock_guard<std::mutex> g(pending_lock_);
            if (!pending_.empty())
                return;
        }
        if (monotonic() - start > 0.050)
            return;
        // Next line of the file
        bool eof = false;
        for (;;) {
            size_t nl = sd.rbuf.find('\n', sd.rpos);
            if (nl != std::string::npos) {
                sd.line.assign(sd.rbuf, sd.rpos, nl - sd.rpos);
                sd.rpos = nl + 1;
                break;
            }
            sd.rbuf.erase(0, sd.rpos);
            sd.rpos = 0;
            size_t have = sd.rbuf.size();
            sd.rbuf.resize(have + kSdReadChunk);
            size_t n = sd.file->read(&sd.rbuf[have], kSdReadChunk);
            sd.rbuf.resize(have + n);
            if (!n) {
                // End of file (like klippy, a last line without a newline
                // is not run)
                eof = true;
                break;
            }
        }
        if (eof) {
            sd.file.reset();
            sd.clear_input();
            gcode_.respond_raw("Done printing file");
            finished = true;
            break;
        }
        sd.cmd_from_sd = true;
        const std::string &line = sd.line;
        size_t next = sd.file_position + line.size() + 1;
        sd.next_file_position = next;
        std::string err;
        static const bool trace = getenv("KL_TRACE_SD") != nullptr;
        if (trace)
            fprintf(stderr, "sd %.3f pt %.3f est %.3f: %s\n", monotonic(),
                    toolhead_->print_time(),
                    mcu_.estimated_print_time(monotonic()), line.c_str());
        bool ok = gcode_.run_line(line, &err);
        sd.cmd_from_sd = false;
        if (!ok) {
            gcode_.respond_raw("!! " + err);
            error_message = err;
            break;
        }
        sd.file_position = sd.next_file_position;
        // M26 (or similar) moved the position
        if (sd.next_file_position != next && sd.file) {
            sd.file->seek(sd.next_file_position);
            sd.clear_input();
        }
    }
    // Work timer exit
    sd.work_active = false;
    sd.cmd_from_sd = false;
    double now = monotonic();
    if (!error_message.empty())
        note_finish(*pstats_, now, "error", error_message);
    else if (!finished && sd.file)
        note_pause(*pstats_, now);
    else
        note_finish(*pstats_, now, "complete");
}

/****************************************************************
 * Main loop
 ****************************************************************/

void
Printer::submit_script(std::string script,
                       std::function<void(bool, const std::string &)> done)
{
    std::lock_guard<std::mutex> g(pending_lock_);
    pending_.push_back({std::move(script), std::move(done)});
}

bool
Printer::run_pending()
{
    bool any = false;
    for (;;) {
        Pending p;
        {
            std::lock_guard<std::mutex> g(pending_lock_);
            if (pending_.empty())
                break;
            p = std::move(pending_.front());
            pending_.pop_front();
        }
        any = true;
        std::string err;
        if (!toolhead_ || !is_ready()) {
            // klippy: G-code is refused unless the printer is ready
            err = shutdown_msg_.empty() ? "Printer is not ready"
                                        : state_message();
            gcode_.respond_raw("!! " + err);
            if (p.done)
                p.done(false, err);
            continue;
        }
        running_cmd_ = true;
        publish_snapshot(monotonic());
        bool ok = gcode_.run_script(p.script, &err);
        running_cmd_ = false;
        next_snapshot_ = 0.;
        if (!ok)
            gcode_.respond_raw("!! " + err);
        if (p.done)
            p.done(ok, err);
    }
    return any;
}

void
Printer::poll()
{
    run_calls();
    bool busy = run_pending();
    if (sd_ && sd_->work_active && is_ready()) {
        sd_work_slice();
        busy = true;
    }
    service();
    if (!busy)
        sleep_s(0.005);
}

/****************************************************************
 * Setup and G-code commands
 ****************************************************************/

bool
Printer::load_sdcard(std::set<std::string> *handled, std::string *err)
{
    (void)err;
    // gcode_move.py SAVE_GCODE_STATE / RESTORE_GCODE_STATE (always)
    gcode_.register_command("SAVE_GCODE_STATE", [this](GCodeCommand &g,
                                                       std::string *e) {
        std::string name;
        if (!g.get("NAME", &name, e, "default"))
            return false;
        saved_states_[name] = gm_;
        return true;
    });
    gcode_.register_command("RESTORE_GCODE_STATE", [this](GCodeCommand &g,
                                                          std::string *e) {
        std::string name;
        if (!g.get("NAME", &name, e, "default"))
            return false;
        auto it = saved_states_.find(name);
        if (it == saved_states_.end()) {
            *e = "Unknown g-code state: " + name;
            return false;
        }
        const GCodeMoveState &st = it->second;
        gm_.absolute_coord = st.absolute_coord;
        gm_.absolute_extrude = st.absolute_extrude;
        for (int i = 0; i < 4; i++) {
            gm_.base_position[i] = st.base_position[i];
            gm_.homing_position[i] = st.homing_position[i];
        }
        gm_.speed = st.speed;
        gm_.speed_factor = st.speed_factor;
        gm_.extrude_factor = st.extrude_factor;
        gm_.base_position[3] += gm_.last_position[3] - st.last_position[3];
        int64_t zero = 0, move;
        if (!g.get_int("MOVE", &move, e, &zero))
            return false;
        if (move) {
            double speed;
            if (!g.get_float("MOVE_SPEED", &speed, e, &gm_.speed, NAN, NAN, 0.))
                return false;
            for (int i = 0; i < 3; i++)
                gm_.last_position[i] = st.last_position[i];
            if (!toolhead_->move(gm_.last_position, speed, e))
                return false;
            throttle();
        }
        return true;
    });

    if (config_.has_section("virtual_sdcard")) {
        ConfigSection sec = config_.getsection("virtual_sdcard");
        sec.get("path"); // a Linux path; storage comes from set_filesystem()
        sec.get("on_error_gcode", "");
        sd_ = std::make_unique<Sdcard>();
        pstats_ = std::make_unique<PrintStats>();
        handled->insert("virtual_sdcard");
        using G = GCodeCommand;
        gcode_.register_command("SDCARD_PRINT_FILE", [this](G &g, std::string *e) {
            if (sd_->work_active) {
                *e = "SD busy";
                return false;
            }
            sd_->file.reset();
            sd_->file_position = sd_->file_size = 0;
            std::string name;
            if (!g.get("FILENAME", &name, e) || !sd_load_file(name, e))
                return false;
            return sd_do_resume(e);
        });
        gcode_.register_command("SDCARD_RESET_FILE", [this](G &, std::string *e) {
            if (sd_->cmd_from_sd) {
                *e = "SDCARD_RESET_FILE cannot be run from the sdcard";
                return false;
            }
            sd_do_pause();
            sd_->file.reset();
            sd_->file_position = sd_->file_size = 0;
            reset_stats(*pstats_);
            return true;
        });
        gcode_.register_command("M20", [this](G &, std::string *) {
            gcode_.respond_raw("Begin file list");
            if (sd_->fs)
                for (const auto &f : sd_->fs->list())
                    gcode_.respond_raw(f.first + " " + std::to_string(f.second));
            gcode_.respond_raw("End file list");
            return true;
        });
        gcode_.register_command("M21", [this](G &, std::string *) {
            gcode_.respond_raw("SD card ok");
            return true;
        });
        gcode_.register_command("M23", [this](G &g, std::string *e) {
            if (sd_->work_active) {
                *e = "SD busy";
                return false;
            }
            sd_->file.reset();
            sd_->file_position = sd_->file_size = 0;
            std::string name = g.get_raw_command_parameters();
            size_t b = name.find_first_not_of(" \t"),
                   en = name.find_last_not_of(" \t");
            name = b == std::string::npos ? "" : name.substr(b, en - b + 1);
            return sd_load_file(name, e);
        });
        gcode_.register_command("M24", [this](G &, std::string *e) {
            return sd_do_resume(e);
        });
        gcode_.register_command("M25", [this](G &, std::string *) {
            sd_do_pause();
            return true;
        });
        gcode_.register_command("M26", [this](G &g, std::string *e) {
            if (sd_->work_active) {
                *e = "SD busy";
                return false;
            }
            int64_t pos;
            if (!g.get_int("S", &pos, e))
                return false;
            sd_->file_position = (size_t)std::max<int64_t>(0, pos);
            return true;
        });
        gcode_.register_command("M27", [this](G &, std::string *) {
            if (!sd_->file)
                gcode_.respond_raw("Not SD printing.");
            else
                gcode_.respond_raw("SD printing byte "
                                   + std::to_string(sd_->file_position) + "/"
                                   + std::to_string(sd_->file_size));
            return true;
        });
        gcode_.register_command("SET_PRINT_STATS_INFO", [](G &, std::string *) {
            return true; // layer info: not shown on a plotter
        });
    }

    if (config_.has_section("pause_resume")) {
        ConfigSection sec = config_.getsection("pause_resume");
        recover_velocity_ = sec.getfloat("recover_velocity", 50.);
        has_pause_resume_ = true;
        handled->insert("pause_resume");
        using G = GCodeCommand;
        auto sd_active = [this] { return sd_ && sd_->work_active; };
        gcode_.register_command("PAUSE", [this, sd_active](G &, std::string *e) {
            if (is_paused_) {
                gcode_.respond_info("Print already paused");
                return true;
            }
            if (!pause_command_sent_) {
                if (sd_active()) {
                    sd_paused_ = true;
                    sd_do_pause();
                } else {
                    sd_paused_ = false;
                    gcode_.respond_info("action:paused");
                }
                pause_command_sent_ = true;
            }
            if (!gcode_.run_script("SAVE_GCODE_STATE NAME=PAUSE_STATE", e))
                return false;
            is_paused_ = true;
            return true;
        });
        gcode_.register_command("RESUME", [this](G &g, std::string *e) {
            if (!is_paused_) {
                gcode_.respond_info("Print is not paused, resume aborted");
                return true;
            }
            double v;
            if (!g.get_float("VELOCITY", &v, e, &recover_velocity_))
                return false;
            char buf[96];
            snprintf(buf, sizeof(buf),
                     "RESTORE_GCODE_STATE NAME=PAUSE_STATE MOVE=1"
                     " MOVE_SPEED=%.4f", v);
            if (!gcode_.run_script(buf, e))
                return false;
            if (sd_paused_) {
                if (!sd_do_resume(e))
                    return false;
                sd_paused_ = false;
            } else {
                gcode_.respond_info("action:resumed");
            }
            pause_command_sent_ = false;
            is_paused_ = false;
            return true;
        });
        gcode_.register_command("CLEAR_PAUSE", [this](G &, std::string *) {
            is_paused_ = pause_command_sent_ = false;
            return true;
        });
        gcode_.register_command("CANCEL_PRINT", [this, sd_active](G &,
                                                                  std::string *) {
            if (sd_active() || sd_paused_)
                sd_do_cancel();
            else
                gcode_.respond_info("action:cancel");
            is_paused_ = pause_command_sent_ = false;
            return true;
        });
    }
    if (config_.has_section("print_stats"))
        handled->insert("print_stats");
    return true;
}

} // namespace kl
