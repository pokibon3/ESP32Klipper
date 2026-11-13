// WiFi of the plotter: saved network (NVS), setup access point, scan.
// See wifi_setup.hpp.
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "sdkconfig.h"
#if CONFIG_PLOTTER_NETWORK
#include "wifi_setup.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs.h"

namespace wifi {
namespace {

const char *TAG = "wifi";
const char *NVS_NS = "wifi";
// While the access point is open, retry the saved network this often:
// each try moves the radio to the network's channel and drops the phone
// on the setup page for a moment.
const uint64_t AP_RETRY_US = 30 * 1000000ULL;
// Failed tries in a row (while joined before) before the access point
// opens again: ~1 min with a wrong password
const int AP_AFTER_FAILS = 10;
// After joining, keep the access point this long (the setup page shows
// the result), then close it
const uint64_t AP_CLOSE_US = 60 * 1000000ULL;

std::mutex lock;
EventGroupHandle_t events;
esp_timer_handle_t retry_timer, close_timer, join_timer;
std::string ssid_, pass_, ip_, ap_name_, ap_pass_;
bool ap_on_ = false, scanning_ = false, scan_done_ = false;
// Access point mode: the board is the network (no other network joined)
bool ap_mode_ = false;
std::vector<kl::WifiNetwork> nets_;
int fails_ = 0;

bool
load(std::string *ssid, std::string *pass)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK)
        return false;
    char s[33] = "", p[65] = "";
    size_t ls = sizeof(s), lp = sizeof(p);
    bool ok = nvs_get_str(h, "ssid", s, &ls) == ESP_OK && s[0];
    if (ok && nvs_get_str(h, "pass", p, &lp) != ESP_OK)
        p[0] = 0;
    nvs_close(h);
    if (ok) {
        *ssid = s;
        *pass = p;
    }
    return ok;
}

void
save(const std::string &ssid, const std::string &pass)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK)
        return;
    nvs_set_str(h, "ssid", ssid.c_str());
    nvs_set_str(h, "pass", pass.c_str());
    nvs_commit(h);
    nvs_close(h);
}

// Access point settings: the mode, and the password made once per board
// (menuconfig's password instead when set)
void
load_ap()
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK)
        return;
    uint8_t mode = 0;
    nvs_get_u8(h, "mode", &mode);
    ap_mode_ = mode == 1;
    char p[65] = "";
    size_t lp = sizeof(p);
    if (nvs_get_str(h, "ap_pass", p, &lp) != ESP_OK || strlen(p) < 8) {
        // No 0/O, 1/l/I: read from the screen
        const char *chars = "abcdefghijkmnpqrstuvwxyz23456789";
        for (int i = 0; i < 8; i++)
            p[i] = chars[esp_random() % 32];
        p[8] = 0;
        nvs_set_str(h, "ap_pass", p);
        nvs_commit(h);
    }
    nvs_close(h);
    ap_pass_ = strlen(CONFIG_PLOTTER_AP_PASSWORD) >= 8
                   ? CONFIG_PLOTTER_AP_PASSWORD : p;
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char name[32];
    snprintf(name, sizeof(name), "ESP32Klipper-%02X%02X", mac[4], mac[5]);
    ap_name_ = name;
}

void
save_mode(bool ap)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK)
        return;
    nvs_set_u8(h, "mode", ap ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

void
apply_sta_config()
{
    wifi_config_t wc = {};
    strncpy((char *)wc.sta.ssid, ssid_.c_str(), sizeof(wc.sta.ssid));
    strncpy((char *)wc.sta.password, pass_.c_str(), sizeof(wc.sta.password));
    esp_wifi_set_config(WIFI_IF_STA, &wc);
}

void
open_ap()
{
    if (ap_on_)
        return;
    wifi_config_t ac = {};
    strncpy((char *)ac.ap.ssid, ap_name_.c_str(), sizeof(ac.ap.ssid));
    ac.ap.ssid_len = ap_name_.size();
    strncpy((char *)ac.ap.password, ap_pass_.c_str(), sizeof(ac.ap.password));
    ac.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ac.ap.max_connection = 2;
    ac.ap.channel = 1;
    // AP + station: the station scans (network list) and joins
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ac);
    {
        std::lock_guard<std::mutex> g(lock);
        ap_on_ = true;
    }
    ESP_LOGW(TAG, "access point %s (password %s): http://192.168.4.1/",
             ap_name_.c_str(), ap_pass_.c_str());
}

void
close_ap(void *)
{
    {
        std::lock_guard<std::mutex> g(lock);
        if (!ap_on_ || ip_.empty() || ap_mode_)
            return;
        ap_on_ = false;
    }
    esp_wifi_set_mode(WIFI_MODE_STA);
}

