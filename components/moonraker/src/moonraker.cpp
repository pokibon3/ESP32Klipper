// Moonraker-compatible API (see moonraker.hpp).
//
// Method list and reply shapes follow Moonraker's documentation
// (docs/web_api.md) for what Mainsail uses.  Methods Mainsail calls for
// features this port does not have (webcams, update manager, power
// devices, ...) answer with empty results.
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "moonraker/moonraker.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "klippy/printer.hpp"
#include "moonraker/json.hpp"

namespace kl {

namespace {

constexpr int ERR_NOT_FOUND = 404, ERR_BAD = 400, ERR_METHOD = -32601;

double
wall_time()
{
    return (double)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
               .count()
           * 1e-3;
}

std::string
quote_filename(const std::string &f)
{
    // SDCARD_PRINT_FILE FILENAME="..." (quotes keep spaces)
    std::string q = "\"";
    for (char c : f)
        if (c != '"')
            q += c;
    return q + "\"";
}

JValue
list_of(const std::vector<std::string> &v)
{
    JList l;
    for (const auto &s : v)
        l.push_back(jstr(s));
    return JValue::make_list(std::move(l));
}

// Objects that do not change while the printer runs
bool
is_static_object(const std::string &o)
{
    return o == "configfile" || o == "gcode";
}

// Moonraker database keys are "a.b.c" paths into the namespace
JValue *
db_walk(JValue &root, const std::string &key, bool create)
{
    JValue *cur = &root;
    size_t pos = 0;
    while (pos <= key.size()) {
        size_t e = key.find('.', pos);
        if (e == std::string::npos)
            e = key.size();
        std::string part = key.substr(pos, e - pos);
        if (cur->t != JValue::T::Dict) {
            if (!create)
                return nullptr;
            *cur = JValue::make_dict();
        }
        JValue *next = cur->dict->find(part);
        if (!next) {
            if (!create)
                return nullptr;
            cur->dict->set(part, JValue::make_dict());
            next = cur->dict->find(part);
        }
        cur = next;
        pos = e + 1;
    }
    return cur;
}

} // namespace

Moonraker::Moonraker(Printer &p, std::shared_ptr<FileSystem> gcodes,
                     std::shared_ptr<FileSystem> config, Options opt)
    : p_(p), gcodes_(std::move(gcodes)), config_(std::move(config)),
      opt_(std::move(opt)), start_time_(wall_time())
{
    db_ = JValue::make_dict();
    db_load();
    p_.add_response_listener([this](const std::string &m) {
        std::lock_guard<std::mutex> g(lock_);
        gcode_queue_.push_back(m);
        if (gcode_queue_.size() > 200)
            gcode_queue_.pop_front();
    });
}

Moonraker::~Moonraker() = default;

size_t
Moonraker::connection_count()
{
    std::lock_guard<std::mutex> g(lock_);
    return conns_.size();
}

bool
Moonraker::open(int conn)
{
    std::lock_guard<std::mutex> g(lock_);
    if (conns_.size() >= opt_.max_connections)
        return false;
    conns_[conn].id = conn;
    return true;
}

void
Moonraker::close(int conn)
{
    std::lock_guard<std::mutex> g(lock_);
    conns_.erase(conn);
}

void
Moonraker::send(int conn, const std::string &text)
{
    if (sender_)
        sender_(conn, text);
}

void
Moonraker::broadcast(const std::string &method, const JValue &params)
{
    std::string msg = "{\"jsonrpc\":\"2.0\",\"method\":";
    json_write_string(msg, method);
    msg += ",\"params\":";
    json_write(msg, params);
    msg += '}';
    std::vector<int> ids;
    {
        std::lock_guard<std::mutex> g(lock_);
        for (auto &c : conns_)
            ids.push_back(c.first);
    }
    for (int id : ids)
        send(id, msg);
}

/****************************************************************
 * JSON-RPC
 ****************************************************************/

void
Moonraker::message(int conn, const std::string &text)
{
    JValue req;
    std::string err;
    if (!json_load(text, &req, &err) || req.t != JValue::T::Dict) {
        send(conn, "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32700,"
                   "\"message\":\"Parse error\"},\"id\":null}");
        return;
    }
    std::string method = jget_str(req, "method");
    const JValue *idp = jget(req, "id");
    JValue id = idp ? *idp : JValue::none();
    const JValue *pp = jget(req, "params");
    JValue params = pp ? *pp : JValue::make_dict();
    bool has_id = idp != nullptr;
    dispatch(method, params, conn,
             [this, conn, id, has_id](bool ok, JValue result, int code,
                                      const std::string &e) {
                 if (!has_id)
                     return;
                 std::string out = "{\"jsonrpc\":\"2.0\",";
                 if (ok) {
                     out += "\"result\":";
                     json_write(out, result);
                 } else {
                     out += "\"error\":{\"code\":" + std::to_string(code)
                            + ",\"message\":";
                     json_write_string(out, e);
                     out += '}';
                 }
                 out += ",\"id\":";
                 json_write(out, id);
                 out += '}';
                 send(conn, out);
             });
}

int
Moonraker::http_api(const std::string &path, const JValue &params,
                    std::string *body)
{
    // "/printer/objects/query" -> "printer.objects.query"
    std::string method;
    for (char c : path.substr(1))
        method += c == '/' ? '.' : c;
    int status = 200;
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    dispatch(method, params, -1,
             [&](bool ok, JValue result, int code, const std::string &e) {
                 std::lock_guard<std::mutex> g(m);
                 if (ok) {
                     *body = "{\"result\":" + json_dump(result) + "}";
                 } else {
                     status = code > 0 ? code : (code == ERR_METHOD ? 404 : 400);
                     *body = "{\"error\":{\"code\":" + std::to_string(status)
                             + ",\"message\":" + json_dump(jstr(e)) + "}}";
                 }
                 done = true;
                 cv.notify_all();
             });
    std::unique_lock<std::mutex> g(m);
    cv.wait_for(g, std::chrono::seconds(60), [&] { return done; });
    if (!done) {
        status = 504;
        *body = "{\"error\":{\"code\":504,\"message\":\"Timeout\"}}";
    }
    return status;
}

namespace {
std::string
url_unescape(const std::string &s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size()) {
            out += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else {
            out += s[i] == '+' ? ' ' : s[i];
        }
    }
    return out;
}
} // namespace

