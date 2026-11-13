// ESP32Klipper plotter firmware: Klipper host (printer.cfg from the board's
// storage) with
//   - CoreS3 (PLOTTER_NETWORK): Moonraker-compatible API for Mainsail over
//     WiFi
//   - CoreS3 also shows the touch screen of the CYD (PLOTTER_CORES3_SCREEN)
//   - CYD (PLOTTER_CYD): touch screen + G-code files on the microSD card
//   - CoreS3 microSD (PLOTTER_CORES3_SD): G-code files and printer.cfg on
//     the card, like the CYD (Mainsail uploads go there too)
//     (no WiFi: not enough RAM next to the plotter without PSRAM)
//
// Startup: storage (/data, microSD) -> WiFi -> printer.cfg -> UI / HTTP
// server -> MCU.
// When the MCU link shares the console (usbjtag / uart:0), the console is
// muted while the link runs (tools/sim_bridge.py relays it to the
// simulated MCU) and the IP address is printed before that.
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <sys/stat.h>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#if CONFIG_PLOTTER_CYD
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "klippy/files.hpp"
#include "klippy/printer.hpp"
#include "link_tests/m4_ui_demo.hpp"
#if CONFIG_PLOTTER_NETWORK
#include "moonraker/esp_server.hpp"
#include "moonraker/moonraker.hpp"
#include "wifi_setup.hpp"
#endif
#if CONFIG_PLOTTER_CYD
#include "cyd_board/cyd_display.hpp"
namespace board = cyd;
#elif CONFIG_PLOTTER_CORES3_SCREEN
#include "cores3_board/cores3_display.hpp"
namespace board = cores3;
#endif
#if CONFIG_PLOTTER_CORES3_SD
#include "cores3_board/cores3_display.hpp"
#endif
#if CONFIG_PLOTTER_CYD || CONFIG_PLOTTER_CORES3_SD
#define PLOTTER_SD 1
#endif
#if CONFIG_PLOTTER_CYD || CONFIG_PLOTTER_CORES3_SCREEN
#define PLOTTER_SCREEN 1
#include "ui/plotter_ui.hpp"
#endif

extern const char sample_cfg[] asm("_binary_skr_pico_corexy_penplotter_cfg_start");

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
    static char buf[2048];
    vTaskList(buf);
    return buf;
}
} // namespace kl

static const char *TAG = "plotter";
static const char *DATA = "/data";
static const uint32_t KLIPPY_STACK = 32768;

/****************************************************************
 * Storage
 ****************************************************************/

static bool
mount_storage()
{
    esp_vfs_fat_mount_config_t mc = {};
    mc.format_if_mount_failed = true;
    mc.max_files = 8;
    mc.allocation_unit_size = 4096;
    wl_handle_t wl;
    esp_err_t e = esp_vfs_fat_spiflash_mount_rw_wl(DATA, "storage", &mc, &wl);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "storage mount failed: %s", esp_err_to_name(e));
        return false;
    }
    for (const char *d : {"/data/config", "/data/gcodes", "/data/www"})
        mkdir(d, 0775);
    return true;
}

#if PLOTTER_SD
static const char *SD = "/sd";
#endif

#if CONFIG_PLOTTER_CORES3_SD
// microSD card next to the display on SPI2 (cores3_board)
static bool
mount_sd()
{
    return cores3::mount_sd(SD, CONFIG_PLOTTER_SD_MHZ);
}
#elif CONFIG_PLOTTER_CYD
// microSD card on SPI3 (CYD: SCK 18, MISO 19, MOSI 23, CS 5)

