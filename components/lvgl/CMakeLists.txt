# LVGL 8.4 (third_party/lvgl, unmodified) with this project's lv_conf.h
set(LVGL_DIR ${CMAKE_CURRENT_LIST_DIR}/../../third_party/lvgl)
file(GLOB_RECURSE lvgl_srcs ${LVGL_DIR}/src/*.c)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${lvgl_srcs}
                           INCLUDE_DIRS ${LVGL_DIR} include)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC LV_CONF_INCLUDE_SIMPLE)
    target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Wno-unused-variable
                           -Wno-unused-function -Wno-maybe-uninitialized)
else()
    add_library(lvgl STATIC ${lvgl_srcs})
    target_include_directories(lvgl PUBLIC ${LVGL_DIR} include)
    target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE)
    target_compile_options(lvgl PRIVATE -O2 -w)
endif()