JValue
Moonraker::http_params(const std::string &path, const std::string &query,
                       const std::string &body, bool json_body)
{
    JValue params = JValue::make_dict();
    bool objects = path == "/printer/objects/query"
                   || path == "/printer/objects/subscribe";
    JValue objs = JValue::make_dict();
    size_t pos = 0;
    while (pos < query.size()) {
        size_t e = query.find('&', pos);
        if (e == std::string::npos)
            e = query.size();
        std::string kv = query.substr(pos, e - pos);
        size_t eq = kv.find('=');
        std::string k = url_unescape(kv.substr(0, eq));
        std::string v = eq == std::string::npos ? ""
                                                : url_unescape(kv.substr(eq + 1));
        if (objects) {
            if (v.empty()) {
                objs.dict->set(k, JValue::none());
            } else {
                JList fields;
                size_t fp = 0;
                while (fp <= v.size()) {
                    size_t fe = v.find(',', fp);
                    if (fe == std::string::npos)
                        fe = v.size();
                    fields.push_back(jstr(v.substr(fp, fe - fp)));
                    fp = fe + 1;
                }
                objs.dict->set(k, JValue::make_list(std::move(fields)));
            }
        } else {
            params.dict->set(k, jstr(v));
        }
        pos = e + 1;
    }
    if (objects)
        params.dict->set("objects", objs);
    if (json_body && !body.empty()) {
        JValue b;
        std::string err;
        if (json_load(body, &b, &err) && b.t == JValue::T::Dict)
            for (const auto &kv : b.dict->items)
                params.dict->set(kv.first, kv.second);
    }
    return params;
}

std::shared_ptr<FileSystem>
Moonraker::root_fs(const std::string &root)
{
    if (root == "gcodes")
        return gcodes_;
    if (root == "config")
        return config_;
    return nullptr;
}

bool
Moonraker::split_root(const std::string &path, std::string *root,
                      std::string *rel)
{
    std::string p = path;
    while (!p.empty() && p[0] == '/')
        p.erase(0, 1);
    while (!p.empty() && p.back() == '/')
        p.pop_back();
    size_t s = p.find('/');
    *root = p.substr(0, s);
    *rel = s == std::string::npos ? "" : p.substr(s + 1);
    return root_fs(*root) && (rel->empty() || safe_relative_path(*rel));
}

JValue
Moonraker::file_item(const std::string &root, const FileInfo &fi)
{
    auto fs = root_fs(root);
    std::string perm = fs && fs->writable() ? "rw" : "r";
    return jdict({{"path", jstr(fi.name)},
                  {"root", jstr(root)},
                  {"modified", jnum(fi.modified)},
                  {"size", jint((int64_t)fi.size)},
                  {"permissions", jstr(perm)}});
}

JValue
Moonraker::status_fields(const std::string &obj,
                         const std::vector<std::string> &fields)
{
    JValue st = p_.get_status(obj);
    if (st.t != JValue::T::Dict)
        return JValue::make_dict();
    if (fields.empty())
        return st;
    JValue out = JValue::make_dict();
    for (const auto &f : fields)
        if (const JValue *v = st.dict->find(f))
            out.dict->set(f, *v);
    return out;
}