static bool
mount_sd()
{
    spi_bus_config_t bus = {};
    bus.mosi_io_num = 23;
    bus.miso_io_num = 19;
    bus.sclk_io_num = 18;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 4096;
    if (spi_bus_initialize(SPI3_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK)
        return false;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST;
    host.max_freq_khz = CONFIG_PLOTTER_SD_MHZ * 1000;
    sdspi_device_config_t dev = SDSPI_DEVICE_CONFIG_DEFAULT();
    dev.gpio_cs = GPIO_NUM_5;
    dev.host_id = SPI3_HOST;
    esp_vfs_fat_mount_config_t mc = {};
    mc.format_if_mount_failed = false;
    mc.max_files = 4;
    mc.allocation_unit_size = 16 * 1024;
    sdmmc_card_t *card;
    esp_err_t e = esp_vfs_fat_sdspi_mount(SD, &host, &dev, &mc, &card);
    if (e != ESP_OK) {
        printf("microSD: none (%s), using the internal storage\n",
               esp_err_to_name(e));
        spi_bus_free(SPI3_HOST);
        return false;
    }
    printf("microSD: %s, %llu MB\n", card->cid.name,
           (unsigned long long)card->csd.capacity * card->csd.sector_size
               / (1024 * 1024));
    return true;
}

#endif

#if PLOTTER_SD
static bool
is_dir(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
#endif

static std::string
read_text(const std::string &path)
{
    std::string s;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return s;
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        s.append(buf, n);
    fclose(f);
    return s;
}


// FIRMWARE_RESTART, host side (after Printer::reset_mcu): start again
static void
restart_board()
{
    printf("restart\n");
    vTaskDelay(pdMS_TO_TICKS(300)); // let the MCU reset command go out
    esp_restart();
}

#if PLOTTER_SCREEN
/****************************************************************
 * Touch screen
 ****************************************************************/

static void
start_screen(kl::Printer &p, const std::string &gcode_dir)
{
#if CONFIG_PLOTTER_NETWORK
    // System -> WiFi on the screen
    struct Backend : kl::PrinterUiBackend {
        using kl::PrinterUiBackend::PrinterUiBackend;
        bool wifi_supported() override { return true; }
        std::string wifi_status() override { return wifi::status(); }
        void wifi_scan_start() override { wifi::scan_start(); }
        bool wifi_scan_results(std::vector<kl::WifiNetwork> *out) override {
            return wifi::scan_results(out);
        }
        void wifi_connect(const std::string &ssid,
                          const std::string &password) override {
            wifi::join(ssid, password);
        }
        bool wifi_ap_mode() override { return wifi::ap_mode(); }
        void wifi_set_ap_mode(bool on) override { wifi::set_ap_mode(on); }
    };
    static Backend backend(p);
#else
    static kl::PrinterUiBackend backend(p);
#endif
    backend.restart_board = restart_board;
    static std::string files_line = "G-code files: " + gcode_dir;
    backend.info = [] {
        std::vector<std::string> l;
        char buf[96];
#if CONFIG_PLOTTER_CYD
        l.push_back("Board: CYD (ESP32), screen + microSD");
#else
        l.push_back("Board: " CONFIG_IDF_TARGET);
#endif
#if CONFIG_PLOTTER_NETWORK
        std::string host = wifi::host();
        snprintf(buf, sizeof(buf), "Mainsail: http://%s/",
                 host.empty() ? "(no WiFi)" : host.c_str());
        l.push_back(buf);
        if (wifi::ap_mode() || wifi::ip().empty()) {
            // Access point name and password (for the phone)
            std::string st = wifi::status();
            size_t nl = st.find('\n');
            l.push_back(wifi::ap_mode() ? st.substr(0, nl)
                        : nl == std::string::npos ? st : st.substr(nl + 1));
        }
#endif
        l.push_back(files_line);
        snprintf(buf, sizeof(buf), "RAM free %u KB (lowest %u KB)",
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)
                            / 1024));
        l.push_back(buf);
        long t = (long)(esp_timer_get_time() / 1000000);
        snprintf(buf, sizeof(buf), "Up %ld:%02ld:%02ld", t / 3600, t / 60 % 60,
                 t % 60);
        l.push_back(buf);
        return l;
    };
    board::start_ui([] {
        static kl::PlotterUi ui(backend);
        ui.create();
    });
}
#endif

/****************************************************************
 * Console sharing with the MCU link (simulated MCU)
 ****************************************************************/

