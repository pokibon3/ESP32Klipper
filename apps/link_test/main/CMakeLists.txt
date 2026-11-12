set(PRIVATE_DATA ${CMAKE_CURRENT_LIST_DIR}/../../../bench/data)
set(EMBED ${CMAKE_CURRENT_LIST_DIR}/../../../config/skr_pico_corexy_penplotter.cfg)
# M4 macro test data (not published, see README): embedded when present
if(EXISTS ${PRIVATE_DATA}/fraxinus0pen_printer.cfg
   AND EXISTS ${PRIVATE_DATA}/card_opt.gcode)
    list(APPEND EMBED ${PRIVATE_DATA}/fraxinus0pen_printer.cfg
                      ${PRIVATE_DATA}/card_opt.gcode)
    set(HAVE_PRIVATE_DATA 1)
endif()
idf_component_register(
    SRCS main.cpp
    REQUIRES link_tests klippy mcu_link klippy_lite chelper esp_timer pthread
             heap esp_driver_uart log ui cyd_board lvgl
    EMBED_TXTFILES ${EMBED})
if(HAVE_PRIVATE_DATA)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE HAVE_PRIVATE_DATA=1)
endif()
