# MCU link test firmware (M2/M3): runs link_tests on the ESP32 against a
# Klipper MCU.
#
#   CYD:    idf.py -B ../../build/link_esp32 set-target esp32 build
#
# Link port (menuconfig "Link test"): uart:0:1:3 = console UART, for the
# simulated MCU via tools/sim_bridge.py; uart:2:27:22 = CYD -> SKR Pico.
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../../components)
set(SDKCONFIG ${CMAKE_BINARY_DIR}/sdkconfig)
# Only build what this app needs
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32klipper_link_test)
