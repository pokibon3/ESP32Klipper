// WiFi station + setup access point (see wifi_setup.hpp).
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "wifi_setup/wifi_setup.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"

namespace wifi_setup {

extern const char kSetupPage[]; // setup_page.cpp

namespace {

const char *TAG = "wifi_setup";
const char *NVS_NS = "wifi";
const char *AP_IP = "192.168.4.1";
constexpr int64_t kApLingerUs = 60 * 1000000LL; // AP stays after connecting

std::mutex lock;
Config cfg;
esp_netif_t *sta_if, *ap_if;
std::string ssid_, pass_, ip_, message_, ap_ssid_;
bool connected_ = false, ap_active_ = false, scanning_ = false;
bool scan_done_ = false;
std::vector<Network> scan_;
int64_t down_since_us_ = 0, connected_since_us_ = 0, retry_at_us_ = 0;

int64_t
now_us()
{
    return esp_timer_get_time();
}

/****************************************************************
 * Saved network (NVS)
 ****************************************************************/

std::string
nvs_str(nvs_handle_t h, const char *key)
{
    size_t len = 0;
    if (nvs_get_str(h, key, nullptr, &len) != ESP_OK || len == 0)
        return "";
    std::string s(len, '\0');
    nvs_get_str(h, key, &s[0], &len);
    s.resize(len ? len - 1 : 0);
    return s;
}

void
load_network()
{
    nvs_handle_t h;
    std::string s, p;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        s = nvs_str(h, "ssid");
        p = nvs_str(h, "pass");
        nvs_close(h);
    }
    if (s.empty()) {
        s = cfg.default_ssid;
        p = cfg.default_password;
    }
    ssid_ = s;
    pass_ = p;
}

void
save_network(const std::string &s, const std::string &p)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK)
        return;
    if (s.empty()) {
        nvs_erase_key(h, "ssid");
        nvs_erase_key(h, "pass");
    } else {
        nvs_set_str(h, "ssid", s.c_str());
        nvs_set_str(h, "pass", p.c_str());
    }
    nvs_commit(h);
    nvs_close(h);
}

// Station config from ssid_ / pass_ (lock held)
void
apply_sta_config()
{
    wifi_config_t wc = {};
    strncpy((char *)wc.sta.ssid, ssid_.c_str(), sizeof(wc.sta.ssid));
    strncpy((char *)wc.sta.password, pass_.c_str(), sizeof(wc.sta.password));
    wc.sta.threshold.authmode = pass_.empty() ? WIFI_AUTH_OPEN
                                              : WIFI_AUTH_WPA_PSK;
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    esp_wifi_set_config(WIFI_IF_STA, &wc);
}

/****************************************************************
 * Captive DNS (every name -> the AP address) while the AP is up
 ****************************************************************/

void
dns_task(void *)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(53);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (s < 0 || bind(s, (sockaddr *)&a, sizeof(a)) != 0) {
        ESP_LOGW(TAG, "DNS: bind failed");
        if (s >= 0)
            close(s);
        vTaskDelete(nullptr);
        return;
    }
    timeval tv = {1, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    uint8_t buf[512];
    uint32_t ip = inet_addr(AP_IP);
    for (;;) {
        {
            std::lock_guard<std::mutex> g(lock);
            if (!ap_active_)
                break;
        }
        sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(s, buf, sizeof(buf) - 16, 0, (sockaddr *)&from, &fl);
        // Standard query with one question
        if (n < 12 || (buf[2] & 0x80) || buf[4] != 0 || buf[5] != 1)
            continue;
        int q = 12;
        while (q < n && buf[q])
            q += buf[q] + 1;
        q += 5; // zero label + type + class
        if (q > n)
            continue;
        bool type_a = buf[q - 4] == 0 && buf[q - 3] == 1;
        buf[2] = 0x84 | (buf[2] & 0x01); // response, authoritative, RD
        buf[3] = 0x80;                   // RA, no error
        buf[6] = 0;
        buf[7] = type_a ? 1 : 0;         // answers
        memset(buf + 8, 0, 4);
        int r = q;
        if (type_a) {
            const uint8_t ans[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4};
            memcpy(buf + r, ans, sizeof(ans));
            r += sizeof(ans);
            memcpy(buf + r, &ip, 4);
            r += 4;
        }
        sendto(s, buf, r, 0, (sockaddr *)&from, fl);
    }
    close(s);
    vTaskDelete(nullptr);
}

