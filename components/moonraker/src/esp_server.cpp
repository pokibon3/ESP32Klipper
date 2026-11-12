// HTTP + WebSocket server for the Moonraker API on the ESP32 (see
// esp_server.hpp).  Uploads and downloads are streamed in small chunks so
// large G-code files do not need RAM.
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "moonraker/esp_server.hpp"

#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <sys/stat.h>

#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "moonraker/json.hpp"
#include "moonraker/moonraker.hpp"

namespace kl {

namespace {

constexpr size_t CHUNK = 2048;

struct Server {
    Moonraker *mr = nullptr;
    httpd_handle_t hd = nullptr;
    EspServerConfig cfg;
    std::mutex frag_lock;
    std::map<int, std::string> frags; // WebSocket messages being assembled
};
Server srv;

const char *
content_type(const std::string &path)
{
    auto ends = [&](const char *e) {
        size_t n = strlen(e);
        return path.size() >= n && path.compare(path.size() - n, n, e) == 0;
    };
    if (ends(".html")) return "text/html; charset=utf-8";
    if (ends(".js")) return "application/javascript";
    if (ends(".css")) return "text/css";
    if (ends(".json") || ends(".webmanifest")) return "application/json";
    if (ends(".svg")) return "image/svg+xml";
    if (ends(".png")) return "image/png";
    if (ends(".ico")) return "image/x-icon";
    if (ends(".woff2")) return "font/woff2";
    if (ends(".woff")) return "font/woff";
    return "application/octet-stream";
}

std::string
url_decode(const std::string &s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            out += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else if (s[i] == '+') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

bool
is_file(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

esp_err_t
send_json(httpd_req_t *req, int status, const std::string &body)
{
    char st[32];
    snprintf(st, sizeof(st), "%d %s", status,
             status == 200 ? "OK" : status == 201 ? "Created"
             : status == 404 ? "Not Found" : status == 503 ? "Unavailable"
                                                           : "Bad Request");
    httpd_resp_set_status(req, st);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, body.data(), body.size());
}

esp_err_t
send_stream(httpd_req_t *req, FILE *f, const char *type, bool gz)
{
    httpd_resp_set_type(req, type);
    if (gz)
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=3600");
    std::unique_ptr<char[]> buf(new char[CHUNK]);
    size_t n;
    while ((n = fread(buf.get(), 1, CHUNK, f)) > 0)
        if (httpd_resp_send_chunk(req, buf.get(), n) != ESP_OK) {
            fclose(f);
            return ESP_FAIL;
        }
    fclose(f);
    return httpd_resp_send_chunk(req, nullptr, 0);
}

esp_err_t
serve_static(httpd_req_t *req, const std::string &uri)
{
    std::string p = uri == "/" ? "/index.html" : uri;
    if (p.find("..") != std::string::npos)
        return send_json(req, 400, "{}");
    std::string file = srv.cfg.www_dir + p;
    if (is_file(file + ".gz"))
        return send_stream(req, fopen((file + ".gz").c_str(), "rb"),
                           content_type(p), true);
    if (FILE *f = fopen(file.c_str(), "rb"))
        return send_stream(req, f, content_type(p), false);
    // Single-page app routes -> index.html
    if (p.rfind('.') == std::string::npos || p.rfind('.') < p.rfind('/')) {
        std::string idx = srv.cfg.www_dir + "/index.html";
        if (is_file(idx + ".gz"))
            return send_stream(req, fopen((idx + ".gz").c_str(), "rb"),
                               "text/html", true);
        if (FILE *f = fopen(idx.c_str(), "rb"))
            return send_stream(req, f, "text/html", false);
        httpd_resp_set_type(req, "text/plain");
        std::string msg = "Mainsail is not installed in " + srv.cfg.www_dir;
        return httpd_resp_send(req, msg.data(), msg.size());
    }
    httpd_resp_send_404(req);
    return ESP_OK;
}

esp_err_t
serve_download(httpd_req_t *req, const std::string &uri)
{
    std::string rest = url_decode(uri.substr(strlen("/server/files/")));
    size_t s = rest.find('/');
    std::string root = rest.substr(0, s);
    std::string rel = s == std::string::npos ? "" : rest.substr(s + 1);
    auto fs = srv.mr->root_fs(root);
    std::unique_ptr<GcodeFile> f;
    if (fs && safe_relative_path(rel))
        f = fs->open(rel);
    if (!f)
        return send_json(req, 404,
                         "{\"error\":{\"code\":404,\"message\":\"File not found\"}}");
    httpd_resp_set_type(req, "application/octet-stream");
    std::unique_ptr<char[]> buf(new char[CHUNK]);
    size_t n;
    while ((n = f->read(buf.get(), CHUNK)) > 0)
        if (httpd_resp_send_chunk(req, buf.get(), n) != ESP_OK)
            return ESP_FAIL;
    return httpd_resp_send_chunk(req, nullptr, 0);
}

// Reads the request body in chunks
struct BodyReader {
    httpd_req_t *req;
    size_t left;
    std::string buf;
    bool fill() {
        if (!left)
            return false;
        char tmp[CHUNK];
        int n = httpd_req_recv(req, tmp, left < CHUNK ? left : CHUNK);
        if (n == HTTPD_SOCK_ERR_TIMEOUT)
            return true;
        if (n <= 0)
            return false;
        left -= n;
        buf.append(tmp, n);
        return true;
    }
};

// multipart/form-data upload, the file part streamed to a temporary file in
// the gcodes root (Mainsail sends the file before the root/path fields)
esp_err_t
handle_upload(httpd_req_t *req)
{
    char ct[160] = "";
    httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof(ct));
    const char *b = strstr(ct, "boundary=");
    if (!b)
        return send_json(req, 400, "{\"error\":{\"code\":400,\"message\":\"No boundary\"}}");
    std::string delim = std::string("\r\n--") + (b + 9);
    BodyReader r{req, req->content_len, ""};
    // First boundary has no leading CRLF
    r.buf = "\r\n";
    std::string root = "gcodes", subdir, filename;
    bool print = false;
    const std::string tmp = ".upload.part";
    auto tmpfs = srv.mr->root_fs("gcodes");
    bool have_file = false;
    std::string err;
    for (;;) {
        size_t d;
        while ((d = r.buf.find(delim)) == std::string::npos)
            if (!r.fill())
                return send_json(req, 400, "{\"error\":{\"code\":400,\"message\":\"Bad upload\"}}");
        r.buf.erase(0, d + delim.size());
        while (r.buf.size() < 2)
            if (!r.fill())
                return send_json(req, 400, "{}");
        if (r.buf.compare(0, 2, "--") == 0)
            break; // last boundary
        size_t he;
        while ((he = r.buf.find("\r\n\r\n")) == std::string::npos)
            if (!r.fill())
                return send_json(req, 400, "{}");
        std::string ph = r.buf.substr(0, he);
        r.buf.erase(0, he + 4);
        auto attr = [&](const char *key) {
            std::string k = std::string(key) + "=\"";
            size_t a = ph.find(k);
            if (a == std::string::npos)
                return std::string();
            a += k.size();
            return ph.substr(a, ph.find('"', a) - a);
        };
        std::string name = attr("name");
        if (name == "file") {
            filename = attr("filename");
            auto w = tmpfs ? tmpfs->create(tmp, &err) : nullptr;
            if (!w)
                return send_json(req, 400, "{\"error\":{\"code\":400,\"message\":"
                                               + json_dump(jstr(err)) + "}}");
            // Stream until the next delimiter, keeping a tail that may hold
            // its beginning
            for (;;) {
                size_t e = r.buf.find(delim);
                if (e != std::string::npos) {
                    if (!w->write(r.buf.data(), e))
                        return send_json(req, 400, "{}");
                    r.buf.erase(0, e);
                    break;
                }
                if (r.buf.size() > delim.size()) {
                    size_t n = r.buf.size() - delim.size();
                    if (!w->write(r.buf.data(), n))
                        return send_json(req, 400, "{}");
                    r.buf.erase(0, n);
                }
                if (!r.fill())
                    return send_json(req, 400, "{}");
            }
            if (!w->close())
                return send_json(req, 400, "{}");
            have_file = true;
        } else {
            size_t e;
            while ((e = r.buf.find(delim)) == std::string::npos)
                if (!r.fill())
                    return send_json(req, 400, "{}");
            std::string value = r.buf.substr(0, e);
            r.buf.erase(0, e);
            if (name == "root")
                root = value;
            else if (name == "path")
                subdir = value;
            else if (name == "print")
                print = value == "true";
        }
    }
    std::string rel = subdir.empty() ? filename : subdir + "/" + filename;
    auto fs = srv.mr->root_fs(root);
    if (!have_file || !fs || filename.empty() || !safe_relative_path(rel))
        return send_json(req, 400, "{\"error\":{\"code\":400,\"message\":\"Bad upload\"}}");
    if (fs == tmpfs) {
        fs->remove(rel, &err);
        if (!fs->rename(tmp, rel, &err))
            return send_json(req, 400, "{}");
    } else {
        auto in = tmpfs->open(tmp);
        auto out = fs->create(rel, &err);
        if (!in || !out)
            return send_json(req, 400, "{}");
        std::unique_ptr<char[]> buf(new char[CHUNK]);
        size_t n;
        while ((n = in->read(buf.get(), CHUNK)) > 0)
            out->write(buf.get(), n);
        out->close();
        tmpfs->remove(tmp, &err);
    }
    return send_json(req, 201, srv.mr->upload_done(root, rel, print));
}

esp_err_t
handle_any(httpd_req_t *req)
{
    esp_err_t hooked;
    if (srv.cfg.hook && srv.cfg.hook(req, &hooked))
        return hooked;
    std::string uri = req->uri;
    std::string path = uri.substr(0, uri.find('?'));
    if (req->method == HTTP_OPTIONS) {
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, DELETE");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "*");
        httpd_resp_set_status(req, "204 No Content");
        return httpd_resp_send(req, nullptr, 0);
    }
    if (req->method == HTTP_POST && path == "/server/files/upload")
        return handle_upload(req);
    if (req->method == HTTP_GET && path.compare(0, 14, "/server/files/") == 0
        && path.find('/', 14) != std::string::npos)
        return serve_download(req, path);
    bool api = path.compare(0, 8, "/server/") == 0
               || path.compare(0, 9, "/printer/") == 0
               || path.compare(0, 9, "/machine/") == 0
               || path.compare(0, 8, "/access/") == 0;
    if (!api)
        return serve_static(req, url_decode(path));
    std::string query;
    size_t qlen = httpd_req_get_url_query_len(req);
    if (qlen) {
        query.resize(qlen + 1);
        httpd_req_get_url_query_str(req, &query[0], qlen + 1);
        query.resize(qlen);
    }
    std::string body;
    if (req->content_len && req->content_len < 65536) {
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
    char ct[64] = "";
    httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof(ct));
    JValue params = Moonraker::http_params(path, query, body,
                                           strstr(ct, "json") != nullptr);
    std::string out;
    int status = srv.mr->http_api(path, params, &out);
    return send_json(req, status, out);
}

esp_err_t
handle_ws(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    if (req->method == HTTP_GET) {
        // Handshake done by the server
        if (!srv.mr->open(fd))
            return ESP_FAIL;
        return ESP_OK;
    }
    httpd_ws_frame_t frame = {};
    if (httpd_ws_recv_frame(req, &frame, 0) != ESP_OK)
        return ESP_FAIL;
    std::string payload(frame.len, '\0');
    if (frame.len) {
        frame.payload = (uint8_t *)&payload[0];
        if (httpd_ws_recv_frame(req, &frame, frame.len) != ESP_OK)
            return ESP_FAIL;
    }
    if (frame.type != HTTPD_WS_TYPE_TEXT && frame.type != HTTPD_WS_TYPE_CONTINUE)
        return ESP_OK;
    std::string msg;
    {
        std::lock_guard<std::mutex> g(srv.frag_lock);
        std::string &f = srv.frags[fd];
        f += payload;
        if (!frame.final)
            return ESP_OK;
        msg.swap(f);
        srv.frags.erase(fd);
    }
    srv.mr->message(fd, msg);
    return ESP_OK;
}

void
on_close(httpd_handle_t, int fd)
{
    srv.mr->close(fd);
    {
        std::lock_guard<std::mutex> g(srv.frag_lock);
        srv.frags.erase(fd);
    }
    close(fd);
}

struct OutMsg {
    int fd;
    std::string text;
};

void
send_work(void *arg)
{
    std::unique_ptr<OutMsg> m((OutMsg *)arg);
    if (httpd_ws_get_fd_info(srv.hd, m->fd) != HTTPD_WS_CLIENT_WEBSOCKET)
        return;
    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.final = true;
    frame.payload = (uint8_t *)m->text.data();
    frame.len = m->text.size();
    httpd_ws_send_frame_async(srv.hd, m->fd, &frame);
}

void
tick_task(void *)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(250));
        srv.mr->tick();
    }
}

} // namespace

