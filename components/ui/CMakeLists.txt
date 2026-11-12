# Local touch UI (LVGL)
set(UI_SRCS ${CMAKE_CURRENT_LIST_DIR}/src/plotter_ui.cpp)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${UI_SRCS}
                           INCLUDE_DIRS include
                           REQUIRES klippy lvgl)
else()
    add_library(ui STATIC ${UI_SRCS})
    target_include_directories(ui PUBLIC include)
    target_link_libraries(ui PUBLIC klippy lvgl)
endif()
