// JSON for the Moonraker API (see json.hpp).
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "moonraker/json.hpp"

#include <cmath>
#include <cstdio>

#include "mcu_link/json_lite.hpp"

namespace kl {

void
json_write_string(std::string &out, const std::string &s)
{
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += (char)c;
            }
        }
    }
    out += '"';
}

void
json_write(std::string &out, const JValue &v)
{
    char buf[32];
    switch (v.t) {
    case JValue::T::Bool:
        out += v.b ? "true" : "false";
        break;
    case JValue::T::Int:
        snprintf(buf, sizeof(buf), "%lld", (long long)v.i);
        out += buf;
        break;
    case JValue::T::Float:
        if (!std::isfinite(v.f)) {
            out += "null";
        } else {
            snprintf(buf, sizeof(buf), "%.10g", v.f);
            out += buf;
        }
        break;
    case JValue::T::Str:
        json_write_string(out, v.s);
        break;
    case JValue::T::List: {
        out += '[';
        bool first = true;
        if (v.list)
            for (const auto &e : *v.list) {
                if (!first)
                    out += ',';
                first = false;
                json_write(out, e);
            }
        out += ']';
        break;
    }
    case JValue::T::Dict: {
        out += '{';
        bool first = true;
        if (v.dict)
            for (const auto &kv : v.dict->items) {
                if (!first)
                    out += ',';
                first = false;
                json_write_string(out, kv.first);
                out += ':';
                json_write(out, kv.second);
            }
        out += '}';
        break;
    }
    default:
        out += "null";
        break;
    }
}

std::string
json_dump(const JValue &v)
{
    std::string s;
    json_write(s, v);
    return s;
}

namespace {
JValue
convert(const JsonValue &j)
{
    switch (j.type) {
    case JsonValue::Type::Bool: return JValue::boolean(j.b);
    case JsonValue::Type::Int: return JValue::integer(j.i);
    case JsonValue::Type::Double: return JValue::number(j.d);
    case JsonValue::Type::String: return JValue::str(j.s);
    case JsonValue::Type::Array: {
        JList l;
        l.reserve(j.arr.size());
        for (const auto &e : j.arr)
            l.push_back(convert(e));
        return JValue::make_list(std::move(l));
    }
    case JsonValue::Type::Object: {
        JValue d = JValue::make_dict();
        for (const auto &kv : j.obj)
            d.dict->set(kv.first, convert(kv.second));
        return d;
    }
    default: return JValue::none();
    }
}
} // namespace

bool
json_load(const std::string &text, JValue *out, std::string *err)
{
    JsonValue j;
    if (!json_parse(text.data(), text.size(), &j, err))
        return false;
    *out = convert(j);
    return true;
}

JValue
jdict(std::initializer_list<std::pair<const char *, JValue>> items)
{
    JValue d = JValue::make_dict();
    for (const auto &kv : items)
        d.dict->set(kv.first, kv.second);
    return d;
}

const JValue *
jget(const JValue &d, const char *key)
{
    if (d.t != JValue::T::Dict || !d.dict)
        return nullptr;
    return d.dict->find(key);
}

std::string
jget_str(const JValue &d, const char *key, const std::string &def)
{
    const JValue *v = jget(d, key);
    if (!v || v->is_undef() || v->t == JValue::T::None)
        return def;
    return v->t == JValue::T::Str ? v->s : v->to_str();
}

} // namespace kl
