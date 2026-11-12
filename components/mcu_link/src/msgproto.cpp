// Port of klippy/msgproto.py (see msgproto.hpp).
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "mcu_link/msgproto.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "mcu_link/json_lite.hpp"

namespace kl {

/****************************************************************
 * MsgFormat / MsgParams
 ****************************************************************/

int
MsgFormat::param_index(const std::string &pname) const
{
    for (size_t i = 0; i < params.size(); i++)
        if (params[i].name == pname)
            return (int)i;
    return -1;
}

bool
MsgParams::has(const char *pname) const
{
    return fmt && fmt->param_index(pname) >= 0;
}

int64_t
MsgParams::get(const char *pname, int64_t def) const
{
    if (!fmt)
        return def;
    int i = fmt->param_index(pname);
    return i < 0 ? def : ints[i];
}

std::string
MsgParams::get_str(const char *pname) const
{
    if (!fmt)
        return "";
    int i = fmt->param_index(pname);
    if (i < 0)
        return "";
    if (!fmt->params[i].is_int())
        return bufs[i];
    return bufs[i].empty() ? std::to_string(ints[i]) : bufs[i];
}

std::string
MsgParams::to_string() const
{
    if (!fmt)
        return name + " " + output;
    std::string out = name;
    for (size_t i = 0; i < fmt->params.size(); i++) {
        out += " " + fmt->params[i].name + "=";
        if (fmt->params[i].is_int())
            out += bufs[i].empty() ? std::to_string(ints[i]) : bufs[i];
        else
            out += "'" + bufs[i] + "'";
    }
    return out;
}

/****************************************************************
 * VLQ encoding
 ****************************************************************/

void
MessageParser::encode_int(std::vector<uint8_t> &out, int64_t v32)
{
    int32_t v = (int32_t)(uint32_t)v32;
    if (v >= 0xc000000 || v < -0x4000000) out.push_back(((v >> 28) & 0x7f) | 0x80);
    if (v >= 0x180000 || v < -0x80000)   out.push_back(((v >> 21) & 0x7f) | 0x80);
    if (v >= 0x3000 || v < -0x1000)      out.push_back(((v >> 14) & 0x7f) | 0x80);
    if (v >= 0x60 || v < -0x20)          out.push_back(((v >> 7) & 0x7f) | 0x80);
    out.push_back(v & 0x7f);
}

bool
MessageParser::parse_int(const uint8_t *s, int len, int *pos, bool is_signed,
                         int64_t *out)
{
    if (*pos >= len)
        return false;
    uint8_t c = s[(*pos)++];
    uint32_t v = c & 0x7f;
    if ((c & 0x60) == 0x60)
        v |= (uint32_t)-0x20;
    while (c & 0x80) {
        if (*pos >= len)
            return false;
        c = s[(*pos)++];
        v = (v << 7) | (c & 0x7f);
    }
    *out = is_signed ? (int64_t)(int32_t)v : (int64_t)v;
    return true;
}

/****************************************************************
 * MessageParser
 ****************************************************************/

MessageParser::MessageParser()
{
    add_message("identify_response offset=%u data=%.*s", 0,
                MsgFormat::Kind::Response);
    add_message("identify offset=%u count=%c", 1, MsgFormat::Kind::Command);
}

static bool
lookup_type(const std::string &t, ParamType *out)
{
    static const struct { const char *s; ParamType t; } types[] = {
        {"%u", ParamType::U32}, {"%i", ParamType::I32},
        {"%hu", ParamType::U16}, {"%hi", ParamType::I16},
        {"%c", ParamType::Byte}, {"%s", ParamType::String},
        {"%.*s", ParamType::ProgmemBuffer}, {"%*s", ParamType::Buffer},
    };
    for (const auto &e : types)
        if (t == e.s) {
            *out = e.t;
            return true;
        }
    return false;
}

static bool
is_signed_type(ParamType t)
{
    return t == ParamType::I32 || t == ParamType::I16;
}

bool
MessageParser::parse_params(const std::string &format, MsgFormat *mf,
                            std::string *err)
{
    if (mf->kind == MsgFormat::Kind::Output) {
        // printf style: find each %-conversion (lookup_output_params)
        size_t pos = 0;
        while ((pos = format.find('%', pos)) != std::string::npos) {
            if (pos + 1 < format.size() && format[pos + 1] == '%') {
                pos += 2;
                continue;
            }
            bool found = false;
            for (int n = 4; n >= 2 && !found; n--) {
                ParamType t;
                if (lookup_type(format.substr(pos, n), &t)) {
                    MsgParam mp;
                    mp.type = t;
                    mf->params.push_back(mp);
                    found = true;
                }
            }
            if (!found) {
                if (err)
                    *err = "Invalid output format for '" + format + "'";
                return false;
            }
            pos++;
        }
        return true;
    }
    // "name a=%u b=%c" (lookup_params)
    std::istringstream ss(format);
    std::string word;
    ss >> word;
    while (ss >> word) {
        size_t eq = word.find('=');
        if (eq == std::string::npos) {
            if (err)
                *err = "Bad message format '" + format + "'";
            return false;
        }
        MsgParam p;
        p.name = word.substr(0, eq);
        if (!lookup_type(word.substr(eq + 1), &p.type)) {
            if (err)
                *err = "Bad parameter type in '" + format + "'";
            return false;
        }
        for (const auto &en : enumerations_) {
            const std::string &ename = en.first;
            if (p.name == ename
                || (p.name.size() > ename.size() + 1
                    && p.name.compare(p.name.size() - ename.size() - 1,
                                      std::string::npos, "_" + ename) == 0)) {
                p.enums = &en.second;
                p.enum_name = ename;
                break;
            }
        }
        mf->params.push_back(std::move(p));
    }
    return true;
}

void
MessageParser::add_message(const std::string &format, int32_t msgid,
                           MsgFormat::Kind kind)
{
    auto mf = std::make_unique<MsgFormat>();
    mf->msgid = msgid;
    mf->format = format;
    mf->kind = kind;
    size_t sp = format.find(' ');
    mf->name = kind == MsgFormat::Kind::Output ? "#output"
                                               : format.substr(0, sp);
    std::string err;
    if (!parse_params(format, mf.get(), &err)) {
        fprintf(stderr, "msgproto: %s\n", err.c_str());
        return;
    }
    by_id_[msgid] = mf.get();
    if (kind != MsgFormat::Kind::Output)
        by_name_[mf->name] = mf.get();
    messages_.push_back(std::move(mf));
}

bool
MessageParser::process_identify(const std::string &json, std::string *err)
{
    JsonValue root;
    if (!json_parse(json.data(), json.size(), &root, err))
        return false;
    if (!root.is_object()) {
        if (err)
            *err = "data dictionary is not an object";
        return false;
    }
    // Enumerations (fill_enumerations)
    if (const JsonValue *ens = root.get("enumerations")) {
        for (const auto &en : ens->obj) {
            auto &enums = enumerations_[en.first];
            auto &rev = reverse_enums_[en.first];
            for (const auto &kv : en.second.obj) {
                if (kv.second.is_int()) {
                    enums[kv.first] = kv.second.i;
                    rev[kv.second.i] = kv.first;
                    continue;
                }
                if (!kv.second.is_array() || kv.second.arr.size() != 2)
                    continue;
                // Range: "gpio0": [start_value, count]
                std::string root_name = kv.first;
                while (!root_name.empty() && isdigit((unsigned char)root_name.back()))
                    root_name.pop_back();
                int64_t start_enum = 0;
                if (root_name.size() != kv.first.size())
                    start_enum = atoll(kv.first.c_str() + root_name.size());
                int64_t start_value = kv.second.arr[0].i;
                int64_t count = kv.second.arr[1].i;
                for (int64_t i = 0; i < count; i++) {
                    std::string n = root_name + std::to_string(start_enum + i);
                    enums[n] = start_value + i;
                    rev[start_value + i] = n;
                }
            }
        }
    }
    // Messages
    struct { const char *key; MsgFormat::Kind kind; } sections[] = {
        {"commands", MsgFormat::Kind::Command},
        {"responses", MsgFormat::Kind::Response},
        {"output", MsgFormat::Kind::Output},
    };
    for (const auto &sec : sections) {
        const JsonValue *msgs = root.get(sec.key);
        if (!msgs)
            continue;
        for (const auto &kv : msgs->obj)
            add_message(kv.first, (int32_t)kv.second.i, sec.kind);
    }
    if (const JsonValue *cfg = root.get("config"))
        for (const auto &kv : cfg->obj)
            config_[kv.first] = kv.second.as_text();
    if (const JsonValue *v = root.get("version"))
        version_ = v->as_text();
    if (const JsonValue *v = root.get("build_versions"))
        build_versions_ = v->as_text();
    return true;
}

const MsgFormat *
MessageParser::lookup_name(const std::string &name) const
{
    auto it = by_name_.find(name);
    return it == by_name_.end() ? nullptr : it->second;
}

const MsgFormat *
MessageParser::lookup_command(const std::string &format, std::string *err) const
{
    std::string name = format.substr(0, format.find(' '));
    const MsgFormat *mf = lookup_name(name);
    if (!mf) {
        if (err)
            *err = "Unknown command: " + name;
        return nullptr;
    }
    if (mf->format != format) {
        if (err)
            *err = "Command format mismatch: " + format + " vs " + mf->format;
        return nullptr;
    }
    return mf;
}

bool
MessageParser::lookup_enum(const std::string &enum_name,
                           const std::string &value, int64_t *out) const
{
    auto it = enumerations_.find(enum_name);
    if (it == enumerations_.end())
        return false;
    auto v = it->second.find(value);
    if (v == it->second.end())
        return false;
    *out = v->second;
    return true;
}

bool
MessageParser::encode(const MsgFormat &fmt, const std::vector<MsgArg> &args,
                      std::vector<uint8_t> *out, std::string *err) const
{
    if (args.size() != fmt.params.size()) {
        if (err)
            *err = "Wrong argument count for " + fmt.name;
        return false;
    }
    out->clear();
    encode_int(*out, fmt.msgid);
    for (size_t i = 0; i < args.size(); i++) {
        const MsgParam &p = fmt.params[i];
        const MsgArg &a = args[i];
        if (p.is_int()) {
            int64_t v = a.i;
            if (a.is_str) {
                if (!p.enums) {
                    if (err)
                        *err = "Parameter " + p.name + " of " + fmt.name
                               + " is not an enumeration";
                    return false;
                }
                auto it = p.enums->find(a.s);
                if (it == p.enums->end()) {
                    if (err)
                        *err = "Unknown value '" + a.s + "' in enumeration '"
                               + p.enum_name + "'";
                    return false;
                }
                v = it->second;
            }
            encode_int(*out, v);
        } else {
            if (a.s.size() > MESSAGE_PAYLOAD_MAX) {
                if (err)
                    *err = "Buffer too long for " + fmt.name;
                return false;
            }
            out->push_back((uint8_t)a.s.size());
            out->insert(out->end(), a.s.begin(), a.s.end());
        }
    }
    if (out->size() > (size_t)MESSAGE_PAYLOAD_MAX) {
        if (err)
            *err = "Message too long: " + fmt.name;
        return false;
    }
    return true;
}

static bool
parse_hex_buffer(const std::string &value, std::string *out)
{
    // msgproto.py _parse_buffer(): big hex number, two digits per byte
    out->clear();
    if (value.size() % 2)
        return false;
    for (size_t i = 0; i < value.size(); i += 2) {
        char *end;
        std::string b = value.substr(i, 2);
        long v = strtol(b.c_str(), &end, 16);
        if (*end)
            return false;
        out->push_back((char)v);
    }
    return true;
}

bool
MessageParser::create_command(const std::string &text,
                              std::vector<uint8_t> *out, std::string *err) const
{
    std::istringstream ss(text);
    std::string name, word;
    ss >> name;
    const MsgFormat *mf = lookup_name(name);
    if (!mf) {
        if (err)
            *err = "Unknown command: " + name;
        return false;
    }
    std::vector<MsgArg> args(mf->params.size(), MsgArg(0));
    std::vector<bool> seen(mf->params.size(), false);
    while (ss >> word) {
        size_t eq = word.find('=');
        int idx = eq == std::string::npos ? -1
                                          : mf->param_index(word.substr(0, eq));
        if (idx < 0) {
            if (err)
                *err = "Unable to extract params from: " + text;
            return false;
        }
        std::string value = word.substr(eq + 1);
        const MsgParam &p = mf->params[idx];
        if (!p.is_int()) {
            std::string buf;
            if (!parse_hex_buffer(value, &buf)) {
                if (err)
                    *err = "Bad buffer value in: " + text;
                return false;
            }
            args[idx] = MsgArg(buf);
        } else {
            char *end;
            long long v = strtoll(value.c_str(), &end, 0);
            if (*end == '\0' && !value.empty())
                args[idx] = MsgArg((int64_t)v);
            else
                args[idx] = MsgArg(value); // enumeration value (pin name)
        }
        seen[idx] = true;
    }
    for (size_t i = 0; i < seen.size(); i++)
        if (!seen[i]) {
            if (err)
                *err = "Missing parameter " + mf->params[i].name + " in: " + text;
            return false;
        }
    return encode(*mf, args, out, err);
}

bool
MessageParser::parse(const uint8_t *s, int len, MsgParams *out,
                     std::string *err) const
{
    int pos = MESSAGE_HEADER_SIZE;
    int end = len - MESSAGE_TRAILER_SIZE;
    if (!parse_at(s, end, &pos, out, err))
        return false;
    if (pos != end) {
        if (err)
            *err = "Extra data at end of message";
        return false;
    }
    return true;
}

std::vector<std::string>
MessageParser::dump(const uint8_t *s, int len) const
{
    // klippy msgproto.py MessageParser.dump(): every message in a block
    std::vector<std::string> out;
    int pos = MESSAGE_HEADER_SIZE;
    int end = len - MESSAGE_TRAILER_SIZE;
    while (pos < end) {
        MsgParams p;
        std::string err;
        if (!parse_at(s, end, &pos, &p, &err)) {
            out.push_back("<" + err + ">");
            break;
        }
        std::string txt = p.name == "#output" || p.name == "#unknown"
                              ? p.name + " " + p.output : p.to_string();
        // Buffers are binary: escape like Python's repr()
        std::string esc;
        for (unsigned char c : txt) {
            if (c >= 0x20 && c < 0x7f) {
                esc += (char)c;
            } else {
                char b[5];
                snprintf(b, sizeof(b), "\\x%02x", c);
                esc += b;
            }
        }
        out.push_back(std::move(esc));
    }
    return out;
}

bool
MessageParser::parse_at(const uint8_t *s, int end, int *posp, MsgParams *out,
                        std::string *err) const
{
    int pos = *posp;
    int64_t msgid;
    *out = MsgParams();
    if (!parse_int(s, end, &pos, true, &msgid)) {
        if (err)
            *err = "Truncated message";
        return false;
    }
    auto it = by_id_.find((int32_t)msgid);
    if (it == by_id_.end()) {
        out->name = "#unknown";
        out->output = "msgid " + std::to_string(msgid);
        *posp = end; // length unknown: skip the rest of the block
        return true;
    }
    const MsgFormat *mf = it->second;
    out->fmt = mf;
    out->name = mf->name;
    out->ints.assign(mf->params.size(), 0);
    out->bufs.assign(mf->params.size(), std::string());
    for (size_t i = 0; i < mf->params.size(); i++) {
        const MsgParam &p = mf->params[i];
        if (p.is_int()) {
            if (!parse_int(s, end, &pos, is_signed_type(p.type), &out->ints[i]))
                goto truncated;
            if (p.enums) {
                const auto &rev = reverse_enums_.at(p.enum_name);
                auto r = rev.find(out->ints[i]);
                out->bufs[i] = r != rev.end()
                    ? r->second : "?" + std::to_string(out->ints[i]);
            }
        } else {
            if (pos >= end)
                goto truncated;
            int l = s[pos++];
            if (pos + l > end)
                goto truncated;
            out->bufs[i].assign((const char *)&s[pos], l);
            pos += l;
        }
    }
    if (mf->kind == MsgFormat::Kind::Output) {
        // Format "#output" text with %s substituted values
        std::string txt;
        const std::string &f = mf->format;
        size_t pi = 0;
        for (size_t i = 0; i < f.size(); i++) {
            if (f[i] == '%' && i + 1 < f.size() && f[i + 1] == '%') {
                txt += '%';
                i++;
                continue;
            }
            if (f[i] == '%' && pi < mf->params.size()) {
                const MsgParam &p = mf->params[pi];
                txt += p.is_int() ? std::to_string(out->ints[pi])
                                  : "'" + out->bufs[pi] + "'";
                static const size_t lens[] = {2, 2, 3, 3, 2, 2, 4, 3};
                i += lens[(int)p.type] - 1;
                pi++;
                continue;
            }
            txt += f[i];
        }
        out->output = txt;
        out->fmt = nullptr;
    }
    *posp = pos;
    return true;
truncated:
    if (err)
        *err = "Truncated message " + mf->name;
    return false;
}

bool
MessageParser::has_constant(const std::string &name) const
{
    return config_.count(name) != 0;
}

std::string
MessageParser::get_constant(const std::string &name,
                            const std::string &def) const
{
    auto it = config_.find(name);
    return it == config_.end() ? def : it->second;
}

double
MessageParser::get_constant_float(const std::string &name, double def) const
{
    auto it = config_.find(name);
    return it == config_.end() ? def : strtod(it->second.c_str(), nullptr);
}

int64_t
MessageParser::get_constant_int(const std::string &name, int64_t def) const
{
    auto it = config_.find(name);
    return it == config_.end() ? def : strtoll(it->second.c_str(), nullptr, 0);
}

} // namespace kl
