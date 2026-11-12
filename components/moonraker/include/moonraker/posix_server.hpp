// HTTP + WebSocket server for the Moonraker API on a POSIX host (Mac,
// Linux): serves Mainsail's files from a directory, /websocket and the
// HTTP API.  One thread; development use (no TLS, no authentication).
//
// This file may be distributed under the terms of the GNU GPLv3 license.
#pragma once

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace kl {

class Moonraker;

class PosixServer {
public:
    PosixServer(Moonraker &mr, int port, std::string www_dir);
    ~PosixServer();
    bool start(std::string *err);
    void stop();

private:
    struct Client;
    void run();
    void handle_http(Client &c);
    void handle_ws(Client &c);
    void send_ws(Client &c, int opcode, const std::string &payload);
    void respond(Client &c, int status, const std::string &type,
                 const std::string &body, const std::string &extra = "");
    void serve_static(Client &c, const std::string &path);
    void serve_download(Client &c, const std::string &path);
    void handle_upload(Client &c);
    void wake();

    Moonraker &mr_;
    int port_;
    std::string www_;
    int listen_fd_ = -1, wake_rd_ = -1, wake_wr_ = -1;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::mutex lock_; // clients_' out buffers
    std::map<int, Client *> clients_; // by connection id
    int next_id_ = 1;
};

} // namespace kl
