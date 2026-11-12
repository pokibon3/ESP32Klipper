// HTTP + WebSocket server for the Moonraker API on a POSIX host (see
// posix_server.hpp).
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "moonraker/posix_server.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "moonraker/json.hpp"
#include "moonraker/moonraker.hpp"
#include "moonraker/sha1.hpp"

namespace kl {

struct PosixServer::Client {
    int fd = -1;
    int id = 0;
    std::string in, out; // out under PosixServer::lock_
    bool ws = false;
    bool close_after_write = false;
    bool closed = false;
    std::string frag; // WebSocket message being assembled
};

namespace {

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

std::string
lower(std::string s)
{
    for (auto &c : s)
        c = (char)tolower((unsigned char)c);
    return s;
}

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
    if (ends(".ttf")) return "font/ttf";
    return "application/octet-stream";
}

bool
read_file(const std::string &path, std::string *out)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    char buf[8192];
    size_t n;
    out->clear();
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        out->append(buf, n);
    fclose(f);
    return true;
}

bool
is_file(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

const char *
status_text(int s)
{
    switch (s) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default: return "Error";
    }
}

} // namespace

PosixServer::PosixServer(Moonraker &mr, int port, std::string www_dir)
    : mr_(mr), port_(port), www_(std::move(www_dir))
{
}

PosixServer::~PosixServer()
{
    stop();
}

bool
PosixServer::start(std::string *err)
{
    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port_);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(listen_fd_, (sockaddr *)&a, sizeof(a)) != 0
        || listen(listen_fd_, 16) != 0) {
        *err = std::string("bind/listen: ") + strerror(errno);
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    int p[2];
    if (pipe(p) != 0) {
        *err = "pipe failed";
        return false;
    }
    wake_rd_ = p[0];
    wake_wr_ = p[1];
    fcntl(wake_rd_, F_SETFL, O_NONBLOCK);
    fcntl(wake_wr_, F_SETFL, O_NONBLOCK);
    mr_.set_sender([this](int conn, const std::string &text) {
        std::lock_guard<std::mutex> g(lock_);
        auto it = clients_.find(conn);
        if (it == clients_.end() || it->second->closed)
            return;
        // Text frame (server frames are not masked)
        std::string &o = it->second->out;
        o += (char)0x81;
        size_t n = text.size();
        if (n < 126) {
            o += (char)n;
        } else if (n < 65536) {
            o += (char)126;
            o += (char)(n >> 8);
            o += (char)n;
        } else {
            o += (char)127;
            for (int i = 7; i >= 0; i--)
                o += (char)((uint64_t)n >> (i * 8));
        }
        o += text;
        char b = 1;
        (void)!write(wake_wr_, &b, 1);
    });
    thread_ = std::thread([this] { run(); });
    return true;
}

void
PosixServer::stop()
{
    if (!thread_.joinable())
        return;
    stop_ = true;
    wake();
    thread_.join();
    mr_.set_sender(nullptr);
}

void
PosixServer::wake()
{
    char b = 1;
    (void)!write(wake_wr_, &b, 1);
}

void
PosixServer::respond(Client &c, int status, const std::string &type,
                     const std::string &body, const std::string &extra)
{
    char head[512];
    snprintf(head, sizeof(head),
             "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
             "Access-Control-Allow-Origin: *\r\nCache-Control: no-cache\r\n"
             "Connection: close\r\n%s\r\n",
             status, status_text(status), type.c_str(), body.size(),
             extra.c_str());
    std::lock_guard<std::mutex> g(lock_);
    c.out += head;
    c.out += body;
    c.close_after_write = true;
}

void
PosixServer::serve_static(Client &c, const std::string &path)
{
    std::string p = path == "/" ? "/index.html" : path;
    if (p.find("..") != std::string::npos)
        return respond(c, 403, "text/plain", "Forbidden");
    std::string file = www_ + p, body;
    std::string extra;
    if (is_file(file + ".gz")) {
        read_file(file + ".gz", &body);
        extra = "Content-Encoding: gzip\r\n";
    } else if (!read_file(file, &body)) {
        // Single-page app routes
        if (p.rfind('.') == std::string::npos || p.rfind('.') < p.rfind('/')) {
            if (!read_file(www_ + "/index.html", &body))
                return respond(c, 404, "text/plain",
                               "Mainsail is not installed in " + www_);
            return respond(c, 200, content_type(".html"), body);
        }
        return respond(c, 404, "text/plain", "Not found");
    }
    respond(c, 200, content_type(p), body, extra);
}

void
PosixServer::serve_download(Client &c, const std::string &path)
{
    // /server/files/<root>/<path>
    std::string rest = url_decode(path.substr(strlen("/server/files/")));
    size_t s = rest.find('/');
    std::string root = rest.substr(0, s);
    std::string rel = s == std::string::npos ? "" : rest.substr(s + 1);
    auto fs = mr_.root_fs(root);
    std::unique_ptr<GcodeFile> f;
    if (fs && safe_relative_path(rel))
        f = fs->open(rel);
    if (!f)
        return respond(c, 404, "application/json",
                       "{\"error\":{\"code\":404,\"message\":\"File not found\"}}");
    std::string body(f->size(), '\0');
    body.resize(f->read(&body[0], body.size()));
    respond(c, 200, "application/octet-stream", body);
}

