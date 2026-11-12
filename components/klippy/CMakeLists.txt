# Host side of Klipper: printer.cfg, G-code, kinematics, printer objects.

set(KLIPPY_SRCS
    ${CMAKE_CURRENT_LIST_DIR}/src/configfile.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/gcode.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/printer.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/tmc2209.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/mcu_endstop.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/homing.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/jinja.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/gcode_macro.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/files.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/virtual_sdcard.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/status.cpp)

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${KLIPPY_SRCS}
        INCLUDE_DIRS include
        REQUIRES klippy_lite mcu_link chelper)
    target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -ffp-contract=off)
else()
    add_library(klippy STATIC ${KLIPPY_SRCS})
    target_include_directories(klippy PUBLIC include)
    target_compile_options(klippy PRIVATE -O2 -ffp-contract=off)
    target_link_libraries(klippy PUBLIC klippy_lite mcu_link)
endif()