// While muted, console text goes to a RAM ring buffer instead (the last
// LOG_RING_SIZE bytes), served as /server/files/klippy.log on CoreS3
#if CONFIG_SPIRAM
static const size_t LOG_RING_SIZE = 128 * 1024;
#else
static const size_t LOG_RING_SIZE = 4 * 1024;
#endif

struct LogRing {
    char *buf = nullptr;
    size_t pos = 0;     // next write offset
    bool wrapped = false;
    std::mutex lock;

    static int write_fn(void *cookie, const char *data, int len) {
        auto *r = (LogRing *)cookie;
        std::lock_guard<std::mutex> g(r->lock);
        for (int i = 0; i < len; i++) {
            r->buf[r->pos++] = data[i];
            if (r->pos == LOG_RING_SIZE) {
                r->pos = 0;
                r->wrapped = true;
            }
        }
        return len;
    }
    std::string text() {
        std::lock_guard<std::mutex> g(lock);
        if (!buf)
            return "";
        std::string out;
        if (wrapped)
            out.assign(buf + pos, LOG_RING_SIZE - pos);
        out.append(buf, pos);
        return out;
    }
};
static LogRing log_ring;

struct ConsoleMute {
    FILE *log_ = nullptr;
    explicit ConsoleMute(bool active) {
        if (!active)
            return;
        fflush(stdout);
        esp_log_level_set("*", ESP_LOG_WARN);
        log_ring.buf = (char *)heap_caps_malloc(LOG_RING_SIZE, MALLOC_CAP_SPIRAM)
                           ?: (char *)malloc(LOG_RING_SIZE);
        if (log_ring.buf)
            log_ = funopen(&log_ring, nullptr, LogRing::write_fn, nullptr,
                           nullptr);
        if (!log_) {
            esp_log_level_set("*", ESP_LOG_NONE);
            log_ = fopen("/dev/null", "w");
        }
        setvbuf(log_, nullptr, _IOLBF, 256);
        stdout = stderr = log_;
        _GLOBAL_REENT->_stdout = _GLOBAL_REENT->_stderr = log_;
    }
};

/****************************************************************
 * Klippy task
 ****************************************************************/

