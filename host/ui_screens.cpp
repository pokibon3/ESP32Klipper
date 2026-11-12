// Renders the local UI (components/ui) in a few printer states to PNG
// files, to check the layout without the board.
//
//   ./build/host/ui_screens build/ui_screens
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <chrono>
#include <thread>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <zlib.h>

#include "chelper_api.h"
#include "lvgl.h"
#include "ui/plotter_ui.hpp"

namespace kl {
uint64_t now_us() { return (uint64_t)(get_monotonic() * 1e6); }
int64_t heap_free_bytes() { return -1; }
int64_t heap_min_free_bytes() { return -1; }
void heap_monitor_start() {}
int64_t heap_monitor_stop() { return -1; }
std::string task_report() { return ""; }
} // namespace kl

extern "C" uint32_t
lv_port_tick_ms(void)
{
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    return (uint32_t)duration_cast<milliseconds>(steady_clock::now() - t0)
        .count();
}

namespace {

constexpr int W = 320, H = 240;
lv_color_t framebuf[W * H];
lv_color_t drawbuf[W * H];

void
flush_cb(lv_disp_drv_t *drv, const lv_area_t *a, lv_color_t *px)
{
    for (int y = a->y1; y <= a->y2; y++)
        for (int x = a->x1; x <= a->x2; x++)
            framebuf[y * W + x] = *px++;
    lv_disp_flush_ready(drv);
}

void
put32(std::vector<uint8_t> &v, uint32_t x)
{
    for (int i = 3; i >= 0; i--)
        v.push_back((uint8_t)(x >> (i * 8)));
}

void
chunk(FILE *f, const char *type, const std::vector<uint8_t> &data)
{
    std::vector<uint8_t> c;
    put32(c, (uint32_t)data.size());
    c.insert(c.end(), type, type + 4);
    c.insert(c.end(), data.begin(), data.end());
    uint32_t crc = crc32(0, c.data() + 4, (uInt)(c.size() - 4));
    put32(c, crc);
    fwrite(c.data(), 1, c.size(), f);
}

void
write_png(const std::string &path)
{
    std::vector<uint8_t> raw;
    for (int y = 0; y < H; y++) {
        raw.push_back(0);
        for (int x = 0; x < W; x++) {
            lv_color_t c = framebuf[y * W + x];
            uint32_t c32 = lv_color_to32(c);
            raw.push_back((c32 >> 16) & 0xff);
            raw.push_back((c32 >> 8) & 0xff);
            raw.push_back(c32 & 0xff);
        }
    }
    uLongf len = compressBound(raw.size());
    std::vector<uint8_t> z(len);
    compress(z.data(), &len, raw.data(), raw.size());
    z.resize(len);
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) {
        perror(path.c_str());
        return;
    }
    static const uint8_t sig[] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    std::vector<uint8_t> ihdr;
    put32(ihdr, W);
    put32(ihdr, H);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    chunk(f, "IHDR", ihdr);
    chunk(f, "IDAT", z);
    chunk(f, "IEND", {});
    fclose(f);
    printf("wrote %s\n", path.c_str());
}

class FakeBackend : public kl::UiBackend {
public:
    kl::PrinterSnapshot s;
    std::vector<std::string> ran;
    kl::PrinterSnapshot snapshot() override { return s; }
    std::vector<std::pair<std::string, size_t>> list_files() override {
        return {{"card_opt.gcode", 184320},
                {"flowers/rose_a4.gcode", 1572864},
                {"test_square.gcode", 812}};
    }
    void run_gcode(const std::string &script) override {
        ran.push_back(script);
    }
    std::vector<std::string> list_macros() override {
        return {"PEN_CHANGE", "_HELPER", "CANCEL_PRINT", "PARK", "PAUSE",
                "RESUME", "TEST_PATTERN"};
    }
    bool can_restart() override { return true; }
    std::vector<std::string> system_info() override {
        return {"Board: CYD (ESP32), screen + microSD",
                "G-code files: /sd/gcodes", "RAM free 61 KB (lowest 38 KB)",
                "Up 0:12:34"};
    }
    void changed() { s.seq++; }
};

