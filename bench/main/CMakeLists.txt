idf_component_register(
    SRCS main.cpp
    REQUIRES klippy_lite chelper esp_timer pthread heap
    EMBED_TXTFILES ${CMAKE_CURRENT_LIST_DIR}/../data/card_opt.gcode)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
