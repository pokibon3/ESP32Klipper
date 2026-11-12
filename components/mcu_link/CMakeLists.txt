# Host <-> MCU link: Klipper's serialqueue/pollreactor (unmodified) plus a
# C++ port of msgproto.py, serialhdl.py, clocksync.py and the config part
# of mcu.py.

set(KLIPPER_CHELPER_DIR
    ${CMAKE_CURRENT_LIST_DIR}/../../third_party/klipper/klippy/chelper)

# serialqueue_wrap.c #includes Klipper's serialqueue.c (unmodified)
set(MCU_LINK_C_SRCS
    ${CMAKE_CURRENT_LIST_DIR}/port/serialqueue_wrap.c
    ${KLIPPER_CHELPER_DIR}/pollreactor.c
    ${KLIPPER_CHELPER_DIR}/trdispatch.c)
set(MCU_LINK_CXX_SRCS
    ${CMAKE_CURRENT_LIST_DIR}/src/json_lite.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/msgproto.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/zlib_port.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/clocksync.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/serialhdl.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/mcu.cpp)

set(SQ_COPTS -O2 -Wno-unused-variable -Wno-unused-function -Wno-attributes
    -Wno-format)

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${MCU_LINK_C_SRCS} ${MCU_LINK_CXX_SRCS}
             ${CMAKE_CURRENT_LIST_DIR}/src/mcu_port_esp.cpp
             ${CMAKE_CURRENT_LIST_DIR}/port/sq_port_esp.c
        INCLUDE_DIRS include
        PRIV_INCLUDE_DIRS port ${KLIPPER_CHELPER_DIR}
        REQUIRES chelper
        PRIV_REQUIRES pthread esp_rom vfs esp_driver_uart driver
                      esp_driver_usb_serial_jtag)
    # serialqueue.c/pollreactor.c: route pipe()/read()/write() of the
    # wake-up pipe to an eventfd (see port/sq_port_esp.h)
    set_source_files_properties(${MCU_LINK_C_SRCS} PROPERTIES COMPILE_OPTIONS
        "-include;${CMAKE_CURRENT_LIST_DIR}/port/sq_port_esp.h;${SQ_COPTS}")
else()
    add_library(mcu_link STATIC ${MCU_LINK_C_SRCS} ${MCU_LINK_CXX_SRCS}
                ${CMAKE_CURRENT_LIST_DIR}/src/mcu_port_host.cpp)
    target_include_directories(mcu_link PUBLIC include
                               PRIVATE port ${KLIPPER_CHELPER_DIR})
    set_source_files_properties(${MCU_LINK_C_SRCS} PROPERTIES COMPILE_OPTIONS
        "${SQ_COPTS};-Wno-unknown-attributes;-Wno-gnu-statement-expression")
    target_compile_features(mcu_link PUBLIC cxx_std_17)
    find_package(ZLIB REQUIRED)
    target_link_libraries(mcu_link PUBLIC chelper ZLIB::ZLIB)
endif()
