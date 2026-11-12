# Host (macOS/Linux) build of the M1 benchmark.  Runs the same code as the
# ESP32 build so results (output hash, step counts) can be compared.
#
#   cmake -S host -B build/host -G Ninja && ninja -C build/host
#   ./build/host/bench_host bench/data/card_opt.gcode
cmake_minimum_required(VERSION 3.16)
project(esp32klipper_bench_host C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 17)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../components/chelper chelper)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../components/klippy_lite klippy_lite)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../components/m1_bench m1_bench)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../components/mcu_link mcu_link)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../components/klippy klippy)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../components/link_tests link_tests)

add_executable(bench_host main_host.cpp)
target_compile_options(bench_host PRIVATE -O2 -ffp-contract=off)
target_link_libraries(bench_host PRIVATE m1_bench)

# M2: one axis through the real MCU protocol (simulated MCU or SKR Pico)
add_executable(m2_axis_test m2_axis_test.cpp)
target_compile_options(m2_axis_test PRIVATE -O2 -ffp-contract=off)
target_link_libraries(m2_axis_test PRIVATE link_tests)

# M3: CoreXY from printer.cfg, squares at 300 mm/s
add_executable(m3_square_test m3_square_test.cpp)
target_compile_options(m3_square_test PRIVATE -O2 -ffp-contract=off)
target_link_libraries(m3_square_test PRIVATE link_tests)

# M4: homing (sensorless X/Y, switch Z) on the simulated MCU
add_executable(m4_homing_test m4_homing_test.cpp)
target_compile_options(m4_homing_test PRIVATE -O2 -ffp-contract=off)
target_link_libraries(m4_homing_test PRIVATE link_tests)

# Jinja2 subset unit test
add_executable(jinja_test jinja_test.cpp)
target_link_libraries(jinja_test PRIVATE klippy)

# M4: printer.cfg macros + drawing file on the simulated MCU
add_executable(m4_macro_test m4_macro_test.cpp)
target_compile_options(m4_macro_test PRIVATE -O2 -ffp-contract=off)
target_link_libraries(m4_macro_test PRIVATE link_tests)

add_executable(gcode_parse_test gcode_parse_test.cpp)
target_link_libraries(gcode_parse_test PRIVATE klippy)

# M4: SD card print with pause/resume/cancel
add_executable(m4_sdcard_test m4_sdcard_test.cpp)
target_compile_options(m4_sdcard_test PRIVATE -O2 -ffp-contract=off)
target_link_libraries(m4_sdcard_test PRIVATE link_tests)

# Local UI: renders the screens to PNG files (no display needed)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../components/lvgl lvgl)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../components/ui ui)
add_executable(ui_screens ui_screens.cpp)
target_link_libraries(ui_screens PRIVATE ui)

# M5: Moonraker-compatible API + Mainsail on the development machine
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../components/moonraker moonraker)
add_executable(moonraker_host moonraker_host.cpp)
target_compile_options(moonraker_host PRIVATE -O2 -ffp-contract=off)
target_link_libraries(moonraker_host PRIVATE moonraker link_tests)
