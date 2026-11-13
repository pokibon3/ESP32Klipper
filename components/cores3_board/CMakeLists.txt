# M5Stack CoreS3 board support: ILI9342C display, FT6336U touch, AXP2101
# power and AW9523 IO expander (I2C), the microSD card and the LVGL task.  ESP32-S3 only.
if(IDF_TARGET STREQUAL "esp32s3")
    set(CORES3_SRCS src/cores3_display.cpp)
else()
    set(CORES3_SRCS)
endif()
idf_component_register(SRCS ${CORES3_SRCS}
                       INCLUDE_DIRS include
                       REQUIRES lvgl esp_lcd esp_driver_spi esp_driver_gpio
                                esp_driver_i2c esp_driver_sdspi fatfs sdmmc
                                esp_timer freertos)
