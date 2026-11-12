// WiFi for the plotter (ESP32 station) with two ways to set it up after
// flashing:
//   - the touch screen (scan(), connect() from the UI)
//   - a setup access point "ESP32Klipper-XXXX": started when no network is
//     saved or the saved one cannot be reached; phones open the setup page
//     (captive portal: every DNS name points at the board)
// The network is kept in NVS.  The build-time network (menuconfig) is used
// while none is saved.
//
// This file may be distributed under the terms of the GNU GPLv3 license.
#pragma once

#include <string>
#include <vector>

#include "esp_err.h"
#include "esp_http_server.h"

namespace wifi_setup {

struct Config {
    std::string hostname = "esp32klipper";
    std::string default_ssid, default_password; // build-time network
    std::string ap_password; // setup AP (8+ characters, empty = open)
    int connect_timeout_s = 30; // no connection for this long -> setup AP
};

struct Network {
    std::string ssid;
    int rssi = 0;
    bool secure = false;
};

struct Status {
    bool has_network = false; // saved or build-time network
    std::string ssid;         // network to join
    bool connected = false;
    std::string ip;           // station address when connected
    bool ap_active = false;
    std::string ap_ssid, ap_password, ap_ip;
    std::string message;      // last problem ("wrong password", ...)
};

// Starts WiFi (non-blocking); call once after nvs_flash_init()
void start(const Config &cfg);
Status status();
// One-line summary for screens and logs
std::string status_text();

// Network scan: start, then poll scan_results() until it returns true
void start_scan();
bool scan_results(std::vector<Network> *out);

// Save a network and join it (the setup AP stays up until the board has
// been connected for a while)
void connect(const std::string &ssid, const std::string &password);
// Forget the saved network (the build-time one is used again, if any)
void forget();

// For the HTTP server, before its own handlers: the setup page (/wifi,
// /wifi/scan, /wifi/status, /wifi/save) and, for clients of the setup AP,
// the captive-portal redirect.  Returns true when it handled the request.
bool http_hook(httpd_req_t *req, esp_err_t *ret);

} // namespace wifi_setup