JValue
Moonraker::query_objects(const JValue &objects, Conn *store)
{
    // objects: {"name": null | ["field", ...]}
    std::vector<std::pair<std::string, std::vector<std::string>>> want;
    if (objects.t == JValue::T::Dict)
        for (const auto &kv : objects.dict->items) {
            std::vector<std::string> fields;
            if (kv.second.t == JValue::T::List)
                for (const auto &f : *kv.second.list)
                    fields.push_back(f.to_str());
            want.emplace_back(kv.first, fields);
        }
    JValue status = JValue::make_dict();
    double eventtime = 0.;
    p_.run_on_klippy([&] {
        eventtime = p_.monotonic();
        auto names = p_.object_names();
        std::set<std::string> known(names.begin(), names.end());
        for (const auto &w : want)
            if (known.count(w.first))
                status.dict->set(w.first, status_fields(w.first, w.second));
    });
    if (store) {
        store->subs.clear();
        store->sent.clear();
        for (const auto &w : want)
            store->subs[w.first] = w.second;
        for (const auto &kv : status.dict->items)
            if (kv.second.t == JValue::T::Dict)
                for (const auto &f : kv.second.dict->items)
                    store->sent[kv.first][f.first] = json_dump(f.second);
    }
    return jdict({{"eventtime", jnum(eventtime)}, {"status", status}});
}

