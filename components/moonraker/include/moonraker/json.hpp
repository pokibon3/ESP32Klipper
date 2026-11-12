// JSON for the Moonraker API, on the JValue type of the G-code templates
// (status objects are JValues already).
//
// This file may be distributed under the terms of the GNU GPLv3 license.
#pragma once

#include <string>

#include "klippy/jinja.hpp"

namespace kl {

// Append v as JSON (undefined -> null, NaN/inf -> null)
void json_write(std::string &out, const JValue &v);
std::string json_dump(const JValue &v);
void json_write_string(std::string &out, const std::string &s);
// Parse JSON text into a JValue (objects -> dict, arrays -> list)
bool json_load(const std::string &text, JValue *out, std::string *err);

// Helpers for building replies
JValue jdict(std::initializer_list<std::pair<const char *, JValue>> items);
inline JValue jstr(const std::string &s) { return JValue::str(s); }
inline JValue jint(int64_t i) { return JValue::integer(i); }
inline JValue jnum(double d) { return JValue::number(d); }
inline JValue jbool(bool b) { return JValue::boolean(b); }
// Member lookup in a dict (nullptr if missing or not a dict)
const JValue *jget(const JValue &d, const char *key);
std::string jget_str(const JValue &d, const char *key,
                     const std::string &def = "");

} // namespace kl
