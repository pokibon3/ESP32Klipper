// Local touch UI (see plotter_ui.hpp).
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "ui/plotter_ui.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "lvgl.h"

namespace kl {

namespace {

constexpr int kHeaderH = 28;
constexpr uint32_t kUpdateMs = 500; // status refresh
constexpr int kTabBarH = 40;

const lv_font_t *const kFontSmall = &lv_font_montserrat_14;
const lv_font_t *const kFontMid = &lv_font_montserrat_20;
const lv_font_t *const kFontBig = &lv_font_montserrat_28;

void
set_text(lv_obj_t *label, const char *text)
{
    const char *old = lv_label_get_text(label);
    if (!old || strcmp(old, text) != 0)
        lv_label_set_text(label, text);
}

// The setters below touch the object only on a change: LVGL redraws (and
// may re-layout) on every style/flag/state call, and drawing competes with
// klippy for the flash cache shared by both cores
void
set_enabled(lv_obj_t *obj, bool en)
{
    if (en == !lv_obj_has_state(obj, LV_STATE_DISABLED))
        return;
    if (en)
        lv_obj_clear_state(obj, LV_STATE_DISABLED);
    else
        lv_obj_add_state(obj, LV_STATE_DISABLED);
}

void
set_visible(lv_obj_t *obj, bool v)
{
    if (v == !lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN))
        return;
    if (v)
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

std::string
fmt_duration(double secs)
{
    if (secs < 0.)
        secs = 0.;
    long t = (long)(secs + .5);
    char buf[32];
    if (t >= 3600)
        snprintf(buf, sizeof(buf), "%ld:%02ld:%02ld", t / 3600, t / 60 % 60,
                 t % 60);
    else
        snprintf(buf, sizeof(buf), "%ld:%02ld", t / 60, t % 60);
    return buf;
}

std::string
fmt_size(size_t n)
{
    char buf[32];
    if (n >= 1024 * 1024)
        snprintf(buf, sizeof(buf), "%.1f MB", n / 1048576.);
    else if (n >= 1024)
        snprintf(buf, sizeof(buf), "%.1f KB", n / 1024.);
    else
        snprintf(buf, sizeof(buf), "%u B", (unsigned)n);
    return buf;
}

lv_obj_t *
make_button(lv_obj_t *parent, const char *text, lv_color_t color, int w,
            int h)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, color, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x33383d), LV_STATE_DISABLED);
    lv_obj_set_style_text_color(b, lv_color_hex(0x80868b), LV_STATE_DISABLED);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return b;
}

void
set_button_text(lv_obj_t *btn, const char *text)
{
    set_text(lv_obj_get_child(btn, 0), text);
}

lv_obj_t *
plain_container(lv_obj_t *parent)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

// Jog pad (btnmatrix) layout; hidden buttons keep the grid square
// ("" ends a map, so the hidden corner buttons are " ")
const char *const kJogMap[] = {" ",  "Y+",           " ",  "\n",
                               "X-", LV_SYMBOL_HOME, "X+", "\n",
                               " ",  "Y-",           " ",  ""};
enum { JOG_YP = 1, JOG_XM = 3, JOG_HOME = 4, JOG_XP = 5, JOG_YM = 7 };
const char *const kStepMap[] = {"1", "10", "50", ""};
const double kSteps[] = {1., 10., 50.};

} // namespace

struct PlotterUi::Widgets {
    lv_obj_t *tabview = nullptr;
    // header
    lv_obj_t *state_dot = nullptr, *state = nullptr, *pos = nullptr;
    // status tab
    lv_obj_t *file = nullptr, *bar = nullptr, *percent = nullptr;
    lv_obj_t *times = nullptr, *message = nullptr;
    lv_obj_t *btn_pause = nullptr, *btn_cancel = nullptr;
    lv_obj_t *speed_row = nullptr, *speed = nullptr;
    // files tab
    lv_obj_t *file_count = nullptr, *file_list = nullptr;
    // move tab
    lv_obj_t *jog = nullptr, *steps = nullptr;
    lv_obj_t *btn_pen_up = nullptr, *btn_pen_down = nullptr;
    lv_obj_t *btn_motors_off = nullptr, *pen_state = nullptr;
    // macros tab
    lv_obj_t *macro_list = nullptr, *macro_empty = nullptr;
    int macros_on = -1; // buttons enabled (-1 = not set yet)
    // system tab
    lv_obj_t *sys_info = nullptr, *btn_estop = nullptr, *btn_restart = nullptr;
    lv_obj_t *btn_wifi = nullptr;
    // WiFi pages (on the top layer while open)
    lv_obj_t *wifi_page = nullptr, *wifi_status = nullptr, *wifi_list = nullptr;
    lv_obj_t *wifi_ap_btn = nullptr;
    int wifi_ap_shown = -1; // AP mode shown on wifi_ap_btn
    lv_obj_t *pw_page = nullptr, *pw_text = nullptr;
    // error toast
    lv_obj_t *toast = nullptr, *toast_label = nullptr;
    lv_timer_t *toast_timer = nullptr;
    lv_obj_t *dialog = nullptr;
};

std::vector<std::pair<std::string, size_t>>
PrinterUiBackend::list_files()
{
    auto fs = p_.filesystem();
    if (!fs)
        return {};
    return fs->list();
}

std::vector<std::string>
PrinterUiBackend::list_macros()
{
    std::vector<std::string> out;
    p_.run_on_klippy([&] {
        for (const auto &n : p_.object_names())
            if (n.compare(0, 12, "gcode_macro ") == 0)
                out.push_back(n.substr(12));
    });
    return out;
}

void
PrinterUiBackend::emergency_stop()
{
    // Like Moonraker's printer.emergency_stop: not queued behind a running
    // command
    p_.run_on_klippy([this] {
        std::string e;
        p_.run_script("M112", &e);
    });
}

