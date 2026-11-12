// MCU link test firmware: runs the M2 (one axis) or M3 (CoreXY squares
// from printer.cfg) test on the ESP32.
//
// When the link shares the console UART (uart:0:...), all console output
// is muted while the link runs and the report is printed afterwards,
// framed by markers that tools/sim_bridge.py uses to switch between
// "console" and "relay to the simulated MCU".
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <cstdio>
#include <mutex>
#include <string>

#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "link_tests/m2_axis.hpp"
#include "link_tests/m3_square.hpp"
#include "link_tests/m4_homing.hpp"
#include "link_tests/m4_macros.hpp"
#include "link_tests/m4_sdcard.hpp"
#include "link_tests/m4_ui_demo.hpp"
#if CONFIG_LINK_TEST_UI
#include "cyd_board/cyd_display.hpp"
#include "ui/plotter_ui.hpp"
#endif

extern const char cfg_start[] asm("_binary_skr_pico_corexy_penplotter_cfg_start");
#ifdef HAVE_PRIVATE_DATA
extern const char frax_cfg[] asm("_binary_fraxinus0pen_printer_cfg_start");
extern const char card_gcode[] asm("_binary_card_opt_gcode_start");
extern const char card_gcode_end[] asm("_binary_card_opt_gcode_end");
#endif

namespace kl {
uint64_t now_us() { return (uint64_t)esp_timer_get_time(); }
int64_t heap_free_bytes() { return heap_caps_get_free_size(MALLOC_CAP_8BIT); }
int64_t heap_min_free_bytes()
{
    return heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
}
void heap_monitor_start() { heap_caps_monitor_local_minimum_free_size_start(); }
int64_t heap_monitor_stop()
{
    int64_t v = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    heap_caps_monitor_local_minimum_free_size_stop();
    return v;
}
std::string task_report()
{
    // vTaskList(): name, state, priority, stack high-water mark (bytes), #
    static char buf[2048];
    vTaskList(buf);
    return buf;
}
} // namespace kl

// Macros nest deeply (homing_override -> _HOME_X -> G28 -> homing move),
// each level rendering a template on this task's stack
static const uint32_t KLIPPY_STACK = 32768;

static const char *LINK_START = "@@LINK_START@@";
static const char *LINK_END = "@@LINK_END@@";

// Mute stdout/stderr of this task and of tasks created from now on
// (newlib copies the global streams into each new task)
struct ConsoleMute {
    FILE *null_ = nullptr, *out_, *err_, *gout_, *gerr_;
    explicit ConsoleMute(bool active) {
        if (!active)
            return;
        fflush(stdout);
        uart_wait_tx_idle_polling(UART_NUM_0);
        esp_log_level_set("*", ESP_LOG_NONE);
        null_ = fopen("/dev/null", "w");
        out_ = stdout;
        err_ = stderr;
        gout_ = _GLOBAL_REENT->_stdout;
        gerr_ = _GLOBAL_REENT->_stderr;
        stdout = stderr = null_;
        _GLOBAL_REENT->_stdout = _GLOBAL_REENT->_stderr = null_;
    }
    ~ConsoleMute() {
        if (!null_)
            return;
        stdout = out_;
        stderr = err_;
        _GLOBAL_REENT->_stdout = gout_;
        _GLOBAL_REENT->_stderr = gerr_;
        fclose(null_);
        esp_log_level_set("*", ESP_LOG_INFO);
    }
};

#if CONFIG_LINK_TEST_UI
// The UI task talks to whichever printer the running test has made
class TestUiBackend : public kl::UiBackend {
public:
    void set(kl::Printer *p) {
        std::lock_guard<std::mutex> g(lock_);
        p_ = p;
    }
    kl::PrinterSnapshot snapshot() override {
        std::lock_guard<std::mutex> g(lock_);
        if (!p_) {
            kl::PrinterSnapshot s;
            s.state_message = "Connecting";
            return s;
        }
        return p_->snapshot();
    }
    std::vector<std::pair<std::string, size_t>> list_files() override {
        std::lock_guard<std::mutex> g(lock_);
        auto fs = p_ ? p_->filesystem() : nullptr;
        return fs ? fs->list() : std::vector<std::pair<std::string, size_t>>();
    }
    void run_gcode(const std::string &script) override {
        std::lock_guard<std::mutex> g(lock_);
        if (p_)
            p_->submit_script(script);
    }

private:
    std::mutex lock_;
    kl::Printer *p_ = nullptr;
};

