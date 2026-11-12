// CYD display (ILI9341 or ST7789) and touch (XPT2046) drivers for LVGL 8, and the
// LVGL task (see cyd_display.hpp).
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "cyd_board/cyd_display.hpp"

#include <atomic>
#include <initializer_list>
#include <utility>
#include <cstdio>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "sdkconfig.h"

extern "C" uint32_t
lv_port_tick_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

namespace cyd {

namespace {

constexpr int W = 320, H = 240;

// Display (SPI2)
constexpr gpio_num_t LCD_SCLK = GPIO_NUM_14, LCD_MOSI = GPIO_NUM_13,
                     LCD_MISO = GPIO_NUM_12, LCD_CS = GPIO_NUM_15,
                     LCD_DC = GPIO_NUM_2, LCD_BL = GPIO_NUM_21;
// Touch (bit-banged)
constexpr gpio_num_t TP_CLK = GPIO_NUM_25, TP_MOSI = GPIO_NUM_32,
                     TP_MISO = GPIO_NUM_39, TP_CS = GPIO_NUM_33,
                     TP_IRQ = GPIO_NUM_36;

esp_lcd_panel_io_handle_t lcd_io;
lv_disp_drv_t disp_drv;
lv_disp_draw_buf_t draw_buf;
lv_indev_drv_t indev_drv;
std::function<void()> build_fn;
UiTaskConfig task_cfg;
std::atomic<uint64_t> busy_us{0}, wait_us{0};
std::atomic<uint32_t> flushes{0};
TaskHandle_t ui_task_handle;
UiMemory memory;
lv_obj_t *touch_dot;

bool
on_color_done(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *,
              void *ctx)
{
    lv_disp_flush_ready((lv_disp_drv_t *)ctx);
    // Wake the UI task (it sleeps in wait_cb instead of spinning)
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(ui_task_handle, &woken);
    return woken == pdTRUE;
}

void
lcd_cmd(int cmd, std::initializer_list<uint8_t> data)
{
    esp_lcd_panel_io_tx_param(lcd_io, cmd, data.size() ? data.begin() : nullptr,
                              data.size());
}

bool
lcd_init()
{
    spi_bus_config_t bus = {};
    bus.sclk_io_num = LCD_SCLK;
    bus.mosi_io_num = LCD_MOSI;
    bus.miso_io_num = LCD_MISO;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = W * task_cfg.lines * 2 + 8;
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK)
        return false;
    esp_lcd_panel_io_spi_config_t io = {};
    io.cs_gpio_num = LCD_CS;
    io.dc_gpio_num = LCD_DC;
    io.spi_mode = 0;
    io.pclk_hz = CONFIG_CYD_LCD_PCLK_MHZ * 1000 * 1000;
    io.trans_queue_depth = 4;
    io.on_color_trans_done = on_color_done;
    io.user_ctx = &disp_drv;
    io.lcd_cmd_bits = 8;
    io.lcd_param_bits = 8;
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io,
                                 &lcd_io) != ESP_OK)
        return false;

#if CONFIG_CYD_LCD_ST7789
    // ST7789: the power-on defaults suit the panel; set the pixel format
    // and orientation (MADCTL bits as on the ILI9341)
    lcd_cmd(0x01, {}); // software reset
    vTaskDelay(pdMS_TO_TICKS(120));
    lcd_cmd(0x36, {(uint8_t)CONFIG_CYD_LCD_MADCTL});
    lcd_cmd(0x3A, {0x55}); // 16 bits per pixel
    lcd_cmd(0x13, {});     // normal display mode