void
retry(void *)
{
    bool go;
    {
        std::lock_guard<std::mutex> g(lock);
        go = !scanning_ && ip_.empty() && !ssid_.empty() && !ap_mode_;
    }
    if (go)
        esp_wifi_connect();
}

void
on_event(void *, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        retry(nullptr);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        bool ap, scanning;
        int fails;
        {
            std::lock_guard<std::mutex> g(lock);
            ip_.clear();
            if (ap_mode_)
                return;
            fails = ++fails_;
            ap = ap_on_;
            scanning = scanning_;
        }
        xEventGroupClearBits(events, 1);
        if (scanning)
            return; // SCAN_DONE tries again
        // Network gone or wrong password (e.g. after joining another one
        // from the screen): the setup access point again
        if (!ap && fails >= AP_AFTER_FAILS) {
            open_ap();
            ap = true;
        }
        esp_timer_stop(retry_timer);
        esp_timer_start_once(retry_timer, ap ? AP_RETRY_US : 1000000);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto *ev = (ip_event_got_ip_t *)data;
        char buf[20];
        snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ev->ip_info.ip));
        bool ap;
        {
            std::lock_guard<std::mutex> g(lock);
            ip_ = buf;
            fails_ = 0;
            ap = ap_on_;
        }
        xEventGroupSetBits(events, 1);
        if (ap) {
            esp_timer_stop(close_timer);
            esp_timer_start_once(close_timer, AP_CLOSE_US);
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        uint16_t n = 0;
        esp_wifi_scan_get_ap_num(&n);
        std::vector<wifi_ap_record_t> rec(n);
        if (n)
            esp_wifi_scan_get_ap_records(&n, rec.data());
        rec.resize(n);
        std::vector<kl::WifiNetwork> nets;
        for (const auto &r : rec) {
            std::string s((const char *)r.ssid);
            if (s.empty())
                continue; // hidden
            auto it = std::find_if(nets.begin(), nets.end(),
                                   [&](const kl::WifiNetwork &w) {
                                       return w.ssid == s;
                                   });
            if (it != nets.end()) {
                it->rssi = std::max(it->rssi, (int)r.rssi);
                continue;
            }
            kl::WifiNetwork w;
            w.ssid = s;
            w.rssi = r.rssi;
            w.secure = r.authmode != WIFI_AUTH_OPEN;
            nets.push_back(w);
        }
        std::sort(nets.begin(), nets.end(),
                  [](const kl::WifiNetwork &a, const kl::WifiNetwork &b) {
                      return a.rssi > b.rssi;
                  });
        {
            std::lock_guard<std::mutex> g(lock);
            nets_ = std::move(nets);
            scanning_ = false;
            scan_done_ = true;
        }
        retry(nullptr);
    }
}

void
join_now(void *)
{
    esp_wifi_disconnect();
    apply_sta_config();
    retry(nullptr);
}

/****************************************************************
 * Setup page
 ****************************************************************/

std::string
html_escape(const std::string &s)
{
    std::string o;
    for (char c : s) {
        switch (c) {
        case '&': o += "&amp;"; break;
        case '<': o += "&lt;"; break;
        case '>': o += "&gt;"; break;
        case '"': o += "&quot;"; break;
        case '\'': o += "&#39;"; break;
        default: o += c;
        }
    }
    return o;
}

std::string
url_decode(const std::string &s)
{
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '+') {
            o += ' ';
        } else if (s[i] == '%' && i + 2 < s.size()) {
            o += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else {
            o += s[i];
        }
    }
    return o;
}

std::string
form_value(const std::string &body, const char *key)
{
    std::string k = std::string(key) + "=";
    size_t p = 0;
    while (p < body.size()) {
        size_t e = body.find('&', p);
        if (e == std::string::npos)
            e = body.size();
        if (body.compare(p, k.size(), k) == 0)
            return url_decode(body.substr(p + k.size(), e - p - k.size()));
        p = e + 1;
    }
    return "";
}

const char *PAGE_HEAD =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP32Klipper WiFi</title><style>"
    "body{font-family:sans-serif;max-width:28em;margin:1em auto;padding:0 1em;"
    "background:#1d2125;color:#e8e8e8}"
    "a,button{color:#fff}label{display:block;margin:.4em 0;padding:.5em;"
    "background:#2b3036;border-radius:6px}"
    "input[type=password],input[type=text]{width:100%;padding:.5em;"
    "box-sizing:border-box;margin:.5em 0}"
    "button{background:#1e88e5;border:0;padding:.7em 1.4em;border-radius:6px;"
    "font-size:1em}small{color:#aaa}"
    "</style></head><body><h2>ESP32Klipper WiFi</h2>";

