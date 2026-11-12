# Klipper chelper (host-side C motion code), built unmodified from
# third_party/klipper plus a small port layer.  Works both as an ESP-IDF
# component and as a plain CMake library for the host (macOS/Linux) build.
#
# The Klipper sources are GPLv3; see third_party/klipper/COPYING.

set(KLIPPER_CHELPER_DIR
    ${CMAKE_CURRENT_LIST_DIR}/../../third_party/klipper/klippy/chelper)

set(CHELPER_SRCS
    ${KLIPPER_CHELPER_DIR}/trapq.c
    ${KLIPPER_CHELPER_DIR}/itersolve.c
    ${KLIPPER_CHELPER_DIR}/stepcompress.c
    ${KLIPPER_CHELPER_DIR}/steppersync.c
    ${KLIPPER_CHELPER_DIR}/msgblock.c
    ${KLIPPER_CHELPER_DIR}/kin_corexy.c
    ${KLIPPER_CHELPER_DIR}/kin_cartesian.c
    ${KLIPPER_CHELPER_DIR}/kin_shaper.c
    ${CMAKE_CURRENT_LIST_DIR}/port/pyhelper_port.c
)

# The Klipper sources are compiled as-is; silence warnings we do not own.
set(CHELPER_COPTS -O2 -ffp-contract=off -Wno-unused-variable
    -Wno-unused-function -Wno-attributes -Wno-format)

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${CHELPER_SRCS}
        INCLUDE_DIRS port
        PRIV_INCLUDE_DIRS ${KLIPPER_CHELPER_DIR}
        PRIV_REQUIRES pthread)
    target_compile_options(${COMPONENT_LIB} PRIVATE ${CHELPER_COPTS})
else()
    add_library(chelper STATIC ${CHELPER_SRCS})
    target_include_directories(chelper PUBLIC port
                               PRIVATE ${KLIPPER_CHELPER_DIR})
    target_compile_options(chelper PRIVATE ${CHELPER_COPTS}
                           -Wno-unknown-attributes -Wno-gnu-statement-expression)
    find_package(Threads REQUIRED)
    target_link_libraries(chelper PUBLIC Threads::Threads m)
endif()