void
PosixServer::handle_upload(Client &c)
{
    size_t he = c.in.find("\r\n\r\n");
    std::string headers = lower(c.in.substr(0, he));
    size_t bp = headers.find("boundary=");
    if (bp == std::string::npos)
        return respond(c, 400, "text/plain", "No boundary");
    // Boundary from the original (case-sensitive) headers
    std::string orig = c.in.substr(0, he);
    std::string boundary = orig.substr(bp + 9, orig.find("\r\n", bp) - bp - 9);
    if (!boundary.empty() && boundary[0] == '"')
        boundary = boundary.substr(1, boundary.size() - 2);
    std::string body = c.in.substr(he + 4);
    std::string delim = "--" + boundary;
    std::string root = "gcodes", subdir, filename, data;
    bool print = false;
    size_t pos = body.find(delim);
    while (pos != std::string::npos) {
        pos += delim.size();
        if (body.compare(pos, 2, "--") == 0)
            break;
        pos += 2; // CRLF
        size_t hend = body.find("\r\n\r\n", pos);
        if (hend == std::string::npos)
            break;
        std::string ph = body.substr(pos, hend - pos);
        size_t next = body.find("\r\n" + delim, hend + 4);
        if (next == std::string::npos)
            break;
        std::string value = body.substr(hend + 4, next - hend - 4);
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
            data = std::move(value);
        } else if (name == "root") {
            root = value;
        } else if (name == "path") {
            subdir = value;
        } else if (name == "print") {
            print = value == "true";
        }
        pos = next + 2;
    }
    std::string rel = subdir.empty() ? filename : subdir + "/" + filename;
    auto fs = mr_.root_fs(root);
    std::string err;
    if (!fs || filename.empty() || !safe_relative_path(rel))
        return respond(c, 400, "application/json",
                       "{\"error\":{\"code\":400,\"message\":\"Bad upload\"}}");
    auto w = fs->create(rel, &err);
    if (!w || !w->write(data.data(), data.size()) || !w->close())
        return respond(c, 400, "application/json",
                       "{\"error\":{\"code\":400,\"message\":"
                           + json_dump(jstr(err.empty() ? "Write error" : err))
                           + "}}");
    respond(c, 201, "application/json", mr_.upload_done(root, rel, print));
}

void
PosixServer::handle_http(Client &c)
{
    size_t he = c.in.find("\r\n\r\n");
    if (he == std::string::npos)
        return;
    std::string head = c.in.substr(0, he);
    std::string lh = lower(head);
    size_t clen = 0;
    size_t cp = lh.find("content-length:");
    if (cp != std::string::npos)
        clen = strtoul(lh.c_str() + cp + 15, nullptr, 10);
    if (c.in.size() < he + 4 + clen)
        return; // body not complete yet
    size_t sp1 = head.find(' '), sp2 = head.find(' ', sp1 + 1);
    std::string method = head.substr(0, sp1);
    std::string target = head.substr(sp1 + 1, sp2 - sp1 - 1);
    std::string path = target.substr(0, target.find('?'));
    std::string query = target.find('?') == std::string::npos
                            ? "" : target.substr(target.find('?') + 1);
    std::string body = c.in.substr(he + 4, clen);

    if (method == "OPTIONS")
        return respond(c, 204, "text/plain", "",
                       "Access-Control-Allow-Methods: GET, POST, DELETE\r\n"
                       "Access-Control-Allow-Headers: *\r\n");
    if (path == "/websocket") {
        size_t kp = lh.find("sec-websocket-key:");
        if (kp == std::string::npos)
            return respond(c, 400, "text/plain", "Not a WebSocket request");
        std::string key = head.substr(kp + 18, head.find("\r\n", kp) - kp - 18);
        while (!key.empty() && key[0] == ' ')
            key.erase(0, 1);
        if (!mr_.open(c.id))
            return respond(c, 503, "text/plain", "Too many connections");
        {
            std::lock_guard<std::mutex> g(lock_);
            c.out += "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                     "Connection: Upgrade\r\nSec-WebSocket-Accept: "
                     + websocket_accept(key) + "\r\n\r\n";
            c.ws = true;
        }
        c.in.erase(0, he + 4);
        return;
    }
    if (method == "POST" && path == "/server/files/upload")
        return handle_upload(c);
    if (method == "GET" && path.compare(0, 14, "/server/files/") == 0
        && path.find('/', 14) != std::string::npos)
        return serve_download(c, path);
    bool api = path.compare(0, 8, "/server/") == 0
               || path.compare(0, 9, "/printer/") == 0
               || path.compare(0, 9, "/machine/") == 0
               || path.compare(0, 8, "/access/") == 0;
    if (!api)
        return serve_static(c, url_decode(path));

    JValue params = Moonraker::http_params(
        path, query, body, lh.find("application/json") != std::string::npos);
    std::string out;
    int status = mr_.http_api(path, params, &out);
    respond(c, status, "application/json", out);
}

