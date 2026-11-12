// Port of klippy/gcode.py (see gcode.hpp).
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "klippy/gcode.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace kl {

static std::string
strip(const std::string &s)
{
    size_t b = 0, e = s.size();
    while (b < e && isspace((unsigned char)s[b]))
        b++;
    while (e > b && isspace((unsigned char)s[e - 1]))
        e--;
    return s.substr(b, e - b);
}

static std::string
upper(std::string s)
{
    for (auto &c : s)
        c = (char)toupper((unsigned char)c);
    return s;
}

// "Traditional" G-code: a letter followed by a number (G1, M3, G01)
static bool
is_traditional_gcode(const std::string &cmd)
{
    if (cmd.size() < 2 || !isupper((unsigned char)cmd[0])
        || !isdigit((unsigned char)cmd[1]))
        return false;
    char *end;
    strtod(cmd.c_str() + 1, &end);
    return *end == '\0';
}

/****************************************************************
 * GCodeCommand
 ****************************************************************/

std::string
GCodeCommand::get_raw_command_parameters() const
{
    const std::string &origline = commandline_;
    size_t param_start = command_.size(), param_end = origline.size();
    if (upper(origline.substr(0, param_start)) != command_) {
        // Skip a line number, ignore a trailing checksum
        size_t p = upper(origline).find(command_);
        param_start += p == std::string::npos ? 0 : p;
        size_t star = origline.rfind('*');
        if (star != std::string::npos && star + 1 < origline.size()) {
            bool digits = true;
            for (size_t i = star + 1; i < origline.size(); i++)
                digits = digits && isdigit((unsigned char)origline[i]);
            if (digits)
                param_end = star;
        }
    }
    if (param_start < origline.size()
        && isspace((unsigned char)origline[param_start]))
        param_start++;
    if (param_start > param_end)
        return "";
    return origline.substr(param_start, param_end - param_start);
}

bool
GCodeCommand::get(const std::string &name, std::string *out, std::string *err,
                  const char *def) const
{
    const std::string *v = find(name);
    if (!v) {
        if (def) {
            *out = def;
            return true;
        }
        if (err)
            *err = "Error on '" + commandline_ + "': missing " + name;
        return false;
    }
    *out = *v;
    return true;
}

bool
GCodeCommand::get_float(const std::string &name, double *out, std::string *err,
                        const double *def, double minval, double maxval,
                        double above, double below) const
{
    const std::string *sv = find(name);
    if (!sv) {
        if (def) {
            *out = *def;
            return true;
        }
        if (err)
            *err = "Error on '" + commandline_ + "': missing " + name;
        return false;
    }
    char *end;
    double v = strtod(sv->c_str(), &end);
    if (sv->empty() || *end) {
        if (err)
            *err = "Unable to parse '" + *sv + "' as a float";
        return false;
    }
    const char *why = nullptr;
    double lim = 0.;
    if (!std::isnan(minval) && v < minval) { why = "must have minimum of"; lim = minval; }
    else if (!std::isnan(maxval) && v > maxval) { why = "must have maximum of"; lim = maxval; }
    else if (!std::isnan(above) && v <= above) { why = "must be above"; lim = above; }
    else if (!std::isnan(below) && v >= below) { why = "must be below"; lim = below; }
    if (why) {
        if (err) {
            char buf[64];
            snprintf(buf, sizeof(buf), " %g", lim);
            *err = "Error on '" + commandline_ + "': " + name + " " + why + buf;
        }
        return false;
    }
    *out = v;
    return true;
}

bool
GCodeCommand::get_int(const std::string &name, int64_t *out, std::string *err,
                      const int64_t *def) const
{
    const std::string *sv = find(name);
    if (!sv) {
        if (def) {
            *out = *def;
            return true;
        }
        if (err)
            *err = "Error on '" + commandline_ + "': missing " + name;
        return false;
    }
    char *end;
    long long v = strtoll(sv->c_str(), &end, 10);
    if (sv->empty() || *end) {
        if (err)
            *err = "Unable to parse '" + *sv + "' as a int";
        return false;
    }
    *out = v;
    return true;
}

/****************************************************************
 * Parsing
 ****************************************************************/

