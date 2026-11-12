// Minimal JSON reader (see json_lite.hpp).
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "mcu_link/json_lite.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace kl {

const JsonValue *
JsonValue::get(const char *key) const
{
    if (type != Type::Object)
        return nullptr;
    for (const auto &kv : obj)
        if (kv.first == key)
            return &kv.second;
    return nullptr;
}

std::string
JsonValue::as_text() const
{
    char buf[32];
    switch (type) {
    case Type::String: return s;
    case Type::Int:
        snprintf(buf, sizeof(buf), "%lld", (long long)i);
        return buf;
    case Type::Double:
        snprintf(buf, sizeof(buf), "%.17g", d);
        return buf;
    case Type::Bool: return b ? "true" : "false";
    default: return "";
    }
}

namespace {

struct Parser {
    const char *p, *end;
    std::string err;

    void ws() {
        while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t'))
            p++;
    }
    bool fail(const char *msg) {
        if (err.empty())
            err = msg;
        return false;
    }
    bool lit(const char *word) {
        size_t n = strlen(word);
        if ((size_t)(end - p) < n || memcmp(p, word, n) != 0)
            return fail("bad literal");
        p += n;
        return true;
    }
    static void put_utf8(std::string &out, uint32_t cp) {
        if (cp < 0x80) {
            out += (char)cp;
        } else if (cp < 0x800) {
            out += (char)(0xc0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3f));
        } else if (cp < 0x10000) {
            out += (char)(0xe0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3f));
            out += (char)(0x80 | (cp & 0x3f));
        } else {
            out += (char)(0xf0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3f));
            out += (char)(0x80 | ((cp >> 6) & 0x3f));
            out += (char)(0x80 | (cp & 0x3f));
        }
    }
    bool hex4(uint32_t *v) {
        if (end - p < 4)
            return fail("bad escape");
        *v = 0;
        for (int i = 0; i < 4; i++) {
            char c = *p++;
            *v <<= 4;
            if (c >= '0' && c <= '9') *v |= c - '0';
            else if (c >= 'a' && c <= 'f') *v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') *v |= c - 'A' + 10;
            else return fail("bad escape");
        }
        return true;
    }
    bool string(std::string &out) {
        p++; // opening quote
        while (p < end && *p != '"') {
            char c = *p++;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (p >= end)
                return fail("bad escape");
            c = *p++;
            switch (c) {
            case '"': case '\\': case '/': out += c; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                uint32_t cp;
                if (!hex4(&cp))
                    return false;
                if (cp >= 0xd800 && cp < 0xdc00 && end - p >= 6 && p[0] == '\\'
                    && p[1] == 'u') {
                    p += 2;
                    uint32_t lo;
                    if (!hex4(&lo))
                        return false;
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                }
                put_utf8(out, cp);
                break;
            }
            default: return fail("bad escape");
            }
        }
        if (p >= end)
            return fail("unterminated string");
        p++;
        return true;
    }
    bool number(JsonValue &v) {
        const char *start = p;
        bool is_float = false;
        if (p < end && *p == '-')
            p++;
        while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e'
                           || *p == 'E' || *p == '+' || *p == '-')) {
            if (*p == '.' || *p == 'e' || *p == 'E')
                is_float = true;
            p++;
        }
        std::string tok(start, p - start);
        if (tok.empty() || tok == "-")
            return fail("bad number");
        if (is_float) {
            v.type = JsonValue::Type::Double;
            v.d = strtod(tok.c_str(), nullptr);
        } else {
            v.type = JsonValue::Type::Int;
            v.i = strtoll(tok.c_str(), nullptr, 10);
            v.d = (double)v.i;
        }
        return true;
    }
    bool value(JsonValue &v, int depth) {
        if (depth > 32)
            return fail("nesting too deep");
        ws();
        if (p >= end)
            return fail("unexpected end");
        char c = *p;
        if (c == '{') {
            p++;
            v.type = JsonValue::Type::Object;
            ws();
            if (p < end && *p == '}') {
                p++;
                return true;
            }
            for (;;) {
                ws();
                if (p >= end || *p != '"')
                    return fail("expected key");
                std::string key;
                if (!string(key))
                    return false;
                ws();
                if (p >= end || *p != ':')
                    return fail("expected ':'");
                p++;
                v.obj.emplace_back(std::move(key), JsonValue());
                if (!value(v.obj.back().second, depth + 1))
                    return false;
                ws();
                if (p < end && *p == ',') {
                    p++;
                    continue;
                }
                if (p < end && *p == '}') {
                    p++;
                    return true;
                }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            p++;
            v.type = JsonValue::Type::Array;
            ws();
            if (p < end && *p == ']') {
                p++;
                return true;
            }
            for (;;) {
                v.arr.emplace_back();
                if (!value(v.arr.back(), depth + 1))
                    return false;
                ws();
                if (p < end && *p == ',') {
                    p++;
                    continue;
                }
                if (p < end && *p == ']') {
                    p++;
                    return true;
                }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            v.type = JsonValue::Type::String;
            return string(v.s);
        }
        if (c == 't') {
            v.type = JsonValue::Type::Bool;
            v.b = true;
            return lit("true");
        }
        if (c == 'f') {
            v.type = JsonValue::Type::Bool;
            return lit("false");
        }
        if (c == 'n') {
            v.type = JsonValue::Type::Null;
            return lit("null");
        }
        return number(v);
    }
};

} // namespace

bool
json_parse(const char *text, size_t len, JsonValue *out, std::string *err)
{
    Parser ps{text, text + len, {}};
    *out = JsonValue();
    if (!ps.value(*out, 0)) {
        if (err)
            *err = "JSON: " + ps.err;
        return false;
    }
    return true;
}

} // namespace kl