#else
    // ILI9341 power-on sequence (as in the common ILI9341 drivers)
    lcd_cmd(0x01, {}); // software reset
    vTaskDelay(pdMS_TO_TICKS(120));
    lcd_cmd(0xCF, {0x00, 0xC1, 0x30});
    lcd_cmd(0xED, {0x64, 0x03, 0x12, 0x81});
    lcd_cmd(0xE8, {0x85, 0x00, 0x78});
    lcd_cmd(0xCB, {0x39, 0x2C, 0x00, 0x34, 0x02});
    lcd_cmd(0xF7, {0x20});
    lcd_cmd(0xEA, {0x00, 0x00});
    lcd_cmd(0xC0, {0x23});       // power control 1
    lcd_cmd(0xC1, {0x10});       // power control 2
    lcd_cmd(0xC5, {0x3E, 0x28}); // VCOM control 1
    lcd_cmd(0xC7, {0x86});       // VCOM control 2
    lcd_cmd(0x36, {(uint8_t)CONFIG_CYD_LCD_MADCTL});
    lcd_cmd(0x3A, {0x55});       // 16 bits per pixel
    lcd_cmd(0xB1, {0x00, 0x18}); // frame rate 79 Hz
    lcd_cmd(0xB6, {0x08, 0x82, 0x27});
    lcd_cmd(0xF2, {0x00});       // 3-gamma off
    lcd_cmd(0x26, {0x01});       // gamma curve 1
    lcd_cmd(0xE0, {0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1, 0x37, 0x07,
                   0x10, 0x03, 0x0E, 0x09, 0x00});
    lcd_cmd(0xE1, {0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1, 0x48, 0x08,
                   0x0F, 0x0C, 0x31, 0x36, 0x0F});
#endif
#if CONFIG_CYD_LCD_INVERT
    lcd_cmd(0x21, {});
#else
    lcd_cmd(0x20, {});
#endif
    lcd_cmd(0x11, {}); // sleep out
    vTaskDelay(pdMS_TO_TICKS(120));
    lcd_cmd(0x29, {}); // display on
    return true;
}

void
flush_cb(lv_disp_drv_t *drv, const lv_area_t *a, lv_color_t *px)
{
    lcd_cmd(0x2A, {(uint8_t)(a->x1 >> 8), (uint8_t)a->x1, (uint8_t)(a->x2 >> 8),
                   (uint8_t)a->x2});
    lcd_cmd(0x2B, {(uint8_t)(a->y1 >> 8), (uint8_t)a->y1, (uint8_t)(a->y2 >> 8),
                   (uint8_t)a->y2});
    size_t len = (size_t)(a->x2 - a->x1 + 1) * (a->y2 - a->y1 + 1) * 2;
    esp_lcd_panel_io_tx_color(lcd_io, 0x2C, px, len);
    flushes++;
}

void
wait_cb(lv_disp_drv_t *)
{
    uint64_t t0 = esp_timer_get_time();
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5));
    wait_us += esp_timer_get_time() - t0;
}

/****************************************************************
 * XPT2046 touch
 ****************************************************************/

uint16_t
tp_read(uint8_t cmd)
{
    gpio_set_level(TP_CS, 0);
    for (int i = 7; i >= 0; i--) {
        gpio_set_level(TP_MOSI, (cmd >> i) & 1);
        esp_rom_delay_us(1);
        gpio_set_level(TP_CLK, 1);
        esp_rom_delay_us(1);
        gpio_set_level(TP_CLK, 0);
    }
    gpio_set_level(TP_MOSI, 0);
    uint16_t v = 0;
    for (int i = 0; i < 16; i++) {
        esp_rom_delay_us(1);
        gpio_set_level(TP_CLK, 1);
        esp_rom_delay_us(1);
        v = (v << 1) | gpio_get_level(TP_MISO);
        gpio_set_level(TP_CLK, 0);
    }
    gpio_set_level(TP_CS, 1);
    return (v >> 3) & 0xfff;
}

void
tp_init()
{
    gpio_config_t out = {};
    out.pin_bit_mask = (1ULL << TP_CLK) | (1ULL << TP_MOSI) | (1ULL << TP_CS);
    out.mode = GPIO_MODE_OUTPUT;
    gpio_config(&out);
    gpio_config_t in = {};
    in.pin_bit_mask = (1ULL << TP_MISO) | (1ULL << TP_IRQ);
    in.mode = GPIO_MODE_INPUT;
    gpio_config(&in);
    gpio_set_level(TP_CS, 1);
    gpio_set_level(TP_CLK, 0);
}

int
median3(int a, int b, int c)
{
    if (a > b)
        std::swap(a, b);
    if (b > c)
        std::swap(b, c);
    return a > b ? a : b;
}

int
map_axis(int v, int lo, int hi, int size)
{
    int r = (v - lo) * (size - 1) / (hi - lo);
    return r < 0 ? 0 : r >= size ? size - 1 : r;
}