bool
GCodeDispatch::parse_line(const std::string &rawline, std::string *cmd,
                          GCodeCommand::ParamList *params,
                          std::string *commandline)
{
    // gcode.py _process_commands(): same result as
    //   parts = args_r.split(line.upper())   (args_r = '([A-Z_]+|[A-Z*])')
    // computed in one pass over the line without building the parts list.
    size_t b = 0, e = rawline.size();
    while (b < e && isspace((unsigned char)rawline[b]))
        b++;
    while (e > b && isspace((unsigned char)rawline[e - 1]))
        e--;
    commandline->assign(rawline, b, e - b);
    const char *ln = commandline->data();
    size_t n = commandline->size();
    if (const char *semi = (const char *)memchr(ln, ';', n))
        n = semi - ln;
    params->clear();
    cmd->clear();
    auto is_name = [](char c) { return isalpha((unsigned char)c) || c == '_'; };
    auto up = [](char c) { return (char)toupper((unsigned char)c); };
    auto strip_span = [&](size_t s0, size_t s1, std::string *out) {
        while (s0 < s1 && isspace((unsigned char)ln[s0]))
            s0++;
        while (s1 > s0 && isspace((unsigned char)ln[s1 - 1]))
            s1--;
        out->clear();
        for (size_t k = s0; k < s1; k++)
            *out += up(ln[k]);
    };
    // Leading text before the first name (parts[0])
    size_t pos = 0;
    while (pos < n && !is_name(ln[pos]) && ln[pos] != '*')
        pos++;
    size_t text0_end = pos;
    int index = 0; // name index (parts[1], parts[3], ...)
    bool skip_n = false;
    std::string name, value;
    while (pos < n) {
        size_t ns = pos;
        if (ln[pos] == '*')
            pos++;
        else
            while (pos < n && is_name(ln[pos]))
                pos++;
        size_t ne = pos;
        while (pos < n && !is_name(ln[pos]) && ln[pos] != '*')
            pos++;
        name.clear();
        for (size_t k = ns; k < ne; k++)
            name += up(ln[k]);
        strip_span(ne, pos, &value);
        if (index == 0) {
            // ''.join(parts[:2]) == 'N': line number, command is parts[3:5]
            skip_n = text0_end == 0 && name == "N";
            if (!skip_n) {
                std::string t0;
                strip_span(0, text0_end, &t0);
                std::string raw;
                for (size_t k = 0; k < text0_end; k++)
                    raw += up(ln[k]);
                std::string full = raw + name;
                for (size_t k = ne; k < pos; k++)
                    full += up(ln[k]);
                size_t fb = full.find_first_not_of(" \t");
                size_t fe = full.find_last_not_of(" \t");
                *cmd = fb == std::string::npos ? "" : full.substr(fb, fe - fb + 1);
            }
        } else if (index == 1 && skip_n) {
            std::string full = name;
            for (size_t k = ne; k < pos; k++)
                full += up(ln[k]);
            size_t fe = full.find_last_not_of(" \t");
            *cmd = fe == std::string::npos ? "" : full.substr(0, fe + 1);
        }
        index++;
        // {parts[i]: parts[i+1]}: first position, last value
        bool found = false;
        for (auto &kv : *params)
            if (kv.first == name) {
                kv.second = value;
                found = true;
                break;
            }
        if (!found)
            params->emplace_back(name, value);
    }
    if (index == 0) {
        std::string t0;
        strip_span(0, text0_end, &t0);
        *cmd = t0;
    }
    return true;
}

// gcode.py _get_extended_params(): KEY=value pairs with shell quoting
static bool
extended_params(const std::string &raw, GCodeCommand::ParamList *out,
                std::string *err, const std::string &commandline)
{
    out->clear();
    size_t i = 0;
    while (i < raw.size()) {
        while (i < raw.size() && isspace((unsigned char)raw[i]))
            i++;
        if (i >= raw.size() || raw[i] == '#' || raw[i] == ';')
            break;
        std::string word;
        while (i < raw.size() && !isspace((unsigned char)raw[i])) {
            char c = raw[i];
            if (c == '"' || c == '\'') {
                size_t close = raw.find(c, i + 1);
                if (close == std::string::npos) {
                    if (err)
                        *err = "Malformed command '" + commandline + "'";
                    return false;
                }
                word += raw.substr(i + 1, close - i - 1);
                i = close + 1;
                continue;
            }
            if (c == '#' || c == ';')
                break;
            word += c;
            i++;
        }
        size_t eq = word.find('=');
        if (eq == std::string::npos) {
            if (err)
                *err = "Malformed command '" + commandline + "'";
            return false;
        }
        std::string k = upper(word.substr(0, eq)), v = word.substr(eq + 1);
        bool found = false;
        for (auto &kv : *out)
            if (kv.first == k) {
                kv.second = v;
                found = true;
            }
        if (!found)
            out->emplace_back(k, v);
    }
    return true;
}

