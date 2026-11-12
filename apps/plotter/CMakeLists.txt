# ESP32Klipper plotter firmware: Klipper host + Moonraker-compatible API
# (Mainsail) on the ESP32.
#
#   CoreS3: idf.py -B ../../build/plotter_s3 set-target esp32s3 build
#
# Storage: a FAT partition "storage" mounted at /data with www/ (Mainsail),
# config/ (printer.cfg) and gcodes/.  `idf.py storage-flash` writes the
# image built from build/plotter_fs (tools/make_plotter_fs.py).
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../../components)
set(SDKCONFIG ${CMAKE_BINARY_DIR}/sdkconfig)
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32klipper_plotter)