static void
klippy_task(void *)
{
    bool have_storage = mount_storage();
    std::string gcode_dir = "/data/gcodes";
    std::string cfg = "";
    std::string config_dir = "/data/config";
#if CONFIG_PLOTTER_CYD
    printf("\nESP32Klipper plotter (CYD: screen + microSD)\n");
#endif
#if PLOTTER_SD
    if (mount_sd()) {
        // G-code files in /gcodes or the card's root; printer.cfg in the
        // root replaces the internal one (and Mainsail edits that one)
        gcode_dir = is_dir("/sd/gcodes") ? "/sd/gcodes" : SD;
        cfg = read_text("/sd/printer.cfg");
        if (!cfg.empty()) {
            printf("printer.cfg: from the microSD card\n");
            config_dir = SD;
        }
    }
    printf("G-code files: %s\n", gcode_dir.c_str());
#endif
#if CONFIG_PLOTTER_NETWORK
    wifi::start(20000);
    printf("\nESP32Klipper plotter: %s\n", wifi::status().c_str());
#endif

    std::string cfg_path = "/data/config/printer.cfg";
    if (cfg.empty() && have_storage)
        cfg = read_text(cfg_path);
    if (cfg.empty()) {
        cfg = sample_cfg;
        if (FILE *f = have_storage ? fopen(cfg_path.c_str(), "wb") : nullptr) {
            fwrite(cfg.data(), 1, cfg.size(), f);
            fclose(f);
            printf("printer.cfg: wrote the sample configuration\n");
        }
    }
    kl::PrinterOptions opt;
    // Moonraker serves the configuration (configfile object)
#if CONFIG_PLOTTER_NETWORK
    opt.keep_config = true;
#endif
#if CONFIG_SPIRAM
    // With PSRAM, Klipper's own buffering.  The CYD values (steps sent only
    // ~50 ms ahead) ran dry on the CoreS3 with WiFi traffic: "Timer too
    // close".  Live position (motion_report) reads the trapq history, so
    // keep it longer than the step generation lead.
    opt.buffer_time_high = 1.0;
    opt.queuing.bgflush_sg_low = 0.45;
    opt.queuing.bgflush_sg_high = 0.70;
    opt.queuing.history_expire = 2.0;
#endif
    static kl::Printer p(opt);
    std::string err;
#if CONFIG_SPIRAM
    // The configuration (macros, sections) is many small blocks, which
    // would all go to internal RAM (< SPIRAM_MALLOC_ALWAYSINTERNAL): a large
    // printer.cfg took ~70 KB of it and the MCU link thread could not start.
    // PSRAM first while it loads.
    heap_caps_malloc_extmem_enable(0);
#endif
    bool cfg_ok = p.load_config(cfg, &err);
#if CONFIG_SPIRAM
    heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
#endif
    cfg.clear();
    cfg.shrink_to_fit();
    if (!cfg_ok)
        printf("printer.cfg error: %s\n", err.c_str());
    for (const auto &w : p.warnings())
        printf("warning: %s\n", w.c_str());

    std::string port = CONFIG_PLOTTER_MCU_PORT;
    if (port.empty())
        port = p.mcu_serial();
    bool shares_console = port == "usbjtag" || port.compare(0, 7, "uart:0:") == 0;

    auto gcodes = std::make_shared<kl::PosixFileSystem>(gcode_dir);
    p.set_filesystem(gcodes);
#if CONFIG_PLOTTER_NETWORK
    auto config = std::make_shared<kl::PosixFileSystem>(config_dir);
    kl::Moonraker::Options mo;
    mo.hostname = CONFIG_PLOTTER_HOSTNAME;
    mo.cpu = CONFIG_IDF_TARGET;
    mo.memory_info = [](uint64_t *total, uint64_t *avail) {
        *total = heap_caps_get_total_size(MALLOC_CAP_8BIT);
        *avail = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    };
    mo.restart = restart_board;
    static kl::Moonraker mr(p, gcodes, config, mo);
    kl::EspServerConfig sc;
    sc.www_dir = "/data/www";
    sc.hook = [](httpd_req_t *req, esp_err_t *ret) {
        if (wifi::http_hook(req, ret))
            return true;
        if (req->method != HTTP_GET
            || (strcmp(req->uri, "/server/files/klippy.log") != 0
                && strcmp(req->uri, "/server/files/logs/klippy.log") != 0))
            return false;
        std::string t = log_ring.text();
        httpd_resp_set_type(req, "text/plain");
        *ret = httpd_resp_send(req, t.data(), t.size());
        return true;
    };
    if (!kl::esp_server_start(mr, sc, &err))
        printf("HTTP server: %s\n", err.c_str());
#endif
#if PLOTTER_SCREEN
    start_screen(p, gcode_dir);
    vTaskDelay(pdMS_TO_TICKS(300)); // first screen drawn before the link
#endif
    printf("heap free %u (internal %u)\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    if (cfg_ok) {
        if (shares_console) {
            printf("MCU on the console port (%s): console muted\n@@LINK_START@@\n",
                   port.c_str());
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        static ConsoleMute mute(shares_console);
        if (p.connect(port, p.mcu_baud(), &err)) {
#if CONFIG_PLOTTER_SIM_HOMING
            kl::install_sim_homing(p);
#endif
        } else {
            printf("MCU connect failed: %s\n", err.c_str());
        }
    }
    bool autostart = strlen(CONFIG_PLOTTER_AUTOSTART) > 0;
    for (;;) {
        if (autostart && p.is_ready()) {
            autostart = false;
            p.submit_script(std::string("G28 X Y\nSDCARD_PRINT_FILE FILENAME=")
                            + CONFIG_PLOTTER_AUTOSTART);
        }
        p.poll();
        if (!p.is_ready())
            vTaskDelay(pdMS_TO_TICKS(10));
    }
}

extern "C" void
app_main(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    xTaskCreatePinnedToCore(klippy_task, "klippy", KLIPPY_STACK, nullptr, 10,
                            nullptr, 1);
}
