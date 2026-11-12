// CoreS3 display (ILI9342C) and touch (FT6336U) drivers for LVGL 8, and
// the LVGL task (see cores3_display.hpp).
//
// The power and reset setup follows what M5Unified / M5GFX do on this
// board: AXP2101 LDOs on (DLDO1 = backlight), AW9523 port directions and
// outputs (BOOST_EN, touch and LCD out of reset).
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "cores3_board/cores3_display.hpp"

#include <atomic>
#include <cstdio>
#include <initializer_list>
#include <utility>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
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

namespace cores3 {

namespace {

constexpr int W = 320, H = 240;

constexpr gpio_num_t LCD_SCLK = GPIO_NUM_36, LCD_MOSI = GPIO_NUM_37,
                     LCD_CS = GPIO_NUM_3, LCD_DC = GPIO_NUM_35;
constexpr gpio_num_t I2C_SDA = GPIO_NUM_12, I2C_SCL = GPIO_NUM_11;
constexpr uint8_t AXP2101 = 0x34, AW9523 = 0x58, FT6336 = 0x38;

i2c_master_bus_handle_t i2c_bus;
i2c_master_dev_handle_t axp, aw, tp;
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

/****************************************************************
 * I2C: power and IO expander
 ****************************************************************/

i2c_master_dev_handle_t
i2c_dev(uint8_t addr)
{
    i2c_device_config_t dc = {};
    dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dc.device_address = addr;
    dc.scl_speed_hz = 400000;
    i2c_master_dev_handle_t h = nullptr;
    i2c_master_bus_add_device(i2c_bus, &dc, &h);
    return h;
}

bool
wr(i2c_master_dev_handle_t d, uint8_t reg, uint8_t val)
{
    uint8_t b[2] = {reg, val};
    return i2c_master_transmit(d, b, 2, 50) == ESP_OK;
}

bool
rd(i2c_master_dev_handle_t d, uint8_t reg, uint8_t *out, size_t n)
{
    return i2c_master_transmit_receive(d, &reg, 1, out, n, 50) == ESP_OK;
}

bool
power_init()
{
    i2c_master_bus_config_t bc = {};
    bc.i2c_port = I2C_NUM_1;
    bc.sda_io_num = I2C_SDA;
    bc.scl_io_num = I2C_SCL;
    bc.clk_source = I2C_CLK_SRC_DEFAULT;
    bc.glitch_ignore_cnt = 7;
    bc.flags.enable_internal_pullup = true;
    if (i2c_new_master_bus(&bc, &i2c_bus) != ESP_OK)
        return false;
    axp = i2c_dev(AXP2101);
    aw = i2c_dev(AW9523);
    tp = i2c_dev(FT6336);
    bool ok = true;
    // AXP2101: ALDO1 1.8 V, ALDO2-4 3.3 V, DLDO1 (backlight) off until the
    // first frame is drawn
    ok &= wr(axp, 0x92, 13);
    ok &= wr(axp, 0x93, 28);
    ok &= wr(axp, 0x94, 28);
    ok &= wr(axp, 0x95, 28);
    ok &= wr(axp, 0x99, 28); // DLDO1 3.3 V (full brightness)
    ok &= wr(axp, 0x90, 0x3F);
    // AW9523: P0 = touch reset, bus out enable, speaker amp reset;
    // P1 = camera reset, LCD reset, BOOST_EN
    ok &= wr(aw, 0x02, 0b00000111);
    ok &= wr(aw, 0x03, 0b10001111);
    ok &= wr(aw, 0x04, 0b00011000); // P0 directions (1 = input)
    ok &= wr(aw, 0x05, 0b00001100); // P1 directions
    ok &= wr(aw, 0x11, 0b00010000); // P0 push-pull
    ok &= wr(aw, 0x12, 0xFF);       // GPIO mode (no LED driver)
    ok &= wr(aw, 0x13, 0xFF);
    // LCD hardware reset (AW9523 P1_1)
    ok &= wr(aw, 0x03, 0b10001101);
    vTaskDelay(pdMS_TO_TICKS(20));
    ok &= wr(aw, 0x03, 0b10001111);
    vTaskDelay(pdMS_TO_TICKS(120));
    return ok;
}

void
backlight(bool on)
{
    wr(axp, 0x90, on ? 0xBF : 0x3F);
}

/****************************************************************
 * ILI9342C display
 ****************************************************************/

bool
on_color_done(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *,
              void *ctx)
{
    lv_disp_flush_ready((lv_disp_drv_t *)ctx);
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
    bus.miso_io_num = -1; // GPIO35 is the DC line (shared MISO pin)
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = W * task_cfg.lines * 2 + 8;
    if (spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK)
        return false;
    esp_lcd_panel_io_spi_config_t io = {};
    io.cs_gpio_num = LCD_CS;
    io.dc_gpio_num = LCD_DC;
    io.spi_mode = 0;
    io.pclk_hz = CONFIG_CORES3_LCD_PCLK_MHZ * 1000 * 1000;
    io.trans_queue_depth = 4;
    io.on_color_trans_done = on_color_done;
    io.user_ctx = &disp_drv;
    io.lcd_cmd_bits = 8;
    io.lcd_param_bits = 8;
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io,
                                 &lcd_io) != ESP_OK)
        return false;
    // ILI9342C: the power-on defaults are fine for this panel; set the
    // pixel format, orientation and inversion
    lcd_cmd(0x01, {}); // software reset
    vTaskDelay(pdMS_TO_TICKS(120));
    lcd_cmd(0x11, {}); // sleep out
    vTaskDelay(pdMS_TO_TICKS(120));
    lcd_cmd(0x3A, {0x55}); // 16 bits per pixel
    lcd_cmd(0x36, {(uint8_t)CONFIG_CORES3_LCD_MADCTL});
#if CONFIG_CORES3_LCD_INVERT
    lcd_cmd(0x21, {});
#endif
    lcd_cmd(0x29, {}); // display on
    return true;
}