void
touch_cb(lv_indev_drv_t *, lv_indev_data_t *data)
{
    static int last_x, last_y;
    data->state = LV_INDEV_STATE_REL;
    if (gpio_get_level(TP_IRQ) == 0) {
        int z = tp_read(0xB0) + 4095 - tp_read(0xC0);
        if (z > 400) {
            tp_read(0xD0); // first sample after a mux change is noisy
            int x0 = tp_read(0xD0), y0 = tp_read(0x90);
            int x1 = tp_read(0xD0), y1 = tp_read(0x90);
            int x2 = tp_read(0xD0), y2 = tp_read(0x90);
            int rx = median3(x0, x1, x2), ry = median3(y0, y1, y2);
#if CONFIG_CYD_TOUCH_SWAP_XY
            std::swap(rx, ry);
#endif
            last_x = map_axis(rx, CONFIG_CYD_TOUCH_X_MIN, CONFIG_CYD_TOUCH_X_MAX,
                              W);
            last_y = map_axis(ry, CONFIG_CYD_TOUCH_Y_MIN, CONFIG_CYD_TOUCH_Y_MAX,
                              H);
            data->state = LV_INDEV_STATE_PR;
            if (touch_dot) {
                lv_obj_set_pos(touch_dot, last_x - 4, last_y - 4);
                lv_obj_clear_flag(touch_dot, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    data->point.x = last_x;
    data->point.y = last_y;
}

void
ui_task(void *)
{
    ui_task_handle = xTaskGetCurrentTaskHandle();
    size_t heap0 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    lv_init();
    if (!lcd_init()) {
        printf("cyd: display init failed\n");
        vTaskDelete(nullptr);
        return;
    }
    size_t buf_px = W * task_cfg.lines;
    auto *buf = (lv_color_t *)heap_caps_malloc(buf_px * sizeof(lv_color_t),
                                               MALLOC_CAP_DMA);
    lv_disp_draw_buf_init(&draw_buf, buf, nullptr, buf_px);
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = W;
    disp_drv.ver_res = H;
    disp_drv.flush_cb = flush_cb;
    disp_drv.wait_cb = wait_cb;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    tp_init();
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touch_cb;
    lv_indev_drv_register(&indev_drv);

    size_t heap1 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    if (build_fn)
        build_fn();
#if CONFIG_CYD_TOUCH_SHOW
    touch_dot = lv_obj_create(lv_layer_sys());
    lv_obj_remove_style_all(touch_dot);
    lv_obj_set_size(touch_dot, 9, 9);
    lv_obj_set_style_radius(touch_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(touch_dot, lv_color_hex(0xff00ff), 0);
    lv_obj_set_style_bg_opa(touch_dot, LV_OPA_COVER, 0);
    lv_obj_add_flag(touch_dot, LV_OBJ_FLAG_HIDDEN);
#endif
    lv_refr_now(nullptr);
    memory.drivers = heap0 - heap1;
    memory.screen = heap1 - heap_caps_get_free_size(MALLOC_CAP_8BIT);
    // Backlight on after the first frame
    gpio_set_direction(LCD_BL, GPIO_MODE_OUTPUT);
    gpio_set_level(LCD_BL, 1);
    for (;;) {
        uint64_t t0 = esp_timer_get_time();
        uint32_t wait_ms = lv_timer_handler();
        busy_us += esp_timer_get_time() - t0;
        memory.stack_unused = uxTaskGetStackHighWaterMark(nullptr);
        if (wait_ms < 5)
            wait_ms = 5;
        if (wait_ms > 30)
            wait_ms = 30;
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }
}

} // namespace

bool
start_ui(std::function<void()> build, const UiTaskConfig &cfg)
{
    build_fn = std::move(build);
    task_cfg = cfg;
    return xTaskCreatePinnedToCore(ui_task, "ui", cfg.stack, nullptr,
                                   cfg.priority, nullptr, cfg.core)
           == pdPASS;
}

uint64_t
ui_busy_us()
{
    return busy_us - wait_us;
}

UiMemory
ui_memory()
{
    return memory;
}

uint32_t
ui_flushes()
{
    return flushes;
}

} // namespace cyd
