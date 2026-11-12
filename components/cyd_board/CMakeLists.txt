# CYD (ESP32-2432S028R) board support: ILI9341 display, XPT2046 touch and
# the LVGL task.  ESP32 only (the pins do not exist on other chips).
if(IDF_TARGET STREQUAL "esp32")
    set(CYD_SRCS src/cyd_display.cpp)
else()
    set(CYD_SRCS)
endif()
idf_component_register(SRCS ${CYD_SRCS}
                       INCLUDE_DIRS include
                       REQUIRES lvgl esp_lcd esp_driver_spi esp_driver_gpio
                                esp_timer freertos)
