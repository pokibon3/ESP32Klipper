// Minimal JSON reader for the Klipper MCU data dictionary.
//
// The dictionary is parsed once at connect time and then discarded, so
// this favours small code over speed.  Numbers are kept as int64 when
// they are integral (all dictionary values are), else as double.
//
// This file may be distributed under the terms of the GNU GPLv3 license.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace kl {

struct JsonValue {
    enum class Type : uint8_t { Null, Bool, Int, Double, String, Array, Object };
    Type type = Type::Null;
    bool b = false;
    int64_t i = 0;
    double d = 0.;
    std::string s;
    std::vector<JsonValue> arr;
    std::vector<std::pair<std::string, JsonValue>> obj;

    bool is_object() const { return type == Type::Object; }
    bool is_array() const { return type == Type::Array; }
    bool is_string() const { return type == Type::String; }
    bool is_int() const { return type == Type::Int; }
    // Object member lookup (nullptr if missing or not an object)
    const JsonValue *get(const char *key) const;
    // Value as text (strings as-is, numbers formatted) for constants
    std::string as_text() const;
};

// Parse 'len' bytes of JSON.  Returns false (and sets *err) on error.
bool json_parse(const char *text, size_t len, JsonValue *out, std::string *err);

} // namespace kl
