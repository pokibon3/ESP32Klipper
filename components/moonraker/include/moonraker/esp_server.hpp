// HTTP + WebSocket server for the Moonraker API on the ESP32 (ESP-IDF
// esp_http_server): Mainsail's files from a directory on the board's
// storage, /websocket, file upload/download and the HTTP API.
//
// This file may be distributed under the terms of the GNU GPLv3 license.
#pragma once

#include <functional>
#include <string>

#include "esp_http_server.h"

namespace kl {

class Moonraker;

struct EspServerConfig {
    int port = 80;
    std::string www_dir = "/www"; // VFS path of Mainsail's files
    int max_sockets = 7;          // HTTP + WebSocket connections
    int stack = 16384;            // 8 KB overflowed with Mainsail (JSON in the handler)
    int priority = 3;
    int core = 0;
    // Called first for every HTTP request (not the WebSocket); returns
    // true when it handled the request (e.g. the WiFi setup page)
    std::function<bool(httpd_req_t *req, esp_err_t *ret)> hook;
};

// Starts the server and a task that calls Moonraker::tick()
bool esp_server_start(Moonraker &mr, const EspServerConfig &cfg,
                      std::string *err);

} // namespace kl