void
Moonraker::dispatch(const std::string &method, const JValue &params, int conn,
                    Reply reply)
{
    auto ok = [&](JValue r) { reply(true, std::move(r), 0, ""); };
    auto fail = [&](int code, const std::string &e) {
        reply(false, JValue::none(), code, e);
    };
    auto run_gcode = [&](const std::string &script) {
        p_.submit_script(script, [reply](bool good, const std::string &e) {
            if (good)
                reply(true, jstr("ok"), 0, "");
            else
                reply(false, JValue::none(), ERR_BAD, e);
        });
    };

    /* Connection, server */
    if (method == "server.connection.identify") {
        std::lock_guard<std::mutex> g(lock_);
        if (conns_.count(conn))
            conns_[conn].client_name = jget_str(params, "client_name");
        ok(jdict({{"connection_id", jint(conn)}}));
    } else if (method == "server.websocket.id") {
        ok(jdict({{"websocket_id", jint(conn)}}));
    } else if (method == "server.info") {
        std::string st;
        p_.run_on_klippy([&] { st = p_.klippy_state(); });
        ok(jdict({{"klippy_connected", jbool(true)},
                  {"klippy_state", jstr(st)},
                  {"components",
                   list_of({"database", "file_manager", "klippy_apis",
                            "machine", "data_store", "history",
                            "webcam", "announcements"})},
                  {"failed_components", JValue::make_list()},
                  {"registered_directories", list_of({"config", "gcodes"})},
                  {"warnings", JValue::make_list()},
                  {"websocket_count", jint((int64_t)connection_count())},
                  {"moonraker_version", jstr(opt_.moonraker_version)},
                  {"missing_klippy_requirements", JValue::make_list()},
                  {"api_version",
                   JValue::make_list({jint(1), jint(5), jint(0)})},
                  {"api_version_string", jstr("1.5.0")}}));
    } else if (method == "server.config") {
        JValue cfg = jdict(
            {{"server", jdict({{"host", jstr("0.0.0.0")}, {"port", jint(80)}})},
             {"file_manager",
              jdict({{"enable_object_processing", jbool(false)},
                     {"queue_gcode_uploads", jbool(false)},
                     {"enable_inotify_warnings", jbool(false)}})},
             {"history", JValue::make_dict()}});
        ok(jdict({{"config", cfg}, {"orig", cfg}, {"files", JValue::make_list()}}));
    } else if (method == "server.restart" || method == "machine.reboot"
               || method == "machine.shutdown") {
        fail(ERR_BAD, "Not supported on this host");
    } else if (method == "server.temperature_store") {
        ok(JValue::make_dict());
    } else if (method == "server.gcode_store") {
        JList l;
        {
            std::lock_guard<std::mutex> g(lock_);
            for (const auto &m : gcode_store_)
                l.push_back(jdict({{"message", jstr(m)},
                                   {"time", jnum(wall_time())},
                                   {"type", jstr("response")}}));
        }
        ok(jdict({{"gcode_store", JValue::make_list(std::move(l))}}));
    } else if (method == "server.webcams.list") {
        ok(jdict({{"webcams", JValue::make_list()}}));
    } else if (method == "server.announcements.list") {
        ok(jdict({{"entries", JValue::make_list()}, {"feeds", JValue::make_list()}}));
    } else if (method == "server.job_queue.status") {
        ok(jdict({{"queued_jobs", JValue::make_list()},
                  {"queue_state", jstr("ready")}}));
    } else if (method == "server.sensors.list") {
        ok(jdict({{"sensors", JValue::make_dict()}}));
    } else if (method == "machine.device_power.devices") {
        ok(jdict({{"devices", JValue::make_list()}}));
    } else if (method == "machine.update.status"
               || method == "machine.update.refresh") {
        ok(jdict({{"busy", jbool(false)},
                  {"github_rate_limit", JValue::none()},
                  {"github_requests_remaining", JValue::none()},
                  {"github_limit_reset_time", JValue::none()},
                  {"version_info", JValue::make_dict()}}));
    } else if (method == "machine.system_info") {
        uint64_t total = 0, avail = 0;
        if (opt_.memory_info)
            opt_.memory_info(&total, &avail);
        JValue info = jdict(
            {{"python", jdict({{"version", JValue::make_list(
                                               {jint(3), jint(11), jint(0),
                                                jstr("final"), jint(0)})},
                               {"version_string", jstr("3.11.0")}})},
             {"cpu_info",
              jdict({{"cpu_count", jint(2)},
                     {"bits", jstr("32bit")},
                     {"processor", jstr(opt_.cpu)},
                     {"cpu_desc", jstr(opt_.cpu)},
                     {"serial_number", jstr("")},
                     {"hardware_desc", jstr(opt_.cpu)},
                     {"model", jstr(opt_.cpu)},
                     {"total_memory", jint((int64_t)(total / 1024))},
                     {"memory_units", jstr("kB")}})},
             {"sd_info", JValue::make_dict()},
             {"distribution",
              jdict({{"name", jstr("ESP32Klipper")},
                     {"id", jstr("esp32klipper")},
                     {"version", jstr(opt_.klipper_version)},
                     {"version_parts", jdict({{"major", jstr("")},
                                              {"minor", jstr("")},
                                              {"build_number", jstr("")}})},
                     {"like", jstr("")},
                     {"codename", jstr("")}})},
             {"available_services", JValue::make_list()},
             {"instance_ids", jdict({{"moonraker", jstr("moonraker")},
                                     {"klipper", jstr("klipper")}})},
             {"service_state", JValue::make_dict()},
             {"virtualization", jdict({{"virt_type", jstr("none")},
                                       {"virt_identifier", jstr("none")}})},
             {"network", JValue::make_dict()},
             {"canbus", JValue::make_dict()}});
        ok(jdict({{"system_info", info}}));
    } else if (method == "machine.proc_stats") {
        uint64_t total = 0, avail = 0;
        if (opt_.memory_info)
            opt_.memory_info(&total, &avail);
        double now = wall_time();
        ok(jdict(
            {{"moonraker_stats",
              JValue::make_list({jdict({{"time", jnum(now)},
                                        {"cpu_usage", jnum(0.)},
                                        {"memory", jint((int64_t)((total - avail) / 1024))},
                                        {"mem_units", jstr("kB")}})})},
             {"throttled_state", jdict({{"bits", jint(0)},
                                        {"flags", JValue::make_list()}})},
             {"cpu_temp", JValue::none()},
             {"network", JValue::make_dict()},
             {"system_cpu_usage", JValue::make_dict()},
             {"system_uptime", jnum(now - start_time_)},
             {"system_memory", jdict({{"total", jint((int64_t)(total / 1024))},
                                      {"available", jint((int64_t)(avail / 1024))},
                                      {"used", jint((int64_t)((total - avail) / 1024))}})},
             {"websocket_connections", jint((int64_t)connection_count())}}));

    /* Printer */
    } else if (method == "printer.info") {
        std::string st, msg;
        p_.run_on_klippy([&] {
            st = p_.klippy_state();
            msg = st == "ready" ? "Printer is ready" : p_.state_message();
        });
        ok(jdict({{"state", jstr(st)},
                  {"state_message", jstr(msg)},
                  {"hostname", jstr(opt_.hostname)},
                  {"klipper_path", jstr("")},
                  {"python_path", jstr("")},
                  {"process_id", jint(0)},
                  {"user_id", jint(0)},
                  {"group_id", jint(0)},
                  {"log_file", jstr("")},
                  {"config_file", jstr("printer.cfg")},
                  {"software_version", jstr(opt_.klipper_version)},
                  {"cpu_info", jstr(opt_.cpu)},
                  {"app", jstr("Klipper")}}));
    } else if (method == "printer.objects.list") {
        std::vector<std::string> names;
        p_.run_on_klippy([&] { names = p_.object_names(); });
        ok(jdict({{"objects", list_of(names)}}));
    } else if (method == "printer.objects.query") {
        const JValue *o = jget(params, "objects");
        ok(query_objects(o ? *o : JValue::make_dict(), nullptr));
    } else if (method == "printer.objects.subscribe") {
        // Not under lock_: the klippy task takes it for G-code responses
        const JValue *o = jget(params, "objects");
        Conn tmp;
        JValue r = query_objects(o ? *o : JValue::make_dict(), &tmp);
        {
            std::lock_guard<std::mutex> g(lock_);
            auto it = conns_.find(conn);
            if (it != conns_.end()) {
                it->second.subs = std::move(tmp.subs);
                it->second.sent = std::move(tmp.sent);
            }
        }
        ok(r);
    } else if (method == "printer.gcode.script") {
        std::string script = jget_str(params, "script");
        {
            std::lock_guard<std::mutex> g(lock_);
            gcode_store_.push_back(script);
            if (gcode_store_.size() > 100)
                gcode_store_.pop_front();
        }
        run_gcode(script);
    } else if (method == "printer.gcode.help") {
        JValue help = JValue::make_dict();
        p_.run_on_klippy([&] {
            JValue g = p_.get_status("gcode");
            if (const JValue *c = jget(g, "commands"))
                for (const auto &kv : c->dict->items)
                    help.dict->set(kv.first, jstr(jget_str(kv.second, "help")));
        });
        ok(help);
    } else if (method == "printer.print.start") {
        std::string f = jget_str(params, "filename");
        if (!safe_relative_path(f))
            return fail(ERR_BAD, "Invalid filename");
        run_gcode("SDCARD_PRINT_FILE FILENAME=" + quote_filename(f));
    } else if (method == "printer.print.pause") {
        run_gcode("PAUSE");
    } else if (method == "printer.print.resume") {
        run_gcode("RESUME");
    } else if (method == "printer.print.cancel") {
        run_gcode("CANCEL_PRINT");
    } else if (method == "printer.emergency_stop") {
        // Not queued behind other commands (klippy handles M112 at once)
        p_.run_on_klippy([&] {
            std::string e;
            p_.run_script("M112", &e);
        });
        ok(jstr("ok"));
    } else if (method == "printer.restart" || method == "printer.firmware_restart") {
        if (!opt_.restart)
            return fail(ERR_BAD, "Restart is not supported: reset the board");
        // Both reset the MCU: the board restarts and configures it again
        p_.run_on_klippy([&] { p_.reset_mcu(); });
        ok(jstr("ok"));
        opt_.restart();

    /* Files */
    } else if (method == "server.files.roots") {
        JList l;
        for (const char *r : {"gcodes", "config"}) {
            auto fs = root_fs(r);
            if (fs)
                l.push_back(jdict({{"name", jstr(r)},
                                   {"path", jstr(std::string("/") + r)},
                                   {"permissions",
                                    jstr(fs->writable() ? "rw" : "r")}}));
        }
        ok(JValue::make_list(std::move(l)));
    } else if (method == "server.files.list") {
        std::string root = jget_str(params, "root", "gcodes");
        auto fs = root_fs(root);
        if (!fs)
            return fail(ERR_BAD, "Invalid root: " + root);
        JList l;
        for (const auto &fi : fs->list_info("", true)) {
            JValue it = file_item(root, fi);
            l.push_back(it);
        }
        ok(JValue::make_list(std::move(l)));
    } else if (method == "server.files.get_directory") {
        std::string root, rel;
        if (!split_root(jget_str(params, "path", "gcodes"), &root, &rel))
            return fail(ERR_BAD, "Invalid path");
        auto fs = root_fs(root);
        JList dirs, files;
        for (const auto &fi : fs->list_info(rel, false)) {
            std::string base = fi.name.substr(fi.name.rfind('/') + 1);
            JValue e = jdict({{"modified", jnum(fi.modified)},
                              {"size", jint((int64_t)fi.size)},
                              {"permissions", jstr(fs->writable() ? "rw" : "r")}});
            e.dict->set(fi.is_dir ? "dirname" : "filename", jstr(base));
            (fi.is_dir ? dirs : files).push_back(e);
        }
        uint64_t total = 0, freeb = 0;
        fs->disk_usage(&total, &freeb);
        ok(jdict({{"dirs", JValue::make_list(std::move(dirs))},
                  {"files", JValue::make_list(std::move(files))},
                  {"disk_usage", jdict({{"total", jint((int64_t)total)},
                                        {"used", jint((int64_t)(total - freeb))},
                                        {"free", jint((int64_t)freeb)}})},
                  {"root_info",
                   jdict({{"name", jstr(root)},
                          {"permissions", jstr(fs->writable() ? "rw" : "r")}})}}));
    } else if (method == "server.files.metadata") {
        std::string f = jget_str(params, "filename");
        FileInfo fi;
        if (!safe_relative_path(f) || !gcodes_ || !gcodes_->stat(f, &fi))
            return fail(ERR_NOT_FOUND, "Metadata not available for <" + f + ">");
        ok(jdict({{"filename", jstr(f)},
                  {"size", jint((int64_t)fi.size)},
                  {"modified", jnum(fi.modified)},
                  {"print_start_time", JValue::none()},
                  {"job_id", JValue::none()},
                  {"slicer", jstr("")},
                  {"thumbnails", JValue::make_list()}}));
    } else if (method == "server.files.delete_file") {
        std::string root, rel, err;
        if (!split_root(jget_str(params, "path"), &root, &rel) || rel.empty())
            return fail(ERR_BAD, "Invalid path");
        auto fs = root_fs(root);
        FileInfo fi;
        fs->stat(rel, &fi);
        if (!fs->remove(rel, &err))
            return fail(ERR_BAD, err);
        fi.name = rel;
        JValue r = jdict({{"item", file_item(root, fi)},
                          {"action", jstr("delete_file")}});
        broadcast("notify_filelist_changed", JValue::make_list({r}));
        ok(r);
    } else if (method == "server.files.post_directory"
               || method == "server.files.delete_directory") {
        std::string root, rel, err;
        if (!split_root(jget_str(params, "path"), &root, &rel) || rel.empty())
            return fail(ERR_BAD, "Invalid path");
        auto fs = root_fs(root);
        bool create = method == "server.files.post_directory";
        const JValue *force = jget(params, "force");
        bool good = create ? fs->make_dir(rel, &err)
                           : fs->remove_dir(rel, force && force->truthy(), &err);
        if (!good)
            return fail(ERR_BAD, err);
        FileInfo fi;
        fi.name = rel;
        fi.is_dir = true;
        JValue r = jdict({{"item", file_item(root, fi)},
                          {"action", jstr(create ? "create_dir" : "delete_dir")}});
        broadcast("notify_filelist_changed", JValue::make_list({r}));
        ok(r);
    } else if (method == "server.files.move" || method == "server.files.copy") {
        std::string sroot, srel, droot, drel, err;
        if (!split_root(jget_str(params, "source"), &sroot, &srel)
            || !split_root(jget_str(params, "dest"), &droot, &drel)
            || srel.empty() || drel.empty())
            return fail(ERR_BAD, "Invalid path");
        auto sfs = root_fs(sroot), dfs = root_fs(droot);
        bool move = method == "server.files.move";
        if (move && sfs == dfs) {
            if (!sfs->rename(srel, drel, &err))
                return fail(ERR_BAD, err);
        } else {
            auto in = sfs->open(srel);
            auto out = in ? dfs->create(drel, &err) : nullptr;
            if (!in || !out)
                return fail(ERR_BAD, err.empty() ? "Source not found" : err);
            char buf[1024];
            size_t n;
            while ((n = in->read(buf, sizeof(buf))) > 0)
                if (!out->write(buf, n))
                    return fail(ERR_BAD, "Write error");
            if (!out->close())
                return fail(ERR_BAD, "Write error");
            if (move)
                sfs->remove(srel, &err);
        }
        FileInfo fi;
        dfs->stat(drel, &fi);
        JValue r = jdict({{"item", file_item(droot, fi)},
                          {"source_item", jdict({{"path", jstr(srel)},
                                                 {"root", jstr(sroot)}})},
                          {"action", jstr(move ? "move_file" : "create_file")}});
        broadcast("notify_filelist_changed", JValue::make_list({r}));
        ok(r);

    /* Database (Mainsail settings) */
    } else if (method == "server.database.list") {
        JList ns;
        std::lock_guard<std::mutex> g(lock_);
        for (const auto &kv : db_.dict->items)
            ns.push_back(jstr(kv.first));
        ok(jdict({{"namespaces", JValue::make_list(std::move(ns))},
                  {"backups", JValue::make_list()}}));
    } else if (method == "server.database.get_item") {
        std::string ns = jget_str(params, "namespace");
        std::string key = jget_str(params, "key");
        std::lock_guard<std::mutex> g(lock_);
        JValue *n = db_.dict->find(ns);
        if (!n)
            return fail(ERR_NOT_FOUND, "Namespace '" + ns + "' not found");
        JValue *v = key.empty() ? n : db_walk(*n, key, false);
        if (!v)
            return fail(ERR_NOT_FOUND,
                        "Key '" + key + "' in namespace '" + ns + "' not found");
        ok(jdict({{"namespace", jstr(ns)},
                  {"key", key.empty() ? JValue::none() : jstr(key)},
                  {"value", *v}}));
    } else if (method == "server.database.post_item") {
        std::string ns = jget_str(params, "namespace");
        std::string key = jget_str(params, "key");
        const JValue *val = jget(params, "value");
        if (ns.empty() || key.empty() || !val)
            return fail(ERR_BAD, "Invalid request");
        {
            std::lock_guard<std::mutex> g(lock_);
            if (!db_.dict->find(ns))
                db_.dict->set(ns, JValue::make_dict());
            *db_walk(*db_.dict->find(ns), key, true) = *val;
        }
        db_save();
        ok(jdict({{"namespace", jstr(ns)}, {"key", jstr(key)}, {"value", *val}}));
    } else if (method == "server.database.delete_item") {
        std::string ns = jget_str(params, "namespace");
        std::string key = jget_str(params, "key");
        JValue old;
        {
            std::lock_guard<std::mutex> g(lock_);
            JValue *n = db_.dict->find(ns);
            size_t dot = key.rfind('.');
            JValue *parent = !n ? nullptr
                             : dot == std::string::npos
                                 ? n
                                 : db_walk(*n, key.substr(0, dot), false);
            std::string leaf = dot == std::string::npos ? key : key.substr(dot + 1);
            JValue *v = parent && parent->t == JValue::T::Dict
                            ? parent->dict->find(leaf) : nullptr;
            if (!v)
                return fail(ERR_NOT_FOUND, "Key '" + key + "' not found");
            old = *v;
            auto &items = parent->dict->items;
            for (auto it = items.begin(); it != items.end(); ++it)
                if (it->first == leaf) {
                    items.erase(it);
                    break;
                }
        }
        db_save();
        ok(jdict({{"namespace", jstr(ns)}, {"key", jstr(key)}, {"value", old}}));

    /* History */
    } else if (method == "server.history.list") {
        JList jobs;
        {
            std::lock_guard<std::mutex> g(lock_);
            for (auto it = history_.rbegin(); it != history_.rend(); ++it)
                jobs.push_back(*it);
        }
        int64_t count = (int64_t)jobs.size();
        ok(jdict({{"count", jint(count)}, {"jobs", JValue::make_list(std::move(jobs))}}));
    } else if (method == "server.history.totals") {
        double total_time = 0., longest = 0.;
        int64_t n = 0;
        {
            std::lock_guard<std::mutex> g(lock_);
            for (const auto &j : history_) {
                const JValue *d = jget(j, "print_duration");
                double v = d ? d->f : 0.;
                total_time += v;
                longest = std::max(longest, v);
                n++;
            }
        }
        ok(jdict({{"job_totals",
                   jdict({{"total_jobs", jint(n)},
                          {"total_time", jnum(total_time)},
                          {"total_print_time", jnum(total_time)},
                          {"total_filament_used", jnum(0.)},
                          {"longest_job", jnum(longest)},
                          {"longest_print", jnum(longest)}})},
                  {"auxiliary_totals", JValue::make_list()}}));
    } else if (method == "server.history.get_job") {
        fail(ERR_NOT_FOUND, "Job not found");
    } else if (method == "server.history.delete_job") {
        {
            std::lock_guard<std::mutex> g(lock_);
            history_.clear();
        }
        ok(jdict({{"deleted_jobs", JValue::make_list()}}));
    } else {
        fail(ERR_METHOD, "Method not found: " + method);
    }
}