/****************************************************************
 * Setup access point
 ****************************************************************/

void
start_ap()
{
    // lock held
    wifi_config_t ac = {};
    strncpy((char *)ac.ap.ssid, ap_ssid_.c_str(), sizeof(ac.ap.ssid));
    ac.ap.ssid_len = ap_ssid_.size();
    ac.ap.channel = 1;
    ac.ap.max_connection = 3;
    if (cfg.ap_password.size() >= 8) {
        strncpy((char *)ac.ap.password, cfg.ap_password.c_str(),
                sizeof(ac.ap.password));
        ac.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ac.ap.authmode = WIFI_AUTH_OPEN;
    }
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ac);
    ap_active_ = true;
    xTaskCreate(dns_task, "dns", 3072, nullptr, 2, nullptr);
    ESP_LOGI(TAG, "setup AP %s up (http://%s/wifi)", ap_ssid_.c_str(), AP_IP);
}

void
stop_ap()
{
    // lock held; the DNS task ends by itself
    ap_active_ = false;
    esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGI(TAG, "setup AP down");
}

int
ap_clients()
{
    wifi_sta_list_t l = {};
    if (esp_wifi_ap_get_sta_list(&l) != ESP_OK)
        return 0;
    return l.num;
}

/****************************************************************
 * Events and supervision
 ****************************************************************/

const char *
reason_text(int r)
{
    switch (r) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
        return "wrong password?";
    case WIFI_REASON_NO_AP_FOUND:
        return "network not found";
    case WIFI_REASON_ASSOC_LEAVE:
        return nullptr; // our own disconnect
    default:
        return "connection lost";
    }
}

void
on_event(void *, esp_event_base_t base, int32_t id, void *data)
{
    std::lock_guard<std::mutex> g(lock);
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (!ssid_.empty())
            esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        auto *ev = (wifi_event_sta_disconnected_t *)data;
        if (connected_)
            down_since_us_ = now_us();
        connected_ = false;
        ip_.clear();
        if (const char *t = reason_text(ev->reason))
            message_ = t;
        // Retry later (a retry scans all channels and disturbs the setup
        // AP: less often while someone uses it)
        retry_at_us_ = now_us() + (ap_active_ ? 20 : 5) * 1000000LL;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto *ev = (ip_event_got_ip_t *)data;
        char buf[20];
        snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ev->ip_info.ip));
        ip_ = buf;
        connected_ = true;
        message_.clear();
        connected_since_us_ = now_us();
        retry_at_us_ = 0;
        ESP_LOGI(TAG, "connected to %s, %s", ssid_.c_str(), buf);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        uint16_t n = 0;
        esp_wifi_scan_get_ap_num(&n);
        std::vector<wifi_ap_record_t> rec(n);
        if (n)
            esp_wifi_scan_get_ap_records(&n, rec.data());
        rec.resize(n);
        scan_.clear();
        for (auto &r : rec) {
            std::string s((const char *)r.ssid);
            if (s.empty())
                continue;
            auto it = std::find_if(scan_.begin(), scan_.end(),
                                   [&](const Network &x) { return x.ssid == s; });
            if (it != scan_.end()) {
                it->rssi = std::max(it->rssi, (int)r.rssi);
                continue;
            }
            Network nw;
            nw.ssid = s;
            nw.rssi = r.rssi;
            nw.secure = r.authmode != WIFI_AUTH_OPEN;
            scan_.push_back(nw);
        }
        std::sort(scan_.begin(), scan_.end(),
                  [](const Network &a, const Network &b) { return a.rssi > b.rssi; });
        scanning_ = false;
        scan_done_ = true;
        if (!connected_ && !ssid_.empty())
            retry_at_us_ = now_us() + 500000;
    }
}

