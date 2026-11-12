// Moonraker-compatible API for Mainsail: JSON-RPC over WebSocket and the
// HTTP endpoints it needs, served from the Printer in the same process.
//
// This class is independent of the HTTP server.  A server (posix_server.cpp
// on the host, the ESP-IDF one on the board) passes WebSocket messages and
// HTTP requests to it and gives it a function that sends text to a
// WebSocket connection (callable from any task).  Call tick() a few times
// per second: it sends status updates and G-code responses.
//
// This file may be distributed under the terms of the GNU GPLv3 license.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "klippy/files.hpp"
#include "klippy/jinja.hpp"

namespace kl {

class Printer;

class Moonraker {
public:
    struct Options {
        std::string hostname = "esp32klipper";
        std::string cpu = "ESP32";
        std::string klipper_version = "v0.13.0-esp32klipper";
        std::string moonraker_version = "v0.9.3-esp32klipper";
        size_t max_connections = 4;
        // Mainsail settings (Moonraker database), kept as JSON in the
        // config root; empty = in memory only
        std::string database_file = ".moonraker_db.json";
        // Total / free RAM in bytes for machine.proc_stats (optional)
        std::function<void(uint64_t *total, uint64_t *avail)> memory_info;
        // Restart the host program / board for printer.restart and
        // printer.firmware_restart (the MCU is reset first in both cases:
        // it is configured again after the restart).  Not set = not
        // supported.
        std::function<void()> restart;
    };
    using Sender = std::function<void(int conn, const std::string &text)>;

    Moonraker(Printer &p, std::shared_ptr<FileSystem> gcodes,
              std::shared_ptr<FileSystem> config, Options opt);
    ~Moonraker();

    void set_sender(Sender s) { sender_ = std::move(s); }
    // WebSocket connections (ids from the server)
    bool open(int conn); // false = too many connections
    void close(int conn);
    void message(int conn, const std::string &text);
    void tick();

    // HTTP JSON API: GET/POST /<a>/<b>/<c> = method "a.b.c" with the query
    // (and JSON body) as params.  Returns the HTTP status; *body = JSON.
    int http_api(const std::string &path, const JValue &params,
                 std::string *body);
    // Params of an HTTP API request from its query string and body
    // (/printer/objects/query?toolhead&webhooks=state -> {"objects": ...})
    static JValue http_params(const std::string &path,
                              const std::string &query,
                              const std::string &body, bool json_body);
    // Files for HTTP download/upload ("gcodes" or "config")
    std::shared_ptr<FileSystem> root_fs(const std::string &root);
    // After an HTTP upload into root/path: notify clients, maybe print.
    // Returns the JSON body of the reply.
    std::string upload_done(const std::string &root, const std::string &path,
                            bool start_print);
    size_t connection_count();

private:
    struct Conn {
        int id;
        std::map<std::string, std::vector<std::string>> subs;
        std::map<std::string, std::map<std::string, std::string>> sent;
        std::string client_name;
    };
    struct Request {
        int conn;
        JValue id;
        std::string method;
        JValue params;
    };
    using Reply = std::function<void(bool ok, JValue result, int code,
                                     const std::string &err)>;

    void dispatch(const std::string &method, const JValue &params, int conn,
                  Reply reply);
    void send(int conn, const std::string &text);
    void broadcast(const std::string &method, const JValue &params);
    JValue query_objects(const JValue &objects, Conn *store);
    JValue status_fields(const std::string &obj,
                         const std::vector<std::string> &fields);
    JValue file_item(const std::string &root, const FileInfo &fi);
    bool split_root(const std::string &path, std::string *root,
                    std::string *rel);
    void db_load();
    void db_save();
    void record_history(const JValue &print_stats);

    Printer &p_;
    std::shared_ptr<FileSystem> gcodes_, config_;
    Options opt_;
    Sender sender_;
    std::mutex lock_; // conns_, gcode_queue_, db_, history_
    std::map<int, Conn> conns_;
    std::deque<std::string> gcode_queue_, gcode_store_;
    JValue db_;
    std::vector<JValue> history_;
    uint32_t next_job_ = 1;
    std::string last_print_state_, last_klippy_state_;
    double start_time_;
    // Objects that do not change while running (sent once)
    std::map<std::string, std::string> static_cache_;
};

} // namespace kl