/****************************************************************
 * Periodic work: status updates, G-code responses, history
 ****************************************************************/

void
Moonraker::tick()
{
    // G-code responses
    std::deque<std::string> lines;
    {
        std::lock_guard<std::mutex> g(lock_);
        lines.swap(gcode_queue_);
        for (const auto &l : lines) {
            gcode_store_.push_back(l);
            if (gcode_store_.size() > 100)
                gcode_store_.pop_front();
        }
    }
    for (const auto &l : lines)
        broadcast("notify_gcode_response", JValue::make_list({jstr(l)}));

    // Union of the subscribed objects
    std::map<std::string, std::set<std::string>> want; // empty set = all
    {
        std::lock_guard<std::mutex> g(lock_);
        for (const auto &c : conns_)
            for (const auto &s : c.second.subs) {
                auto &w = want[s.first];
                if (s.second.empty())
                    w.insert("*");
                for (const auto &f : s.second)
                    w.insert(f);
            }
    }
    want["print_stats"];
    want["webhooks"];
    std::map<std::string, JValue> status;
    double eventtime = 0.;
    bool done = p_.run_on_klippy([&] {
        eventtime = p_.monotonic();
        auto names = p_.object_names();
        std::set<std::string> known(names.begin(), names.end());
        for (const auto &w : want) {
            if (!known.count(w.first) || is_static_object(w.first))
                continue;
            status[w.first] = p_.get_status(w.first);
        }
    }, 1.0);
    if (!done)
        return;

    // Klippy state and job history
    std::string kstate = jget_str(status["webhooks"], "state");
    if (kstate != last_klippy_state_) {
        if (kstate == "ready")
            broadcast("notify_klippy_ready", JValue::make_list());
        else if (kstate == "shutdown")
            broadcast("notify_klippy_shutdown", JValue::make_list());
        last_klippy_state_ = kstate;
    }
    record_history(status["print_stats"]);

    // Changed fields per connection
    std::vector<std::pair<int, std::string>> out;
    {
        std::lock_guard<std::mutex> g(lock_);
        for (auto &c : conns_) {
            JValue diff = JValue::make_dict();
            for (const auto &s : c.second.subs) {
                auto it = status.find(s.first);
                if (it == status.end() || it->second.t != JValue::T::Dict)
                    continue;
                JValue od = JValue::make_dict();
                auto &sent = c.second.sent[s.first];
                for (const auto &f : it->second.dict->items) {
                    if (!s.second.empty()
                        && std::find(s.second.begin(), s.second.end(), f.first)
                               == s.second.end())
                        continue;
                    std::string js = json_dump(f.second);
                    auto si = sent.find(f.first);
                    if (si != sent.end() && si->second == js)
                        continue;
                    sent[f.first] = js;
                    od.dict->set(f.first, f.second);
                }
                if (!od.dict->items.empty())
                    diff.dict->set(s.first, od);
            }
            if (diff.dict->items.empty())
                continue;
            std::string msg = "{\"jsonrpc\":\"2.0\",\"method\":"
                              "\"notify_status_update\",\"params\":[";
            json_write(msg, diff);
            msg += ',';
            json_write(msg, jnum(eventtime));
            msg += "]}";
            out.emplace_back(c.first, std::move(msg));
        }
    }
    for (auto &o : out)
        send(o.first, o.second);
}