esp_err_t
send_page(httpd_req_t *req, const std::string &body)
{
    std::string s = PAGE_HEAD + body + "</body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, s.data(), s.size());
}

esp_err_t
get_page(httpd_req_t *req)
{
    std::vector<kl::WifiNetwork> nets;
    bool have = scan_results(&nets);
    std::string b = "<p>" + html_escape(status()) + "</p>";
    if (!have) {
        // Scanning: look again shortly
        httpd_resp_set_hdr(req, "Refresh", "3");
        b += "<p>Scanning...</p>";
        return send_page(req, b);
    }
    b += "<form method='post' action='/wifi'>";
    for (size_t i = 0; i < nets.size(); i++) {
        const auto &n = nets[i];
        std::string e = html_escape(n.ssid);
        char extra[48];
        snprintf(extra, sizeof(extra), " <small>%d dBm%s</small>", n.rssi,
                 n.secure ? "" : ", open");
        b += "<label><input type='radio' name='ssid' value='" + e + "'"
             + (i == 0 ? " checked" : "") + "> " + e + extra + "</label>";
    }
    b += "<label>Other network: <input type='text' name='other' "
         "maxlength='32' placeholder='SSID'></label>"
         "<input type='password' name='password' maxlength='64' "
         "placeholder='Password (empty: open network)'>"
         "<button type='submit'>Join</button></form>"
         "<p><a href='/wifi?scan=1'>Scan again</a></p>";
    if (!ap_mode())
        b += "<form method='post' action='/wifi'>"
             "<input type='hidden' name='mode' value='ap'>"
             "<p><button type='submit' style='background:#00897b'>"
             "Use as access point</button><br><small>The board becomes its "
             "own WiFi network (" + html_escape(ap_name()) + "); Mainsail at "
             "http://192.168.4.1/</small></p></form>";
    return send_page(req, b);
}

esp_err_t
post_page(httpd_req_t *req)
{
    if (req->content_len > 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "too long");
        return ESP_FAIL;
    }
    std::string body(req->content_len, '\0');
    size_t got = 0;
    while (got < body.size()) {
        int r = httpd_req_recv(req, &body[got], body.size() - got);
        if (r <= 0)
            return ESP_FAIL;
        got += r;
    }
    if (form_value(body, "mode") == "ap") {
        set_ap_mode(true);
        return send_page(req, "<p>Access point mode: join <b>"
                                  + html_escape(ap_name()) + "</b> (password "
                                  "on the board's screen), then open "
                                  "http://192.168.4.1/</p>");
    }
    std::string ssid = form_value(body, "other");
    if (ssid.empty())
        ssid = form_value(body, "ssid");
    std::string pw = form_value(body, "password");
    if (ssid.empty() || ssid.size() > 32 || pw.size() > 64
        || (!pw.empty() && pw.size() < 8))
        return send_page(req, "<p>Check the network name and the password "
                              "(8 to 64 characters, or empty).</p>"
                              "<p><a href='/wifi'>Back</a></p>");
    join(ssid, pw);
    return send_page(
        req, "<p>Saved. Joining <b>" + html_escape(ssid)
                 + "</b>...</p><p>This access point closes a minute after "
                   "the board has joined. The board's address is then on "
                   "its screen (System tab) and in your router's list "
                   "(host name " CONFIG_PLOTTER_HOSTNAME ").</p>"
                   "<p><a href='/wifi'>Status</a></p>");
}

} // namespace

bool
start(int wait_ms)
{
    events = xEventGroupCreate();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(sta, CONFIG_PLOTTER_HOSTNAME);
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&ic);
    esp_wifi_set_storage(WIFI_STORAGE_RAM); // our own NVS keys instead
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event,
                               nullptr);
    esp_timer_create_args_t ta = {};
    ta.callback = retry;
    ta.name = "wifi_retry";
    esp_timer_create(&ta, &retry_timer);
    ta.callback = close_ap;
    ta.name = "wifi_ap_close";
    esp_timer_create(&ta, &close_timer);
    ta.callback = join_now;
    ta.name = "wifi_join";
    esp_timer_create(&ta, &join_timer);

    if (!load(&ssid_, &pass_)) {
        ssid_ = CONFIG_PLOTTER_WIFI_SSID;
        pass_ = CONFIG_PLOTTER_WIFI_PASSWORD;
    }
    load_ap();
    esp_wifi_set_mode(WIFI_MODE_STA);
    apply_sta_config();
    // Keep latency low for the WebSocket (no modem sleep)
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_start();
    if (ap_mode_) {
        open_ap();
        scan_start();
        return false;
    }
    bool joined = false;
    if (!ssid_.empty())
        joined = xEventGroupWaitBits(events, 1, pdFALSE, pdTRUE,
                                     pdMS_TO_TICKS(wait_ms)) & 1;
    if (!joined) {
        open_ap();
        scan_start(); // ready for the setup page
    }
    return joined;
}