void
supervisor_task(void *)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        std::lock_guard<std::mutex> g(lock);
        int64_t now = now_us();
        if (!connected_ && retry_at_us_ && now >= retry_at_us_ && !scanning_
            && !ssid_.empty()) {
            retry_at_us_ = 0;
            esp_wifi_connect();
        }
        bool unreachable = ssid_.empty()
                           || now - down_since_us_ > cfg.connect_timeout_s * 1000000LL;
        if (!connected_ && !ap_active_ && unreachable)
            start_ap();
        if (connected_ && ap_active_ && now - connected_since_us_ > kApLingerUs
            && ap_clients() == 0)
            stop_ap();
    }
}

std::string
json_str(const std::string &s)
{
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') {
            o += '\\';
            o += c;
        } else if ((unsigned char)c < 0x20) {
            char b[8];
            snprintf(b, sizeof(b), "\\u%04x", c);
            o += b;
        } else {
            o += c;
        }
    }
    return o + "\"";
}

std::string
url_decode(const std::string &s)
{
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '+')
            o += ' ';
        else if (s[i] == '%' && i + 2 < s.size()) {
            o += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else
            o += s[i];
    }
    return o;
}

std::string
form_value(const std::string &body, const std::string &key)
{
    size_t p = 0;
    while (p <= body.size()) {
        size_t e = body.find('&', p);
        if (e == std::string::npos)
            e = body.size();
        std::string kv = body.substr(p, e - p);
        size_t eq = kv.find('=');
        if (eq != std::string::npos && url_decode(kv.substr(0, eq)) == key)
            return url_decode(kv.substr(eq + 1));
        p = e + 1;
    }
    return "";
}

std::string
status_json()
{
    Status s = status();
    return std::string("{\"ssid\":") + json_str(s.ssid)
           + ",\"connected\":" + (s.connected ? "true" : "false")
           + ",\"ip\":" + json_str(s.ip)
           + ",\"ap\":" + (s.ap_active ? "true" : "false")
           + ",\"ap_ssid\":" + json_str(s.ap_ssid)
           + ",\"message\":" + json_str(s.message) + "}";
}

esp_err_t
send(httpd_req_t *req, const char *type, const std::string &body)
{
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body.data(), body.size());
}

} // namespace

/****************************************************************
 * Interface
 ****************************************************************/

void
start(const Config &c)
{
    cfg = c;
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char buf[40];
    snprintf(buf, sizeof(buf), "ESP32Klipper-%02X%02X", mac[4], mac[5]);
    ap_ssid_ = buf;
    {
        std::lock_guard<std::mutex> g(lock);
        load_network();
        down_since_us_ = now_us();
    }
    esp_netif_init();
    esp_event_loop_create_default();
    sta_if = esp_netif_create_default_wifi_sta();
    ap_if = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(sta_if, cfg.hostname.c_str());
    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&ic);
    esp_wifi_set_storage(WIFI_STORAGE_RAM); // the network is kept in our NVS
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, nullptr);
    esp_wifi_set_mode(WIFI_MODE_STA);
    {
        std::lock_guard<std::mutex> g(lock);
        apply_sta_config();
    }
    esp_wifi_set_ps(WIFI_PS_NONE); // low latency for the WebSocket
    esp_wifi_start();
    xTaskCreate(supervisor_task, "wifi_setup", 3072, nullptr, 2, nullptr);
}

Status
status()
{
    std::lock_guard<std::mutex> g(lock);
    Status s;
    s.has_network = !ssid_.empty();
    s.ssid = ssid_;
    s.connected = connected_;
    s.ip = ip_;
    s.ap_active = ap_active_;
    s.ap_ssid = ap_ssid_;
    s.ap_password = cfg.ap_password.size() >= 8 ? cfg.ap_password : "";
    s.ap_ip = AP_IP;
    s.message = message_;
    return s;
}

std::string
status_text()
{
    Status s = status();
    std::string t;
    if (s.connected)
        t = "WiFi " + s.ssid + ": " + s.ip;
    else if (s.has_network)
        t = "WiFi " + s.ssid + ": connecting"
            + (s.message.empty() ? "" : " (" + s.message + ")");
    else
        t = "WiFi: not set up";
    return t;
}