void
Moonraker::record_history(const JValue &ps)
{
    std::string state = jget_str(ps, "state");
    if (state == last_print_state_)
        return;
    std::string prev = last_print_state_;
    last_print_state_ = state;
    bool finished = state == "complete" || state == "cancelled"
                    || state == "error";
    if (!finished || (prev != "printing" && prev != "paused"))
        return;
    const JValue *pd = jget(ps, "print_duration");
    const JValue *td = jget(ps, "total_duration");
    double now = wall_time();
    double total = td ? (td->t == JValue::T::Int ? (double)td->i : td->f) : 0.;
    char id[16];
    snprintf(id, sizeof(id), "%06X", (unsigned)next_job_++);
    JValue job = jdict({{"job_id", jstr(id)},
                        {"exists", jbool(true)},
                        {"filename", jstr(jget_str(ps, "filename"))},
                        {"status", jstr(state == "complete" ? "completed" : state)},
                        {"start_time", jnum(now - total)},
                        {"end_time", jnum(now)},
                        {"print_duration", pd ? *pd : jnum(0.)},
                        {"total_duration", td ? *td : jnum(0.)},
                        {"filament_used", jnum(0.)},
                        {"metadata", JValue::make_dict()}});
    {
        std::lock_guard<std::mutex> g(lock_);
        history_.push_back(job);
        if (history_.size() > 50)
            history_.erase(history_.begin());
    }
    broadcast("notify_history_changed",
              JValue::make_list({jdict({{"action", jstr("finished")},
                                        {"job", job}})}));
}