std::string
ap_name()
{
    std::lock_guard<std::mutex> g(lock);
    return ap_name_;
}

std::string
ip()
{
    std::lock_guard<std::mutex> g(lock);
    return ip_;
}

std::string
status()
{
    std::lock_guard<std::mutex> g(lock);
    std::string s;
    if (ap_mode_)
        return "AP " + ap_name_ + " / pass " + ap_pass_
               + "\nMainsail: http://192.168.4.1/";
    if (!ip_.empty())
        s = "Connected: " + ssid_ + " (" + ip_ + ")";
    else if (ssid_.empty())
        s = "No network set";
    else
        s = "Connecting: " + ssid_ + (fails_ ? " (failed, retrying)" : "");
    if (ap_on_)
        s += "\nSetup AP " + ap_name_ + " / pass " + ap_pass_;
    return s;
}

void
scan_start()
{
    {
        std::lock_guard<std::mutex> g(lock);
        if (scanning_)
            return;
        scanning_ = true;
        scan_done_ = false;
    }
    // A scan fails while the station is connecting: stop trying first
    esp_timer_stop(retry_timer);
    bool joined = !ip().empty();
    if (!joined)
        esp_wifi_disconnect();
    wifi_scan_config_t sc = {};
    sc.show_hidden = false;
    if (esp_wifi_scan_start(&sc, false) != ESP_OK) {
        ESP_LOGW(TAG, "scan failed to start");
        std::lock_guard<std::mutex> g(lock);
        scanning_ = false;
        scan_done_ = true;
    }
}

bool
scan_results(std::vector<kl::WifiNetwork> *out)
{
    std::lock_guard<std::mutex> g(lock);
    if (scanning_ || !scan_done_)
        return false;
    *out = nets_;
    return true;
}

void
join(const std::string &ssid, const std::string &password)
{
    save(ssid, password);
    save_mode(false);
    {
        std::lock_guard<std::mutex> g(lock);
        ssid_ = ssid;
        pass_ = password;
        fails_ = 0;
        ap_mode_ = false; // the access point closes once joined
    }
    ESP_LOGW(TAG, "joining %s", ssid.c_str());
    // Shortly, so that the setup page's answer goes out first (joining
    // may move the radio to another channel)
    esp_timer_stop(retry_timer);
    esp_timer_stop(join_timer);
    esp_timer_start_once(join_timer, 1000000);
}

void
set_ap_mode(bool on)
{
    save_mode(on);
    {
        std::lock_guard<std::mutex> g(lock);
        if (ap_mode_ == on)
            return;
        ap_mode_ = on;
        fails_ = 0;
    }
    ESP_LOGW(TAG, "%s", on ? "access point mode" : "joining the network");
    esp_timer_stop(retry_timer);
    if (on) {
        esp_wifi_disconnect();
        open_ap();
    } else {
        // The access point stays until joined (close_timer)
        retry(nullptr);
    }
}

bool
ap_mode()
{
    std::lock_guard<std::mutex> g(lock);
    return ap_mode_;
}

std::string
host()
{
    std::lock_guard<std::mutex> g(lock);
    if (!ip_.empty())
        return ip_;
    return ap_on_ ? "192.168.4.1" : "";
}

bool
http_hook(httpd_req_t *req, esp_err_t *ret)
{
    const char *uri = req->uri;
    bool wifi_page = strncmp(uri, "/wifi", 5) == 0
                     && (uri[5] == 0 || uri[5] == '?');
    if (!wifi_page) {
        // Not joined: the setup page instead of Mainsail
        bool root = strcmp(uri, "/") == 0 || strcmp(uri, "/index.html") == 0;
        if (!(root && req->method == HTTP_GET && ip().empty() && !ap_mode()))
            return false;
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/wifi");
        *ret = httpd_resp_send(req, nullptr, 0);
        return true;
    }
    if (req->method == HTTP_POST) {
        *ret = post_page(req);
        return true;
    }
    if (req->method != HTTP_GET)
        return false;
    if (strstr(uri, "scan=1")) {
        scan_start();
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "/wifi");
        *ret = httpd_resp_send(req, nullptr, 0);
        return true;
    }
    *ret = get_page(req);
    return true;
}

} // namespace wifi
#endif // CONFIG_PLOTTER_NETWORK
