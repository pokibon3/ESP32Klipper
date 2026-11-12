# C++ port of the Klipper host (klippy) parts needed on the ESP32.
# toolhead/lookahead/motion_queuing.

set(KLIPPY_LITE_SRCS
    ${CMAKE_CURRENT_LIST_DIR}/src/motion.cpp
)

set(KLIPPY_LITE_COPTS -O2 -ffp-contract=off)

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${KLIPPY_LITE_SRCS}
        INCLUDE_DIRS include
        REQUIRES chelper)
    target_compile_options(${COMPONENT_LIB} PRIVATE ${KLIPPY_LITE_COPTS})
else()
    add_library(klippy_lite STATIC ${KLIPPY_LITE_SRCS})
    target_include_directories(klippy_lite PUBLIC include)
    target_compile_features(klippy_lite PUBLIC cxx_std_17)
    target_compile_options(klippy_lite PRIVATE ${KLIPPY_LITE_COPTS})
    target_link_libraries(klippy_lite PUBLIC chelper)
endif()
