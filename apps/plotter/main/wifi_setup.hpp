// WiFi of the plotter (CoreS3): joins the network saved in NVS (or the one
// of menuconfig); without one, or when it cannot join, it opens its own
// access point with a setup page (http://192.168.4.1/wifi) and keeps
// trying the saved network.  In access point mode the board stays its own
// network.  The access point's password is made once per board (shown on
// the screen and in the log).  The touch screen sets the network through
// the same functions (System tab -> WiFi).
//
// This file may be distributed under the terms of the GNU GPLv3 license.
#pragma once

#include <string>
#include <vector>

#include "esp_http_server.h"
#include "ui/plotter_ui.hpp"

namespace wifi {

// Starts WiFi; waits up to wait_ms for an address on the saved network,
// then opens the setup access point.  True when joined.
bool start(int wait_ms);
// Address on the network ("" when not joined)
std::string ip();
// Address for Mainsail: ip(), or 192.168.4.1 while the access point is
// open, or ""
std::string host();
// Access point mode: the board is its own network (ESP32Klipper-XXXX,
// WPA2 with a password made once per board), Mainsail at
// http://192.168.4.1/; saved.  join() leaves it.
void set_ap_mode(bool on);
bool ap_mode();
std::string ap_name();
// One line for the screen and the log
std::string status();
// Scan (asynchronous): results() true once done
void scan_start();
bool scan_results(std::vector<kl::WifiNetwork> *out);
// Save and join a network (empty password: open network)
void join(const std::string &ssid, const std::string &password);
// Setup page: GET/POST /wifi; while not joined, / goes there too.
// Returns true when it handled the request.
bool http_hook(httpd_req_t *req, esp_err_t *ret);

} // namespace wifi
