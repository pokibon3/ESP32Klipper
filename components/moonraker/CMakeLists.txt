# Moonraker-compatible API (Mainsail).  posix_server.cpp is the host's
# HTTP/WebSocket server; the board uses esp_server.cpp.
set(MR_SRCS ${CMAKE_CURRENT_LIST_DIR}/src/json.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/moonraker.cpp
    ${CMAKE_CURRENT_LIST_DIR}/src/sha1.cpp)

if(ESP_PLATFORM)
    idf_component_register(SRCS ${MR_SRCS} src/esp_server.cpp
                           INCLUDE_DIRS include
                           REQUIRES klippy mcu_link esp_http_server)
else()
    add_library(moonraker STATIC ${MR_SRCS}
                ${CMAKE_CURRENT_LIST_DIR}/src/posix_server.cpp)
    target_include_directories(moonraker PUBLIC include)
    target_link_libraries(moonraker PUBLIC klippy mcu_link)
endif()