void
PrinterUiBackend::restart()
{
    p_.run_on_klippy([this] { p_.reset_mcu(); });
    if (restart_board)
        restart_board();
}

PlotterUi::PlotterUi(UiBackend &backend)
    : backend_(backend), w_(new Widgets)
{
}

PlotterUi::~PlotterUi()
{
    if (timer_)
        lv_timer_del(timer_);
    if (w_->toast_timer)
        lv_timer_del(w_->toast_timer);
    delete w_;
}

void
PlotterUi::create()
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    build_header(scr);

    lv_obj_t *tv = lv_tabview_create(scr, LV_DIR_BOTTOM, kTabBarH);
    w_->tabview = tv;
    lv_obj_set_size(tv, LV_HOR_RES, LV_VER_RES - kHeaderH);
    lv_obj_set_pos(tv, 0, kHeaderH);
    // Tabs change by the tab bar only (no swiping while jogging)
    lv_obj_clear_flag(lv_tabview_get_content(tv), LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *t_status = lv_tabview_add_tab(tv, LV_SYMBOL_HOME "\nStatus");
    lv_obj_t *t_files = lv_tabview_add_tab(tv, LV_SYMBOL_DIRECTORY "\nFiles");
    lv_obj_t *t_move = lv_tabview_add_tab(tv, LV_SYMBOL_GPS "\nMove");
    lv_obj_t *t_macros = lv_tabview_add_tab(tv, LV_SYMBOL_LIST "\nMacros");
    lv_obj_t *t_system = lv_tabview_add_tab(tv, LV_SYMBOL_SETTINGS "\nSystem");
    for (lv_obj_t *t : {t_status, t_files, t_move, t_macros, t_system}) {
        lv_obj_set_style_pad_all(t, 6, 0);
        lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_t *btns = lv_tabview_get_tab_btns(tv);
    lv_obj_set_style_text_font(btns, kFontSmall, 0);
    lv_obj_add_event_cb(
        tv,
        [](lv_event_t *e) {
            auto *self = (PlotterUi *)lv_event_get_user_data(e);
            switch (lv_tabview_get_tab_act(self->w_->tabview)) {
            case TAB_FILES: self->refresh_files(); break;
            case TAB_MACROS: self->refresh_macros(); break;
            case TAB_SYSTEM: self->update_system(); break;
            default: break;
            }
        },
        LV_EVENT_VALUE_CHANGED, this);
    build_status(t_status);
    build_files(t_files);
    build_move(t_move);
    build_macros(t_macros);
    build_system(t_system);

    // Error toast over everything (tap to close)
    lv_obj_t *toast = lv_obj_create(lv_layer_top());
    w_->toast = toast;
    lv_obj_set_width(toast, LV_HOR_RES - 16);
    lv_obj_set_height(toast, LV_SIZE_CONTENT);
    lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, -kTabBarH - 6);
    lv_obj_set_style_bg_color(toast, lv_palette_darken(LV_PALETTE_RED, 2), 0);
    lv_obj_set_style_border_width(toast, 0, 0);
    lv_obj_set_style_pad_all(toast, 8, 0);
    lv_obj_add_flag(toast, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(toast, LV_OBJ_FLAG_SCROLLABLE);
    w_->toast_label = lv_label_create(toast);
    lv_obj_set_width(w_->toast_label, lv_pct(100));
    lv_label_set_long_mode(w_->toast_label, LV_LABEL_LONG_WRAP);
    lv_obj_add_event_cb(
        toast, [](lv_event_t *e) { lv_obj_add_flag(lv_event_get_target(e),
                                                   LV_OBJ_FLAG_HIDDEN); },
        LV_EVENT_CLICKED, nullptr);
    set_visible(toast, false);

    timer_ = lv_timer_create(on_timer, kUpdateMs, this);
    update();
}

void
PlotterUi::build_header(lv_obj_t *scr)
{
    lv_obj_t *h = plain_container(scr);
    lv_obj_set_size(h, LV_HOR_RES, kHeaderH);
    lv_obj_set_style_bg_color(h, lv_color_hex(0x202428), 0);
    lv_obj_set_style_bg_opa(h, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(h, 8, 0);

    w_->state_dot = lv_obj_create(h);
    lv_obj_remove_style_all(w_->state_dot);
    lv_obj_set_size(w_->state_dot, 10, 10);
    lv_obj_set_style_radius(w_->state_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(w_->state_dot, LV_OPA_COVER, 0);
    lv_obj_align(w_->state_dot, LV_ALIGN_LEFT_MID, 0, 0);

    w_->state = lv_label_create(h);
    lv_obj_set_style_text_font(w_->state, kFontSmall, 0);
    lv_obj_align(w_->state, LV_ALIGN_LEFT_MID, 16, 0);
    lv_label_set_text(w_->state, "");

    w_->pos = lv_label_create(h);
    lv_obj_set_style_text_font(w_->pos, kFontSmall, 0);
    lv_obj_align(w_->pos, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_label_set_text(w_->pos, "");
}

void
PlotterUi::build_status(lv_obj_t *tab)
{
    w_->file = lv_label_create(tab);
    lv_obj_set_width(w_->file, lv_pct(100));
    lv_label_set_long_mode(w_->file, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(w_->file, kFontMid, 0);
    lv_obj_align(w_->file, LV_ALIGN_TOP_LEFT, 0, 0);

    w_->bar = lv_bar_create(tab);
    lv_obj_set_size(w_->bar, 218, 16);
    lv_bar_set_range(w_->bar, 0, 1000);
    lv_obj_align(w_->bar, LV_ALIGN_TOP_LEFT, 0, 38);

    w_->percent = lv_label_create(tab);
    lv_obj_set_style_text_font(w_->percent, kFontBig, 0);
    lv_obj_align(w_->percent, LV_ALIGN_TOP_RIGHT, 0, 30);
    lv_label_set_text(w_->percent, "");

    w_->times = lv_label_create(tab);
    lv_obj_set_style_text_color(w_->times, lv_palette_lighten(LV_PALETTE_GREY, 2),
                                0);
    lv_obj_align(w_->times, LV_ALIGN_TOP_LEFT, 0, 64);
    lv_label_set_text(w_->times, "");

    w_->message = lv_label_create(tab);
    lv_obj_set_width(w_->message, lv_pct(100));
    lv_label_set_long_mode(w_->message, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(w_->message, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_align(w_->message, LV_ALIGN_TOP_LEFT, 0, 38);
    lv_label_set_text(w_->message, "");

    // Speed factor (M220) while printing: KlipperScreen's "fine tune"
    lv_obj_t *row = plain_container(tab);
    w_->speed_row = row;
    lv_obj_set_size(row, lv_pct(100), 30);
    lv_obj_align(row, LV_ALIGN_TOP_LEFT, 0, 84);
    w_->speed = lv_label_create(row);
    lv_obj_align(w_->speed, LV_ALIGN_LEFT_MID, 0, 0);
    lv_label_set_text(w_->speed, "");
    lv_obj_t *slower = make_button(row, LV_SYMBOL_MINUS,
                                   lv_palette_main(LV_PALETTE_BLUE_GREY), 56, 30);
    lv_obj_align(slower, LV_ALIGN_RIGHT_MID, -62, 0);
    lv_obj_add_event_cb(
        slower,
        [](lv_event_t *e) {
            auto *self = (PlotterUi *)lv_event_get_user_data(e);
            self->set_speed(self->snap_.speed_factor - 10.);
        },
        LV_EVENT_CLICKED, this);
    lv_obj_t *faster = make_button(row, LV_SYMBOL_PLUS,
                                   lv_palette_main(LV_PALETTE_BLUE_GREY), 56, 30);
    lv_obj_align(faster, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(
        faster,
        [](lv_event_t *e) {
            auto *self = (PlotterUi *)lv_event_get_user_data(e);
            self->set_speed(self->snap_.speed_factor + 10.);
        },
        LV_EVENT_CLICKED, this);
    set_visible(row, false);

    w_->btn_pause = make_button(tab, LV_SYMBOL_PAUSE " Pause",
                                lv_palette_main(LV_PALETTE_AMBER), 146, 44);
    lv_obj_align(w_->btn_pause, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_add_event_cb(
        w_->btn_pause,
        [](lv_event_t *e) {
            auto *self = (PlotterUi *)lv_event_get_user_data(e);
            if (self->snap_.print.state == "paused")
                self->backend_.run_gcode("RESUME");
            else
                self->backend_.run_gcode("PAUSE");
        },
        LV_EVENT_CLICKED, this);

    w_->btn_cancel = make_button(tab, LV_SYMBOL_STOP " Cancel",
                                 lv_palette_main(LV_PALETTE_RED), 146, 44);
    lv_obj_align(w_->btn_cancel, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(
        w_->btn_cancel,
        [](lv_event_t *e) {
            ((PlotterUi *)lv_event_get_user_data(e))->confirm_cancel();
        },
        LV_EVENT_CLICKED, this);
}

void
PlotterUi::build_files(lv_obj_t *tab)
{
    w_->file_count = lv_label_create(tab);
    lv_obj_align(w_->file_count, LV_ALIGN_TOP_LEFT, 0, 4);
    lv_label_set_text(w_->file_count, "");

    lv_obj_t *refresh = make_button(tab, LV_SYMBOL_REFRESH,
                                    lv_palette_main(LV_PALETTE_BLUE), 44, 28);
    lv_obj_align(refresh, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_add_event_cb(
        refresh,
        [](lv_event_t *e) {
            ((PlotterUi *)lv_event_get_user_data(e))->refresh_files();
        },
        LV_EVENT_CLICKED, this);

    w_->file_list = lv_list_create(tab);
    lv_obj_set_size(w_->file_list, lv_pct(100),
                    LV_VER_RES - kHeaderH - kTabBarH - 12 - 34);
    lv_obj_align(w_->file_list, LV_ALIGN_TOP_LEFT, 0, 34);
    lv_obj_set_style_pad_all(w_->file_list, 0, 0);
}

void
PlotterUi::refresh_files()
{
    auto list = backend_.list_files();
    files_.clear();
    lv_obj_clean(w_->file_list);
    for (auto &f : list) {
        files_.push_back(f.first);
        std::string text = f.first + "  (" + fmt_size(f.second) + ")";
        lv_obj_t *b = lv_list_add_btn(w_->file_list, LV_SYMBOL_FILE,
                                      text.c_str());
        lv_obj_t *lbl = lv_obj_get_child(b, 1);
        if (lbl)
            lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_user_data(b, (void *)(intptr_t)(files_.size() - 1));
        lv_obj_add_event_cb(
            b,
            [](lv_event_t *e) {
                auto *self = (PlotterUi *)lv_event_get_user_data(e);
                size_t i = (size_t)(intptr_t)lv_obj_get_user_data(
                    lv_event_get_current_target(e));
                if (i < self->files_.size())
                    self->confirm_print(self->files_[i]);
            },
            LV_EVENT_CLICKED, this);
    }
    char buf[48];
    snprintf(buf, sizeof(buf), "%u file%s", (unsigned)list.size(),
             list.size() == 1 ? "" : "s");
    set_text(w_->file_count, buf);
}

void
PlotterUi::build_move(lv_obj_t *tab)
{
    // Jog pad
    lv_obj_t *jog = lv_btnmatrix_create(tab);
    w_->jog = jog;
    lv_btnmatrix_set_map(jog, (const char **)kJogMap);
    lv_obj_set_size(jog, 160, 160);
    lv_obj_align(jog, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_pad_all(jog, 2, 0);
    lv_obj_set_style_pad_gap(jog, 4, 0);
    for (int id : {0, 2, 6, 8})
        lv_btnmatrix_set_btn_ctrl(jog, id, LV_BTNMATRIX_CTRL_HIDDEN);
    lv_obj_set_style_text_color(jog, lv_color_hex(0x80868b),
                                LV_PART_ITEMS | LV_STATE_DISABLED);
    lv_obj_add_event_cb(
        jog,
        [](lv_event_t *e) {
            auto *self = (PlotterUi *)lv_event_get_user_data(e);
            uint16_t id = lv_btnmatrix_get_selected_btn(self->w_->jog);
            switch (id) {
            case JOG_YP: self->jog('Y', 1.); break;
            case JOG_YM: self->jog('Y', -1.); break;
            case JOG_XP: self->jog('X', 1.); break;
            case JOG_XM: self->jog('X', -1.); break;
            case JOG_HOME: self->backend_.run_gcode("G28"); break;
            default: break;
            }
        },
        LV_EVENT_VALUE_CHANGED, this);

    // Right column: step, pen, motors
    lv_obj_t *col = plain_container(tab);
    lv_obj_set_size(col, 136, 160);
    lv_obj_align(col, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t *step_label = lv_label_create(col);
    lv_label_set_text(step_label, "Step (mm)");
    lv_obj_align(step_label, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *steps = lv_btnmatrix_create(col);
    w_->steps = steps;
    lv_btnmatrix_set_map(steps, (const char **)kStepMap);
    lv_btnmatrix_set_btn_ctrl_all(steps, LV_BTNMATRIX_CTRL_CHECKABLE);
    lv_btnmatrix_set_one_checked(steps, true);
    lv_btnmatrix_set_btn_ctrl(steps, 1, LV_BTNMATRIX_CTRL_CHECKED);
    lv_obj_set_size(steps, 136, 38);
    lv_obj_set_style_pad_all(steps, 2, 0);
    lv_obj_set_style_pad_gap(steps, 4, 0);
    lv_obj_align(steps, LV_ALIGN_TOP_LEFT, 0, 18);
    lv_obj_add_event_cb(
        steps,
        [](lv_event_t *e) {
            auto *self = (PlotterUi *)lv_event_get_user_data(e);
            uint16_t id = lv_btnmatrix_get_selected_btn(self->w_->steps);
            if (id < 3)
                self->jog_step_ = kSteps[id];
        },
        LV_EVENT_VALUE_CHANGED, this);

    w_->btn_pen_up = make_button(col, "Pen up", lv_palette_main(LV_PALETTE_BLUE),
                                 66, 38);
    lv_obj_align(w_->btn_pen_up, LV_ALIGN_TOP_LEFT, 0, 66);
    lv_obj_add_event_cb(
        w_->btn_pen_up,
        [](lv_event_t *e) {
            auto *self = (PlotterUi *)lv_event_get_user_data(e);
            if (!self->snap_.pen_pin.empty())
                self->backend_.run_gcode("SET_PIN PIN=" + self->snap_.pen_pin
                                         + " VALUE=0");
        },
        LV_EVENT_CLICKED, this);
    w_->btn_pen_down = make_button(col, "Pen dn",
                                   lv_palette_main(LV_PALETTE_BLUE), 66, 38);
    lv_obj_align(w_->btn_pen_down, LV_ALIGN_TOP_RIGHT, 0, 66);
    lv_obj_add_event_cb(
        w_->btn_pen_down,
        [](lv_event_t *e) {
            auto *self = (PlotterUi *)lv_event_get_user_data(e);
            if (!self->snap_.pen_pin.empty())
                self->backend_.run_gcode("SET_PIN PIN=" + self->snap_.pen_pin
                                         + " VALUE=1");
        },
        LV_EVENT_CLICKED, this);

    w_->btn_motors_off = make_button(col, LV_SYMBOL_POWER " Motors off",
                                     lv_palette_main(LV_PALETTE_GREY), 136, 38);
    lv_obj_align(w_->btn_motors_off, LV_ALIGN_TOP_LEFT, 0, 112);
    lv_obj_add_event_cb(
        w_->btn_motors_off,
        [](lv_event_t *e) {
            ((PlotterUi *)lv_event_get_user_data(e))->backend_.run_gcode("M84");
        },
        LV_EVENT_CLICKED, this);
}

void
PlotterUi::set_speed(double percent)
{
    // 10 % steps from the current value, 10..300 %
    percent = std::round(percent / 10.) * 10.;
    percent = percent < 10. ? 10. : percent > 300. ? 300. : percent;
    char buf[32];
    snprintf(buf, sizeof(buf), "M220 S%d", (int)percent);
    backend_.run_gcode(buf);
}

namespace {
bool
hidden_macro(const std::string &name)
{
    // KlipperScreen hides "_" macros; PAUSE / RESUME / CANCEL_PRINT have
    // their own buttons in the Status tab
    if (name.empty() || name[0] == '_')
        return true;
    std::string u;
    for (char c : name)
        u += (char)toupper((unsigned char)c);
    return u == "PAUSE" || u == "RESUME" || u == "CANCEL_PRINT";
}
} // namespace

void
PlotterUi::build_macros(lv_obj_t *tab)
{
    w_->macro_list = lv_list_create(tab);
    lv_obj_set_size(w_->macro_list, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(w_->macro_list, 0, 0);
    w_->macro_empty = lv_label_create(tab);
    lv_obj_set_width(w_->macro_empty, lv_pct(100));
    lv_label_set_long_mode(w_->macro_empty, LV_LABEL_LONG_WRAP);
    lv_label_set_text(w_->macro_empty,
                      "No macros: add [gcode_macro NAME] sections to"
                      " printer.cfg.");
    lv_obj_align(w_->macro_empty, LV_ALIGN_TOP_LEFT, 0, 4);
    set_visible(w_->macro_empty, false);
}

void
PlotterUi::refresh_macros()
{
    std::vector<std::string> names;
    for (auto &n : backend_.list_macros())
        if (!hidden_macro(n))
            names.push_back(n);
    if (names == macros_ && w_->macros_on >= 0)
        return;
    macros_ = std::move(names);
    lv_obj_clean(w_->macro_list);
    for (size_t i = 0; i < macros_.size(); i++) {
        lv_obj_t *b = lv_list_add_btn(w_->macro_list, LV_SYMBOL_PLAY,
                                      macros_[i].c_str());
        lv_obj_set_style_bg_color(b, lv_color_hex(0x33383d), LV_STATE_DISABLED);
        lv_obj_set_style_text_color(b, lv_color_hex(0x80868b),
                                    LV_STATE_DISABLED);
        lv_obj_set_user_data(b, (void *)(intptr_t)i);
        lv_obj_add_event_cb(
            b,
            [](lv_event_t *e) {
                auto *self = (PlotterUi *)lv_event_get_user_data(e);
                size_t i = (size_t)(intptr_t)lv_obj_get_user_data(
                    lv_event_get_current_target(e));
                if (i < self->macros_.size())
                    self->backend_.run_gcode(self->macros_[i]);
            },
            LV_EVENT_CLICKED, this);
    }
    set_visible(w_->macro_empty, macros_.empty());
    w_->macros_on = -1; // set the button states again
    apply(snap_);
}

void
PlotterUi::build_system(lv_obj_t *tab)
{
    w_->sys_info = lv_label_create(tab);
    lv_obj_set_width(w_->sys_info, lv_pct(100));
    lv_label_set_long_mode(w_->sys_info, LV_LABEL_LONG_WRAP);
    lv_obj_align(w_->sys_info, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_label_set_text(w_->sys_info, "");

    // Two buttons, or three with WiFi setup
    bool wifi = backend_.wifi_supported();
    int bw = wifi ? 96 : 146;
    w_->btn_estop = make_button(tab, LV_SYMBOL_WARNING " E-stop",
                                lv_palette_main(LV_PALETTE_RED), bw, 44);
    lv_obj_align(w_->btn_estop, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_add_event_cb(
        w_->btn_estop,
        [](lv_event_t *e) {
            ((PlotterUi *)lv_event_get_user_data(e))->confirm_estop();
        },
        LV_EVENT_CLICKED, this);
    if (wifi) {
        w_->btn_wifi = make_button(tab, LV_SYMBOL_WIFI " WiFi",
                                   lv_palette_main(LV_PALETTE_TEAL), bw, 44);
        lv_obj_align(w_->btn_wifi, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_add_event_cb(
            w_->btn_wifi,
            [](lv_event_t *e) {
                ((PlotterUi *)lv_event_get_user_data(e))->show_wifi();
            },
            LV_EVENT_CLICKED, this);
    }
    w_->btn_restart = make_button(tab, LV_SYMBOL_REFRESH " Restart",
                                  lv_palette_main(LV_PALETTE_BLUE), bw, 44);
    lv_obj_align(w_->btn_restart, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_add_event_cb(
        w_->btn_restart,
        [](lv_event_t *e) {
            ((PlotterUi *)lv_event_get_user_data(e))->confirm_restart();
        },
        LV_EVENT_CLICKED, this);
}

void
PlotterUi::update_system()
{
    const PrinterSnapshot &s = snap_;
    std::string text = "Klippy: ";
    if (s.ready)
        text += "ready";
    else {
        std::string m = s.state_message;
        size_t nl = m.find('\n');
        text += m.substr(0, nl);
    }
    if (!s.pen_pin.empty())
        text += "\nPen: " + s.pen_pin;
    for (const auto &l : backend_.system_info())
        text += "\n" + l;
    set_text(w_->sys_info, text.c_str());
}

/****************************************************************
 * WiFi setup pages
 ****************************************************************/

namespace {
lv_obj_t *
make_page()
{
    lv_obj_t *pg = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(pg);
    lv_obj_set_size(pg, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(pg, lv_color_hex(0x1d2125), 0);
    lv_obj_set_style_bg_opa(pg, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(pg, 6, 0);
    lv_obj_add_flag(pg, LV_OBJ_FLAG_CLICKABLE); // block the screen below
    lv_obj_clear_flag(pg, LV_OBJ_FLAG_SCROLLABLE);
    return pg;
}

const char *
signal_bars(int rssi)
{
    return rssi > -55 ? "****" : rssi > -67 ? "***" : rssi > -78 ? "**" : "*";
}
} // namespace

void
PlotterUi::show_wifi()
{
    if (w_->wifi_page)
        return;
    lv_obj_t *pg = make_page();
    w_->wifi_page = pg;
    lv_obj_t *title = lv_label_create(pg);
    lv_obj_set_style_text_font(title, kFontMid, 0);
    lv_label_set_text(title, LV_SYMBOL_WIFI); // buttons fill the row
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 4);
    lv_obj_t *close = make_button(pg, LV_SYMBOL_CLOSE,
                                  lv_palette_main(LV_PALETTE_GREY), 48, 32);
    lv_obj_align(close, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_add_event_cb(
        close,
        [](lv_event_t *e) {
            ((PlotterUi *)lv_event_get_user_data(e))->close_wifi();
        },
        LV_EVENT_CLICKED, this);
    lv_obj_t *scan = make_button(pg, LV_SYMBOL_REFRESH " Scan",
                                 lv_palette_main(LV_PALETTE_BLUE), 80, 32);
    lv_obj_align(scan, LV_ALIGN_TOP_RIGHT, -54, 0);
    // Access point mode on / off
    w_->wifi_ap_btn = make_button(pg, "AP mode",
                                  lv_palette_main(LV_PALETTE_TEAL), 92, 32);
    lv_obj_align(w_->wifi_ap_btn, LV_ALIGN_TOP_RIGHT, -140, 0);
    w_->wifi_ap_shown = -1;
    lv_obj_add_event_cb(
        w_->wifi_ap_btn,
        [](lv_event_t *e) {
            auto *self = (PlotterUi *)lv_event_get_user_data(e);
            if (self->backend_.wifi_ap_mode()) {
                self->confirm("Join WiFi?",
                              "Leave access point mode and join the saved "
                              "network.",
                              "Join", [self] {
                                  self->backend_.wifi_set_ap_mode(false);
                              });
            } else {
                self->confirm("Access point mode?",
                              "This board becomes its own WiFi network. "
                              "Mainsail: http://192.168.4.1/",
                              "AP mode", [self] {
                                  self->backend_.wifi_set_ap_mode(true);
                              });
            }
        },
        LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(
        scan,
        [](lv_event_t *e) {
            auto *self = (PlotterUi *)lv_event_get_user_data(e);
            self->wifi_scanning_ = true;
            self->backend_.wifi_scan_start();
            self->fill_wifi_list();
        },
        LV_EVENT_CLICKED, this);
    w_->wifi_status = lv_label_create(pg);
    lv_obj_set_width(w_->wifi_status, lv_pct(100));
    lv_label_set_long_mode(w_->wifi_status, LV_LABEL_LONG_WRAP);
    lv_obj_align(w_->wifi_status, LV_ALIGN_TOP_LEFT, 0, 38);
    lv_label_set_text(w_->wifi_status, "");
    w_->wifi_list = lv_list_create(pg);
    lv_obj_set_size(w_->wifi_list, lv_pct(100), LV_VER_RES - 12 - 78);
    lv_obj_align(w_->wifi_list, LV_ALIGN_TOP_LEFT, 0, 78);
    lv_obj_set_style_pad_all(w_->wifi_list, 0, 0);
    // Scan right away
    wifi_scanning_ = true;
    backend_.wifi_scan_start();
    fill_wifi_list();
    update_wifi();
}

void
PlotterUi::fill_wifi_list()
{
    lv_obj_clean(w_->wifi_list);
    if (wifi_scanning_) {
        lv_list_add_text(w_->wifi_list, "Scanning...");
        return;
    }
    if (wifi_nets_.empty())
        lv_list_add_text(w_->wifi_list, "No networks found");
    for (size_t i = 0; i < wifi_nets_.size(); i++) {
        const WifiNetwork &n = wifi_nets_[i];
        std::string text = n.ssid + "  " + signal_bars(n.rssi);
        lv_obj_t *b = lv_list_add_btn(
            w_->wifi_list, n.secure ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_WIFI,
            text.c_str());
        lv_obj_t *lbl = lv_obj_get_child(b, 1);
        if (lbl)
            lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_user_data(b, (void *)(intptr_t)i);
        lv_obj_add_event_cb(
            b,
            [](lv_event_t *e) {
                auto *self = (PlotterUi *)lv_event_get_user_data(e);
                size_t i = (size_t)(intptr_t)lv_obj_get_user_data(
                    lv_event_get_current_target(e));
                if (i >= self->wifi_nets_.size())
                    return;
                WifiNetwork n = self->wifi_nets_[i];
                if (n.secure) {
                    self->show_wifi_password(n.ssid);
                } else {
                    self->wifi_ssid_ = n.ssid;
                    self->confirm("Join network?", n.ssid.c_str(), "Join",
                                  [self] {
                                      self->backend_.wifi_connect(
                                          self->wifi_ssid_, "");
                                  });
                }
            },
            LV_EVENT_CLICKED, this);
    }
}

void
PlotterUi::update_wifi()
{
    set_text(w_->wifi_status, backend_.wifi_status().c_str());
    int ap = backend_.wifi_ap_mode() ? 1 : 0;
    if (ap != w_->wifi_ap_shown) {
        w_->wifi_ap_shown = ap;
        set_button_text(w_->wifi_ap_btn, ap ? "Use WiFi" : "AP mode");
    }
    if (wifi_scanning_ && backend_.wifi_scan_results(&wifi_nets_)) {
        wifi_scanning_ = false;
        fill_wifi_list();
    }
}

void
PlotterUi::show_wifi_password(const std::string &ssid)
{
    if (w_->pw_page)
        return;
    wifi_ssid_ = ssid;
    lv_obj_t *pg = make_page();
    w_->pw_page = pg;
    lv_obj_t *title = lv_label_create(pg);
    lv_obj_set_width(title, LV_HOR_RES - 12 - 90);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_label_set_text(title, ("Password: " + ssid).c_str());
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 8);
    lv_obj_t *cancel = make_button(pg, "Cancel",
                                   lv_palette_main(LV_PALETTE_GREY), 84, 32);
    lv_obj_align(cancel, LV_ALIGN_TOP_RIGHT, 0, 0);
    auto close_pw = [](lv_event_t *e) {
        auto *self = (PlotterUi *)lv_event_get_user_data(e);
        if (self->w_->pw_page) {
            lv_obj_del_async(self->w_->pw_page);
            self->w_->pw_page = nullptr;
        }
    };
    lv_obj_add_event_cb(cancel, close_pw, LV_EVENT_CLICKED, this);
    lv_obj_t *ta = lv_textarea_create(pg);
    w_->pw_text = ta;
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_password_mode(ta, true);
    lv_textarea_set_max_length(ta, 64);
    lv_textarea_set_placeholder_text(ta, "Password");
    lv_obj_set_size(ta, LV_HOR_RES - 12, 38);
    lv_obj_align(ta, LV_ALIGN_TOP_LEFT, 0, 36);
    lv_obj_add_state(ta, LV_STATE_FOCUSED); // show the cursor
    lv_obj_t *kb = lv_keyboard_create(pg);
    lv_obj_set_size(kb, LV_HOR_RES, LV_VER_RES - 80);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 6);
    lv_keyboard_set_textarea(kb, ta);
    // Keyboard check mark: join; keyboard key: cancel
    lv_obj_add_event_cb(
        kb,
        [](lv_event_t *e) {
            auto *self = (PlotterUi *)lv_event_get_user_data(e);
            std::string pw = lv_textarea_get_text(self->w_->pw_text);
            self->backend_.wifi_connect(self->wifi_ssid_, pw);
            lv_obj_del_async(self->w_->pw_page);
            self->w_->pw_page = nullptr;
        },
        LV_EVENT_READY, this);
    lv_obj_add_event_cb(kb, close_pw, LV_EVENT_CANCEL, this);
}

void
PlotterUi::close_wifi()
{
    if (w_->pw_page) {
        lv_obj_del(w_->pw_page);
        w_->pw_page = nullptr;
    }
    if (w_->wifi_page) {
        lv_obj_del(w_->wifi_page);
        w_->wifi_page = nullptr;
        w_->wifi_ap_btn = nullptr;
    }
    wifi_scanning_ = false;
}

void
PlotterUi::jog(char axis, double dir)
{
    char buf[96];
    snprintf(buf, sizeof(buf), "G91\nG1 %c%.3f F6000\nG90", axis,
             dir * jog_step_);
    backend_.run_gcode(buf);
}

void
PlotterUi::show_tab(Tab t)
{
    lv_tabview_set_act(w_->tabview, t, LV_ANIM_OFF);
    if (t == TAB_FILES)
        refresh_files();
}

namespace {
// Buttons of the open dialog (LVGL keeps the pointer; one dialog at a time)
const char *dialog_btns[] = {"OK", "Close", ""};

// Bigger buttons than the theme default (touch)
void
style_dialog(lv_obj_t *m)
{
    lv_obj_set_width(m, 280);
    lv_obj_t *btns = lv_msgbox_get_btns(m);
    lv_obj_set_size(btns, lv_pct(100), 48);
    lv_obj_set_style_pad_gap(btns, 12, 0);
    lv_obj_set_style_pad_all(btns, 4, 0);
    lv_obj_set_style_bg_opa(btns, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(btns, lv_palette_main(LV_PALETTE_BLUE),
                              LV_PART_ITEMS);
    lv_obj_set_style_radius(btns, 6, LV_PART_ITEMS);
    lv_obj_center(m);
}
} // namespace

void
PlotterUi::confirm(const char *title, const char *text, const char *ok_label,
                   std::function<void()> action)
{
    if (w_->dialog)
        return;
    dialog_btns[0] = ok_label;
    dialog_action_ = std::move(action);
    lv_obj_t *m = lv_msgbox_create(nullptr, title, text,
                                   (const char **)dialog_btns, false);
    w_->dialog = m;
    style_dialog(m);
    lv_obj_add_event_cb(
        m,
        [](lv_event_t *e) {
            auto *self = (PlotterUi *)lv_event_get_user_data(e);
            lv_obj_t *mb = lv_event_get_current_target(e);
            bool go = lv_msgbox_get_active_btn(mb) == 0;
            lv_msgbox_close(mb);
            self->w_->dialog = nullptr;
            auto action = std::move(self->dialog_action_);
            self->dialog_action_ = nullptr;
            if (go && action)
                action();
        },
        LV_EVENT_VALUE_CHANGED, this);
}

void
PlotterUi::confirm_print(const std::string &filename)
{
    pending_file_ = filename;
    confirm("Start print?", filename.c_str(), "Print", [this] {
        backend_.run_gcode("SDCARD_PRINT_FILE FILENAME=" + pending_file_);
        show_tab(TAB_STATUS);
    });
}

void
PlotterUi::confirm_cancel()
{
    confirm("Cancel print?", "The print stops and cannot be resumed.", "Stop",
            [this] { backend_.run_gcode("CANCEL_PRINT"); });
}

void
PlotterUi::confirm_estop()
{
    confirm("Emergency stop?",
            "The motors stop at once. Restart afterwards.", "E-stop",
            [this] { backend_.emergency_stop(); });
}

void
PlotterUi::confirm_restart()
{
    confirm("Restart?",
            "The MCU and this board restart (FIRMWARE_RESTART).", "Restart",
            [this] { backend_.restart(); });
}

void
PlotterUi::close_dialog()
{
    if (w_->dialog) {
        lv_msgbox_close(w_->dialog);
        w_->dialog = nullptr;
    }
}

void
PlotterUi::show_error(const std::string &msg)
{
    set_text(w_->toast_label, msg.c_str());
    set_visible(w_->toast, true);
    lv_obj_move_foreground(w_->toast);
    if (!w_->toast_timer) {
        w_->toast_timer = lv_timer_create(
            [](lv_timer_t *t) {
                auto *self = (PlotterUi *)t->user_data;
                set_visible(self->w_->toast, false);
                lv_timer_pause(t);
            },
            5000, this);
    }
    lv_timer_reset(w_->toast_timer);
    lv_timer_resume(w_->toast_timer);
}

void
PlotterUi::on_timer(lv_timer_t *t)
{
    ((PlotterUi *)t->user_data)->update();
}

void
PlotterUi::update()
{
    PrinterSnapshot s = backend_.snapshot();
    if (w_->wifi_page)
        update_wifi();
    // System tab: once a second while shown (memory, uptime change)
    if (++ticks_ % 2 == 0 && w_->tabview
        && lv_tabview_get_tab_act(w_->tabview) == TAB_SYSTEM)
        update_system();
    if (!first_update_ && s.seq == last_seq_)
        return;
    if (!first_update_ && s.error_seq != last_error_seq_
        && !s.last_error.empty())
        show_error(s.last_error);
    first_update_ = false;
    last_seq_ = s.seq;
    last_error_seq_ = s.error_seq;
    apply(s);
    snap_ = std::move(s);
}

void
PlotterUi::apply(const PrinterSnapshot &s)
{
    const std::string &ps = s.print.state;
    bool printing = ps == "printing", paused = ps == "paused";
    bool shutdown = !s.ready
                    && s.state_message.compare(0, 8, "Shutdown") == 0;

    // Header: state and live position
    const char *state;
    lv_palette_t color;
    if (shutdown) {
        state = "Shutdown";
        color = LV_PALETTE_RED;
    } else if (!s.ready) {
        state = "Starting";
        color = LV_PALETTE_ORANGE;
    } else if (printing) {
        state = "Printing";
        color = LV_PALETTE_GREEN;
    } else if (paused) {
        state = "Paused";
        color = LV_PALETTE_AMBER;
    } else if (s.busy || s.queued) {
        state = "Busy";
        color = LV_PALETTE_LIGHT_BLUE;
    } else {
        state = "Ready";
        color = LV_PALETTE_LIGHT_BLUE;
    }
    if (strcmp(lv_label_get_text(w_->state), state) != 0) {
        set_text(w_->state, state);
        lv_obj_set_style_bg_color(w_->state_dot, lv_palette_main(color), 0);
    }
    char buf[160];
    if (s.homed_axes.find('x') != std::string::npos
        && s.homed_axes.find('y') != std::string::npos)
        snprintf(buf, sizeof(buf), "X %.1f  Y %.1f  %s", s.live_pos[0],
                 s.live_pos[1], s.pen_value > 0. ? "Pen dn" : "Pen up");
    else
        snprintf(buf, sizeof(buf), "Not homed  %s",
                 s.pen_value > 0. ? "Pen dn" : "Pen up");
    set_text(w_->pos, buf);

    // Status tab
    std::string name = s.print.filename;
    if (name.empty())
        name = "No file";
    set_text(w_->file, name.c_str());
    bool have_job = !s.print.filename.empty();
    set_visible(w_->bar, have_job && !shutdown);
    set_visible(w_->percent, have_job && !shutdown);
    set_visible(w_->times, !shutdown);
    if (lv_obj_get_y(w_->times) != (have_job ? 64 : 34))
        lv_obj_align(w_->times, LV_ALIGN_TOP_LEFT, 0, have_job ? 64 : 34);
    lv_bar_set_value(w_->bar, (int32_t)lround(s.progress * 1000.), LV_ANIM_OFF);
    snprintf(buf, sizeof(buf), "%d%%", (int)(s.progress * 100.));
    set_text(w_->percent, buf);
    std::string times = "Time " + fmt_duration(s.print.print_duration);
    if (!have_job)
        times = "Choose a file in " LV_SYMBOL_DIRECTORY " Files to print.";
    else if (printing && s.progress > 0.01)
        times += "   Left ~"
                 + fmt_duration(s.print.print_duration * (1. - s.progress)
                                / s.progress);
    else if (ps == "complete")
        times += "   Done";
    else if (ps == "cancelled")
        times += "   Cancelled";
    else if (ps == "error")
        times += "   Error";
    set_text(w_->times, times.c_str());
    std::string msg = shutdown ? s.state_message
                      : ps == "error" ? s.print.message
                                      : std::string();
    if (shutdown && backend_.can_restart())
        msg += "\nRestart in " LV_SYMBOL_SETTINGS " System.";
    set_text(w_->message, msg.c_str());
    set_visible(w_->message, !msg.empty());
    if (lv_obj_get_y(w_->message) != (shutdown ? 34 : 86))
        lv_obj_align(w_->message, LV_ALIGN_TOP_LEFT, 0, shutdown ? 34 : 86);

    const char *pause_text = paused ? LV_SYMBOL_PLAY " Resume"
                                    : LV_SYMBOL_PAUSE " Pause";
    if (strcmp(lv_label_get_text(lv_obj_get_child(w_->btn_pause, 0)),
               pause_text) != 0) {
        set_button_text(w_->btn_pause, pause_text);
        lv_obj_set_style_bg_color(
            w_->btn_pause,
            lv_palette_main(paused ? LV_PALETTE_GREEN : LV_PALETTE_AMBER), 0);
    }
    bool job = s.ready && (printing || paused);
    set_visible(w_->speed_row, job);
    if (job) {
        snprintf(buf, sizeof(buf), "Speed %d%%", (int)lround(s.speed_factor));
        set_text(w_->speed, buf);
    }
    set_enabled(w_->btn_pause, s.ready && (printing || paused));
    set_enabled(w_->btn_cancel, s.ready && (printing || paused));

    // Move tab: only while idle
    bool idle = s.ready && !printing && !paused;
    set_enabled(w_->jog, idle);
    set_enabled(w_->btn_pen_up, idle && !s.pen_pin.empty());
    set_enabled(w_->btn_pen_down, idle && !s.pen_pin.empty());
    set_enabled(w_->btn_motors_off, idle);

    // Macros: not while printing (pen changes etc. while paused are fine)
    int macros_on = s.ready && !printing;
    if (macros_on != w_->macros_on) {
        w_->macros_on = macros_on;
        uint32_t n = lv_obj_get_child_cnt(w_->macro_list);
        for (uint32_t i = 0; i < n; i++)
            set_enabled(lv_obj_get_child(w_->macro_list, i), macros_on);
    }
    // System
    set_enabled(w_->btn_estop, s.ready);
    set_enabled(w_->btn_restart, backend_.can_restart());
}

} // namespace kl