void
PosixServer::handle_ws(Client &c)
{
    for (;;) {
        const std::string &in = c.in;
        if (in.size() < 2)
            return;
        uint8_t b0 = in[0], b1 = in[1];
        bool fin = b0 & 0x80;
        int op = b0 & 0x0f;
        bool masked = b1 & 0x80;
        uint64_t len = b1 & 0x7f;
        size_t hdr = 2;
        if (len == 126) {
            if (in.size() < 4)
                return;
            len = (uint8_t)in[2] << 8 | (uint8_t)in[3];
            hdr = 4;
        } else if (len == 127) {
            if (in.size() < 10)
                return;
            len = 0;
            for (int i = 0; i < 8; i++)
                len = len << 8 | (uint8_t)in[2 + i];
            hdr = 10;
        }
        size_t mask_at = hdr;
        if (masked)
            hdr += 4;
        if (in.size() < hdr + len)
            return;
        std::string payload = in.substr(hdr, len);
        if (masked)
            for (size_t i = 0; i < payload.size(); i++)
                payload[i] ^= in[mask_at + (i & 3)];
        c.in.erase(0, hdr + len);
        if (op == 0x8) { // close
            send_ws(c, 0x8, "");
            std::lock_guard<std::mutex> g(lock_);
            c.close_after_write = true;
            return;
        }
        if (op == 0x9) { // ping
            send_ws(c, 0xA, payload);
            continue;
        }
        if (op == 0xA)
            continue;
        c.frag += payload;
        if (!fin)
            continue;
        std::string msg;
        msg.swap(c.frag);
        mr_.message(c.id, msg);
    }
}

void
PosixServer::send_ws(Client &c, int opcode, const std::string &payload)
{
    std::lock_guard<std::mutex> g(lock_);
    c.out += (char)(0x80 | opcode);
    c.out += (char)payload.size(); // control frames < 126 bytes
    c.out += payload;
}

void
PosixServer::run()
{
    using clock = std::chrono::steady_clock;
    auto next_tick = clock::now();
    std::vector<Client *> list;
    while (!stop_) {
        std::vector<pollfd> fds;
        fds.push_back({listen_fd_, POLLIN, 0});
        fds.push_back({wake_rd_, POLLIN, 0});
        {
            std::lock_guard<std::mutex> g(lock_);
            list.clear();
            for (auto &kv : clients_) {
                Client *c = kv.second;
                list.push_back(c);
                short ev = POLLIN;
                if (!c->out.empty())
                    ev |= POLLOUT;
                fds.push_back({c->fd, ev, 0});
            }
        }
        int wait_ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                          next_tick - clock::now()).count();
        if (wait_ms < 0)
            wait_ms = 0;
        poll(fds.data(), fds.size(), wait_ms);
        if (fds[1].revents & POLLIN) {
            char buf[64];
            while (read(wake_rd_, buf, sizeof(buf)) > 0) {
            }
        }
        if (fds[0].revents & POLLIN) {
            int fd = accept(listen_fd_, nullptr, nullptr);
            if (fd >= 0) {
                fcntl(fd, F_SETFL, O_NONBLOCK);
                int one = 1;
                setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
                auto *c = new Client;
                c->fd = fd;
                std::lock_guard<std::mutex> g(lock_);
                c->id = next_id_++;
                clients_[c->id] = c;
            }
        }
        for (size_t i = 0; i < list.size(); i++) {
            Client &c = *list[i];
            short re = fds[i + 2].revents;
            if (re & (POLLIN | POLLHUP | POLLERR)) {
                char buf[16384];
                ssize_t n = read(c.fd, buf, sizeof(buf));
                if (n > 0) {
                    c.in.append(buf, n);
                    if (c.ws)
                        handle_ws(c);
                    else
                        handle_http(c);
                } else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                    c.closed = true;
                }
            }
            std::lock_guard<std::mutex> g(lock_);
            if (!c.out.empty() && !c.closed) {
                ssize_t n = write(c.fd, c.out.data(), c.out.size());
                if (n > 0)
                    c.out.erase(0, n);
                else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
                    c.closed = true;
            }
            if (c.close_after_write && c.out.empty())
                c.closed = true;
        }
        // Drop closed connections
        std::vector<Client *> dead;
        {
            std::lock_guard<std::mutex> g(lock_);
            for (auto it = clients_.begin(); it != clients_.end();) {
                if (it->second->closed) {
                    dead.push_back(it->second);
                    it = clients_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (Client *c : dead) {
            if (c->ws)
                mr_.close(c->id);
            ::close(c->fd);
            delete c;
        }
        if (clock::now() >= next_tick) {
            mr_.tick();
            next_tick = clock::now() + std::chrono::milliseconds(250);
        }
    }
    std::lock_guard<std::mutex> g(lock_);
    for (auto &kv : clients_) {
        ::close(kv.second->fd);
        delete kv.second;
    }
    clients_.clear();
    ::close(listen_fd_);
}

} // namespace kl
