# Hardware-in-the-loop tests of the MCU link (shared by host and ESP32).

set(LINK_TESTS_SRCS ${CMAKE_CURRENT_LIST_DIR}/src/m2_axis.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/m3_square.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/m4_homing.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/m4_macros.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/m4_sdcard.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/m4_ui_demo.cpp)

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${LINK_TESTS_SRCS}
        INCLUDE_DIRS include
        REQUIRES klippy klippy_lite mcu_link chelper)
    target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -ffp-contract=off)
else()
    add_library(link_tests STATIC ${LINK_TESTS_SRCS})
    target_include_directories(link_tests PUBLIC include)
    target_compile_options(link_tests PRIVATE -O2 -ffp-contract=off)
    target_link_libraries(link_tests PUBLIC klippy klippy_lite mcu_link)
endif()