void
render()
{
    // Let the theme's style transitions finish
    auto until = std::chrono::steady_clock::now()
                 + std::chrono::milliseconds(400);
    while (std::chrono::steady_clock::now() < until) {
        lv_timer_handler();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(nullptr);
}

} // namespace

int
main(int argc, char **argv)
{
    std::string out = argc > 1 ? argv[1] : "build/ui_screens";
    lv_init();
    static lv_disp_draw_buf_t buf;
    lv_disp_draw_buf_init(&buf, drawbuf, nullptr, W * H);
    static lv_disp_drv_t drv;
    lv_disp_drv_init(&drv);
    drv.hor_res = W;
    drv.ver_res = H;
    drv.flush_cb = flush_cb;
    drv.draw_buf = &buf;
    lv_disp_drv_register(&drv);

    FakeBackend be;
    kl::PrinterSnapshot &s = be.s;
    s.ready = true;
    s.pen_pin = "LASER";
    s.axis_max[0] = 250.;
    s.axis_max[1] = 180.;
    kl::PlotterUi ui(be);
    ui.create();

    // Idle after power-on
    be.changed();
    ui.update();
    render();
    write_png(out + "/1_status_idle.png");

    // Printing
    s.homed_axes = "xyz";
    s.live_pos[0] = 123.4;
    s.live_pos[1] = 56.7;
    s.pen_value = 1.;
    s.print.state = "printing";
    s.print.filename = "card_opt.gcode";
    s.print.print_duration = 61.;
    s.progress = 0.42;
    s.speed_factor = 120.;
    s.sd_active = true;
    be.changed();
    ui.update();
    render();
    write_png(out + "/2_status_printing.png");

    // Paused
    s.print.state = "paused";
    s.pen_value = 0.;
    be.changed();
    ui.update();
    render();
    write_png(out + "/3_status_paused.png");

    // Cancel confirmation
    ui.confirm_cancel();
    render();
    write_png(out + "/4_confirm_cancel.png");
    ui.close_dialog();

    ui.confirm_print("card_opt.gcode");
    render();
    write_png(out + "/4b_confirm_print.png");
    ui.close_dialog();

    // Files
    s.print.state = "complete";
    s.progress = 1.;
    s.print.print_duration = 131.6;
    be.changed();
    ui.update();
    ui.show_tab(kl::PlotterUi::TAB_FILES);
    render();
    write_png(out + "/5_files.png");

    // Move tab
    ui.show_tab(kl::PlotterUi::TAB_MOVE);
    render();
    write_png(out + "/6_move.png");

    // Macros, System
    ui.show_tab(kl::PlotterUi::TAB_MACROS);
    ui.refresh_macros();
    render();
    write_png(out + "/6b_macros.png");
    ui.show_tab(kl::PlotterUi::TAB_SYSTEM);
    ui.update();
    ui.update();
    render();
    write_png(out + "/6c_system.png");
    ui.confirm_estop();
    render();
    write_png(out + "/6d_confirm_estop.png");
    ui.close_dialog();
    ui.show_tab(kl::PlotterUi::TAB_MOVE);

    // Error toast (jog before homing)
    s.homed_axes = "";
    s.last_error = "Must home axis first: 10.000 0.000 0.000 [0.000]";
    s.error_seq++;
    be.changed();
    ui.update();
    render();
    write_png(out + "/7_error.png");

    // Shutdown
    ui.show_tab(kl::PlotterUi::TAB_STATUS);
    s.ready = false;
    s.state_message = "Shutdown: Timer too close\nOnce the underlying issue"
                      " is corrected, use the FIRMWARE_RESTART command.";
    be.changed();
    ui.update();
    render();
    write_png(out + "/8_shutdown.png");
    return 0;
}
