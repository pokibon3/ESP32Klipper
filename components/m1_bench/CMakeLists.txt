# M1 benchmark: runs G-code through klippy_lite with a message sink in
# place of the real serial queue (serialqueue_stub.c).

set(KLIPPER_CHELPER_DIR
    ${CMAKE_CURRENT_LIST_DIR}/../../third_party/klipper/klippy/chelper)
set(M1_BENCH_SRCS
    ${CMAKE_CURRENT_LIST_DIR}/src/bench.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/serialqueue_stub.c)

if(ESP_PLATFORM)
    idf_component_register(
        SRCS ${M1_BENCH_SRCS}
        INCLUDE_DIRS include
        PRIV_INCLUDE_DIRS ${KLIPPER_CHELPER_DIR}
        REQUIRES klippy_lite chelper)
    target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -ffp-contract=off)
else()
    add_library(m1_bench STATIC ${M1_BENCH_SRCS})
    target_include_directories(m1_bench PUBLIC include
                               PRIVATE ${KLIPPER_CHELPER_DIR})
    target_compile_options(m1_bench PRIVATE -O2 -ffp-contract=off)
    target_link_libraries(m1_bench PUBLIC klippy_lite)
endif()