bool
esp_server_start(Moonraker &mr, const EspServerConfig &cfg, std::string *err)
{
    srv.mr = &mr;
    srv.cfg = cfg;
    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.server_port = cfg.port;
    hc.max_open_sockets = cfg.max_sockets;
    hc.stack_size = cfg.stack;
    hc.task_priority = cfg.priority;
    hc.core_id = cfg.core;
    hc.lru_purge_enable = true;
    hc.close_fn = on_close;
    hc.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&srv.hd, &hc) != ESP_OK) {
        *err = "httpd_start failed";
        return false;
    }
    httpd_uri_t ws = {};
    ws.uri = "/websocket";
    ws.method = HTTP_GET;
    ws.handler = handle_ws;
    ws.is_websocket = true;
    httpd_register_uri_handler(srv.hd, &ws);
    for (httpd_method_t m : {HTTP_GET, HTTP_POST, HTTP_OPTIONS, HTTP_DELETE}) {
        httpd_uri_t any = {};
        any.uri = "/*";
        any.method = m;
        any.handler = handle_any;
        httpd_register_uri_handler(srv.hd, &any);
    }
    mr.set_sender([](int fd, const std::string &text) {
        auto *m = new OutMsg{fd, text};
        if (httpd_queue_work(srv.hd, send_work, m) != ESP_OK)
            delete m;
    });
    xTaskCreatePinnedToCore(tick_task, "moonraker", 6144, nullptr,
                            cfg.priority, nullptr, cfg.core);
    return true;
}

} // namespace kl