static TestUiBackend ui_backend;
static kl::PlotterUi *ui;

static void
start_ui()
{
    kl::test_printer_hook = [](kl::Printer *p) { ui_backend.set(p); };
    cyd::start_ui([] {
        ui = new kl::PlotterUi(ui_backend);
        ui->create();
    });
}
#endif

static void
link_task(void *)
{
    std::string port = CONFIG_LINK_TEST_PORT;
    bool shares_console = port.compare(0, 7, "uart:0:") == 0;
    printf("\nESP32Klipper link test: port %s, heap free %u\n", port.c_str(),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    vTaskDelay(pdMS_TO_TICKS(1000));
    std::string report;
    size_t heap_before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    bool ok;
    {
        if (shares_console) {
            printf("%s\n", LINK_START);
            vTaskDelay(pdMS_TO_TICKS(500)); // let the bridge start the MCU
        }
        ConsoleMute mute(shares_console);
#if CONFIG_LINK_TEST_UI_DEMO
#ifdef HAVE_PRIVATE_DATA
        ok = kl::run_m4_ui_demo(port, 250000, frax_cfg, card_gcode,
                                (size_t)(card_gcode_end - card_gcode) - 1,
                                CONFIG_LINK_TEST_DEMO_MINUTES, &report);
#else
        ok = kl::run_m4_ui_demo(port, 250000, cfg_start, nullptr, 0,
                                CONFIG_LINK_TEST_DEMO_MINUTES, &report);
#endif
#elif CONFIG_LINK_TEST_M4_SDCARD
#ifdef HAVE_PRIVATE_DATA
        ok = kl::run_m4_sdcard_test(port, 250000, frax_cfg, card_gcode,
                                    (size_t)(card_gcode_end - card_gcode) - 1,
                                    &report);
#else
        report = "M4 SD test data missing (bench/data)\n";
        ok = false;
#endif
#elif CONFIG_LINK_TEST_M4_MACROS
#ifdef HAVE_PRIVATE_DATA
        // EMBED_TXTFILES adds a NUL terminator
        ok = kl::run_m4_macro_test(port, 250000, frax_cfg, card_gcode,
                                   (size_t)(card_gcode_end - card_gcode) - 1,
                                   CONFIG_LINK_TEST_MAX_LINES, &report);
#else
        report = "M4 macro test data missing (bench/data)\n";
        ok = false;
#endif
#elif CONFIG_LINK_TEST_M4
        ok = kl::run_m4_homing_test(port, 250000, cfg_start, &report);
#elif CONFIG_LINK_TEST_M3
        ok = kl::run_m3_square_test(port, 250000, cfg_start, &report);
#else
        ok = kl::run_m2_axis_test(port, 250000, CONFIG_LINK_TEST_DISTANCE,
                                  &report);
#endif
    }
    if (shares_console) {
        vTaskDelay(pdMS_TO_TICKS(100));
        printf("\n%s\n", LINK_END);
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    printf("%s", report.c_str());
    printf("heap: %u free before, %u min since boot\n", (unsigned)heap_before,
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
    printf("klippy task stack: %u of %u bytes never used\n",
           (unsigned)uxTaskGetStackHighWaterMark(nullptr),
           (unsigned)KLIPPY_STACK);
#if CONFIG_LINK_TEST_UI
    cyd::UiMemory um = cyd::ui_memory();
    printf("ui task: %.1f s busy (%u flushes); heap: drivers %u, screen %u;"
           " stack %u unused\n", cyd::ui_busy_us() * 1e-6,
           (unsigned)cyd::ui_flushes(), (unsigned)um.drivers,
           (unsigned)um.screen, (unsigned)um.stack_unused);
#endif
    printf("link test done: %s\n", ok ? "PASS" : "FAIL");
    vTaskDelete(nullptr);
}

extern "C" void
app_main(void)
{
#if CONFIG_LINK_TEST_UI
    start_ui();
#endif
    // klippy runs on core 1 (design document: core 0 for WiFi/UI)
    xTaskCreatePinnedToCore(link_task, "klippy", KLIPPY_STACK, nullptr, 10,
                            nullptr, 1);
}