void
start_scan()
{
    std::lock_guard<std::mutex> g(lock);
    if (scanning_)
        return;
    scan_done_ = false;
    // A station that is still trying to connect cannot scan
    if (!connected_)
        esp_wifi_disconnect();
    wifi_scan_config_t sc = {};
    sc.show_hidden = false;
    if (esp_wifi_scan_start(&sc, false) == ESP_OK) {
        scanning_ = true;
    } else {
        scan_.clear();
        scan_done_ = true;
        retry_at_us_ = now_us() + 500000;
    }
}

bool
scan_results(std::vector<Network> *out)
{
    std::lock_guard<std::mutex> g(lock);
    if (!scan_done_)
        return false;
    *out = scan_;
    return true;
}

void
connect(const std::string &ssid, const std::string &password)
{
    save_network(ssid, password);
    std::lock_guard<std::mutex> g(lock);
    ssid_ = ssid;
    pass_ = password;
    message_.clear();
    connected_ = false;
    ip_.clear();
    down_since_us_ = now_us();
    retry_at_us_ = 0;
    esp_wifi_disconnect();
    apply_sta_config();
    esp_wifi_connect();
}

void
forget()
{
    save_network("", "");
    std::lock_guard<std::mutex> g(lock);
    load_network();
    esp_wifi_disconnect();
    apply_sta_config();
    connected_ = false;
    down_since_us_ = now_us();
    retry_at_us_ = ssid_.empty() ? 0 : now_us() + 1000000;
}

bool
http_hook(httpd_req_t *req, esp_err_t *ret)
{
    std::string uri = req->uri;
    std::string path = uri.substr(0, uri.find('?'));
    bool ap;
    {
        std::lock_guard<std::mutex> g(lock);
        ap = ap_active_;
    }
    // Clients of the setup AP asking for other hosts (phone captive-portal
    // checks): send them to the setup page
    if (ap && path.compare(0, 5, "/wifi") != 0) {
        sockaddr_in local = {};
        socklen_t ll = sizeof(local);
        int fd = httpd_req_to_sockfd(req);
        char host[64] = "";
        httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
        if (getsockname(fd, (sockaddr *)&local, &ll) == 0
            && local.sin_addr.s_addr == inet_addr(AP_IP)
            && strncmp(host, AP_IP, strlen(AP_IP)) != 0) {
            httpd_resp_set_status(req, "302 Found");
            httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/wifi");
            *ret = httpd_resp_send(req, nullptr, 0);
            return true;
        }
    }
    if (path == "/wifi" || path == "/wifi/") {
        *ret = send(req, "text/html", kSetupPage);
    } else if (path == "/wifi/status") {
        *ret = send(req, "application/json", status_json());
    } else if (path == "/wifi/scan") {
        start_scan();
        std::vector<Network> nets;
        for (int i = 0; i < 80 && !scan_results(&nets); i++)
            vTaskDelay(pdMS_TO_TICKS(100));
        std::string o = "[";
        for (auto &n : nets) {
            if (o.size() > 1)
                o += ",";
            o += "{\"ssid\":" + json_str(n.ssid) + ",\"rssi\":"
                 + std::to_string(n.rssi) + ",\"secure\":"
                 + (n.secure ? "true" : "false") + "}";
        }
        *ret = send(req, "application/json", o + "]");
    } else if (path == "/wifi/save" && req->method == HTTP_POST) {
        std::string body;
        if (req->content_len > 0 && req->content_len < 1024) {
            body.resize(req->content_len);
            size_t got = 0;
            while (got < body.size()) {
                int n = httpd_req_recv(req, &body[got], body.size() - got);
                if (n <= 0)
                    break;
                got += n;
            }
            body.resize(got);
        }
        std::string s = form_value(body, "ssid");
        if (s.empty() || s.size() > 32) {
            httpd_resp_set_status(req, "400 Bad Request");
            *ret = send(req, "application/json", "{\"error\":\"bad ssid\"}");
        } else {
            connect(s, form_value(body, "password"));
            *ret = send(req, "application/json", "{\"ok\":true}");
        }
    } else {
        return false;
    }
    return true;
}

} // namespace wifi_setup
