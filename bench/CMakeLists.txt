# M1 benchmark firmware (ESP-IDF).
#
#   CYD (ESP32):      idf.py -B build/esp32 set-target esp32 build flash monitor
#   CoreS3 (ESP32-S3) idf.py -B build/esp32s3 set-target esp32s3 build flash monitor
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/../components)
# Keep one sdkconfig per build directory so CYD and CoreS3 builds coexist
set(SDKCONFIG ${CMAKE_BINARY_DIR}/sdkconfig)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32klipper_bench)