/****************************************************************
 * Dispatch
 ****************************************************************/

void
GCodeDispatch::register_command(const std::string &cmd, Handler h,
                                const std::string &desc)
{
    if (!h) {
        handlers_.erase(cmd);
        return;
    }
    handlers_[cmd] = std::move(h);
    if (!desc.empty())
        help_[cmd] = desc;
}

std::vector<std::pair<std::string, std::string>>
GCodeDispatch::command_help() const
{
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto &h : handlers_) {
        auto it = help_.find(h.first);
        out.emplace_back(h.first, it != help_.end() ? it->second : "");
    }
    for (const auto &m : mux_)
        if (!handlers_.count(m.first)) {
            auto it = help_.find(m.first);
            out.emplace_back(m.first, it != help_.end() ? it->second : "");
        }
    return out;
}

void
GCodeDispatch::register_mux_command(const std::string &cmd,
                                    const std::string &key,
                                    const std::string &value, Handler h,
                                    const std::string &desc)
{
    auto it = mux_.find(cmd);
    if (it == mux_.end()) {
        mux_[cmd].key = key;
        register_command(cmd, [this, cmd](GCodeCommand &g, std::string *err) {
            Mux &m = mux_[cmd];
            std::string v;
            if (!g.get(m.key, &v, err))
                return false;
            auto h = m.values.find(v);
            if (h == m.values.end()) {
                if (err)
                    *err = "The value '" + v + "' is not valid for " + m.key;
                return false;
            }
            return h->second(g, err);
        }, desc);
    }
    mux_[cmd].values[value] = std::move(h);
}

bool
GCodeDispatch::has_command(const std::string &cmd) const
{
    return handlers_.count(cmd) != 0;
}

GCodeDispatch::Handler
GCodeDispatch::get_handler(const std::string &cmd) const
{
    auto it = handlers_.find(cmd);
    return it == handlers_.end() ? Handler() : it->second;
}

void
GCodeDispatch::rename_command(const std::string &cmd,
                              const std::string &new_name)
{
    auto it = handlers_.find(cmd);
    if (it == handlers_.end())
        return;
    handlers_[new_name] = it->second;
    handlers_.erase(cmd);
}

bool
GCodeDispatch::run_line(const std::string &line, std::string *err)
{
    std::string cmd, commandline;
    GCodeCommand::ParamList params;
    double t0 = clock ? clock() : 0.;
    parse_line(line, &cmd, &params, &commandline);
    if (cmd.empty())
        return true;
    auto it = handlers_.find(cmd);
    if (it == handlers_.end()) {
        // cmd_default(): quietly accept a few harmless commands
        if (cmd == "M105" || cmd == "M21" || cmd == "M107" || cmd == "M140"
            || cmd == "M104" || cmd == "M106")
            return true;
        respond_info("Unknown command:\"" + cmd + "\"");
        return true;
    }
    if (!is_traditional_gcode(cmd)) {
        GCodeCommand tmp(cmd, commandline, {});
        if (!extended_params(tmp.get_raw_command_parameters(), &params, err,
                             commandline))
            return false;
    }
    GCodeCommand gcmd(cmd, commandline, std::move(params));
    if (clock) {
        parse_s += clock() - t0;
        parsed++;
    }
    Handler h = it->second; // the handler may re-register commands
    depth_++;
    bool ok = h(gcmd, err);
    depth_--;
    return ok;
}

bool
GCodeDispatch::run_script(const std::string &script, std::string *err)
{
    // Split on newlines (std::istringstream is slow on the ESP32)
    size_t pos = 0, n = script.size();
    std::string line;
    while (pos < n) {
        size_t nl = script.find('\n', pos);
        size_t end = nl == std::string::npos ? n : nl;
        line.assign(script, pos, end - pos);
        if (!run_line(line, err))
            return false;
        pos = end + 1;
    }
    return true;
}

void
GCodeDispatch::respond_info(const std::string &msg) const
{
    // Each line prefixed with "// " like Klipper
    std::istringstream in(msg);
    std::string line, out;
    while (std::getline(in, line))
        out += (out.empty() ? "// " : "\n// ") + line;
    respond_raw(out);
}

void
GCodeDispatch::respond_raw(const std::string &msg) const
{
    if (monitor_)
        monitor_(msg);
    if (responder_)
        responder_(msg);
    else
        printf("%s\n", msg.c_str());
}

} // namespace kl
