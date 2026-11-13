// Local touch UI for the pen plotter (LVGL 8, 320x240 landscape).
//
// Five tabs (the parts of KlipperScreen a pen plotter uses): Status (state,
// file, progress, speed factor, pause/resume/cancel), Files (print a file
// from storage), Move (jog, home, pen, motors off), Macros (run a
// [gcode_macro]) and System (state, memory, emergency stop, restart).  The
// UI only reads PrinterSnapshot copies and submits G-code scripts, so it
// runs in its own task next to the klippy task.  All calls must come from
// the task that runs lv_timer_handler().
//
// This file may be distributed under the terms of the GNU GPLv3 license.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "klippy/printer.hpp"

struct _lv_obj_t;
struct _lv_timer_t;

namespace kl {

struct WifiNetwork {
    std::string ssid;
    int rssi = 0;
    bool secure = false;
};

class UiBackend {
public:
    virtual ~UiBackend() = default;
    virtual PrinterSnapshot snapshot() = 0;
    virtual std::vector<std::pair<std::string, size_t>> list_files() = 0;
    // Queue a G-code script (runs in the klippy task)
    virtual void run_gcode(const std::string &script) = 0;
    // [gcode_macro] names
    virtual std::vector<std::string> list_macros() { return {}; }
    // M112 at once (not queued behind other commands)
    virtual void emergency_stop() { run_gcode("M112"); }
    // FIRMWARE_RESTART: reset the MCU and restart the host
    virtual bool can_restart() { return false; }
    virtual void restart() {}
    // Extra lines for the System tab (network address, memory, ...)
    virtual std::vector<std::string> system_info() { return {}; }
    // WiFi setup (boards with WiFi): status text, network scan (start,
    // then poll the results until true), join a network (saved)
    virtual bool wifi_supported() { return false; }
    virtual std::string wifi_status() { return ""; }
    virtual void wifi_scan_start() {}
    virtual bool wifi_scan_results(std::vector<WifiNetwork> *) { return true; }
    virtual void wifi_connect(const std::string &ssid,
                              const std::string &password) {}
    // Access point mode (the board is its own network; saved)
    virtual bool wifi_ap_mode() { return false; }
    virtual void wifi_set_ap_mode(bool on) {}
};

// Backend on a Printer in the same process
class PrinterUiBackend : public UiBackend {
public:
    explicit PrinterUiBackend(Printer &p) : p_(p) {}
    PrinterSnapshot snapshot() override { return p_.snapshot(); }
    std::vector<std::pair<std::string, size_t>> list_files() override;
    void run_gcode(const std::string &script) override {
        p_.submit_script(script);
    }
    std::vector<std::string> list_macros() override;
    void emergency_stop() override;
    bool can_restart() override { return (bool)restart_board; }
    // Resets the MCU (if its firmware can), then calls restart_board
    void restart() override;
    std::vector<std::string> system_info() override {
        return info ? info() : std::vector<std::string>();
    }

    // Board functions (set by the firmware)
    std::function<void()> restart_board;
    std::function<std::vector<std::string>()> info;

protected:
    Printer &p_;
};

class PlotterUi {
public:
    enum Tab {
        TAB_STATUS = 0,
        TAB_FILES = 1,
        TAB_MOVE = 2,
        TAB_MACROS = 3,
        TAB_SYSTEM = 4
    };

    explicit PlotterUi(UiBackend &backend);
    ~PlotterUi();
    PlotterUi(const PlotterUi &) = delete;
    PlotterUi &operator=(const PlotterUi &) = delete;

    // Build the screen on the active display (starts a 200 ms update timer)
    void create();
    // Read the backend now (otherwise done by the timer)
    void update();
    void show_tab(Tab t);
    void refresh_files();
    void refresh_macros();
    // Dialogs (also used by the host screenshot tool)
    void confirm_print(const std::string &filename);
    void confirm_cancel();
    void confirm_estop();
    void confirm_restart();
    void close_dialog();
    // WiFi setup pages (System tab)
    void show_wifi();
    void show_wifi_password(const std::string &ssid);
    void close_wifi();

private:
    struct Widgets;
    UiBackend &backend_;
    Widgets *w_;
    _lv_timer_t *timer_ = nullptr;
    uint32_t last_seq_ = 0, last_error_seq_ = 0;
    bool first_update_ = true;
    std::vector<std::string> files_, macros_;
    std::string pending_file_;
    std::function<void()> dialog_action_;
    uint32_t ticks_ = 0;
    std::vector<WifiNetwork> wifi_nets_;
    bool wifi_scanning_ = false;
    std::string wifi_ssid_;
    double jog_step_ = 10.;
    PrinterSnapshot snap_;

    void build_header(_lv_obj_t *scr);
    void build_status(_lv_obj_t *tab);
    void build_files(_lv_obj_t *tab);
    void build_move(_lv_obj_t *tab);
    void build_macros(_lv_obj_t *tab);
    void build_system(_lv_obj_t *tab);
    void update_system();
    void confirm(const char *title, const char *text, const char *ok_label,
                 std::function<void()> action);
    void set_speed(double percent);
    void update_wifi();
    void fill_wifi_list();
    void show_error(const std::string &msg);
    void apply(const PrinterSnapshot &s);
    void jog(char axis, double dir);
    static void on_timer(_lv_timer_t *t);
};

} // namespace kl