std::string
Moonraker::upload_done(const std::string &root, const std::string &path,
                       bool start_print)
{
    FileInfo fi;
    auto fs = root_fs(root);
    if (fs)
        fs->stat(path, &fi);
    fi.name = path;
    JValue item = file_item(root, fi);
    broadcast("notify_filelist_changed",
              JValue::make_list({jdict({{"item", item},
                                        {"action", jstr("create_file")}})}));
    bool started = false;
    if (start_print && root == "gcodes") {
        p_.submit_script("SDCARD_PRINT_FILE FILENAME=" + quote_filename(path));
        started = true;
    }
    return json_dump(jdict({{"item", item},
                            {"print_started", jbool(started)},
                            {"print_queued", jbool(false)},
                            {"action", jstr("create_file")}}));
}

/****************************************************************
 * Database file
 ****************************************************************/

void
Moonraker::db_load()
{
    if (!config_ || opt_.database_file.empty())
        return;
    auto f = config_->open(opt_.database_file);
    if (!f)
        return;
    std::string text(f->size(), '\0');
    text.resize(f->read(&text[0], text.size()));
    JValue v;
    std::string err;
    if (json_load(text, &v, &err) && v.t == JValue::T::Dict)
        db_ = v;
}

void
Moonraker::db_save()
{
    if (!config_ || opt_.database_file.empty() || !config_->writable())
        return;
    std::string text;
    {
        std::lock_guard<std::mutex> g(lock_);
        text = json_dump(db_);
    }
    std::string err;
    auto w = config_->create(opt_.database_file, &err);
    if (w && w->write(text.data(), text.size()))
        w->close();
}

} // namespace kl
