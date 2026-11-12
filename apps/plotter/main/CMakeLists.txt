idf_component_register(
    SRCS main.cpp
    REQUIRES klippy klippy_lite mcu_link chelper moonraker link_tests
             esp_timer heap log nvs_flash esp_wifi esp_netif esp_event fatfs
             vfs pthread ui cyd_board cores3_board lvgl sdmmc esp_driver_sdspi
    EMBED_TXTFILES ${CMAKE_CURRENT_LIST_DIR}/../../../config/skr_pico_corexy_penplotter.cfg)

# Storage image (tools/make_plotter_fs.py); flashed with the firmware only
# when PLOTTER_FLASH_FS=1 (it replaces the board's files)
# (CYD: build/plotter_fs_cyd, made with --no-www)
if(CONFIG_IDF_TARGET_ESP32)
    set(PLOTTER_FS ${CMAKE_CURRENT_LIST_DIR}/../../../build/plotter_fs_cyd)
else()
    set(PLOTTER_FS ${CMAKE_CURRENT_LIST_DIR}/../../../build/plotter_fs)
endif()
if(EXISTS ${PLOTTER_FS})
    if("$ENV{PLOTTER_FLASH_FS}" STREQUAL "1")
        fatfs_create_spiflash_image(storage ${PLOTTER_FS} FLASH_IN_PROJECT
                                    PRESERVE_TIME)
    else()
        fatfs_create_spiflash_image(storage ${PLOTTER_FS} PRESERVE_TIME)
    endif()
endif()