void
flush_cb(lv_disp_drv_t *, const lv_area_t *a, lv_color_t *px)
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
 * FT6336U touch
 ****************************************************************/

void
touch_cb(lv_indev_drv_t *, lv_indev_data_t *data)
{
    static int last_x, last_y;
    data->state = LV_INDEV_STATE_REL;
    uint8_t b[5];
    if (rd(tp, 0x02, b, sizeof(b)) && (b[0] & 0x0F) > 0) {
        int x = ((b[1] & 0x0F) << 8) | b[2];
        int y = ((b[3] & 0x0F) << 8) | b[4];
        // The panel reports y up to ~280 (touch area below the screen)
        if (x < W && y < H) {
            last_x = x;
            last_y = y;
            data->state = LV_INDEV_STATE_PR;
            if (touch_dot) {
                lv_obj_set_pos(touch_dot, x - 4, y - 4);
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
    if (!power_init())
        printf("cores3: power / IO expander setup failed\n");
    lv_init();
    if (!lcd_init()) {
        printf("cores3: display init failed\n");
        vTaskDelete(nullptr);
        return;
    }
    size_t buf_px = W * task_cfg.lines;
    auto *buf = (lv_color_t *)heap_caps_malloc(
        buf_px * sizeof(lv_color_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    lv_disp_draw_buf_init(&draw_buf, buf, nullptr, buf_px);
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = W;
    disp_drv.ver_res = H;
    disp_drv.flush_cb = flush_cb;
    disp_drv.wait_cb = wait_cb;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touch_cb;
    lv_indev_drv_register(&indev_drv);

    size_t heap1 = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    if (build_fn)
        build_fn();
#if CONFIG_CORES3_TOUCH_SHOW
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
    backlight(true); // after the first frame
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

} // namespace cores3
