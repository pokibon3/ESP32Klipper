// Port of klippy/msgproto.py: the Klipper host <-> MCU message format.
//
// A message is a VLQ encoded message id followed by its parameters.  The
// ids and parameter formats come from the MCU's data dictionary, which
// is read with "identify" at connect time (zlib compressed JSON).
//
// This file may be distributed under the terms of the GNU GPLv3 license.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace kl {

constexpr int MESSAGE_MIN = 5;
constexpr int MESSAGE_MAX = 64;
constexpr int MESSAGE_HEADER_SIZE = 2;
constexpr int MESSAGE_TRAILER_SIZE = 3;
constexpr int MESSAGE_PAYLOAD_MAX = MESSAGE_MAX - MESSAGE_MIN;

enum class ParamType : uint8_t {
    U32, I32, U16, I16, Byte,    // integers (VLQ)
    String, ProgmemBuffer, Buffer // length prefixed bytes
};

struct MsgParam {
    std::string name;
    ParamType type;
    // Enumeration applied to this parameter ("pin" for step_pin=...)
    const std::map<std::string, int64_t> *enums = nullptr;
    std::string enum_name;
    bool is_int() const { return type <= ParamType::Byte; }
};

struct MsgFormat {
    int32_t msgid = 0;
    std::string format;       // full format string from the dictionary
    std::string name;         // first word
    enum class Kind : uint8_t { Command, Response, Output } kind;
    std::vector<MsgParam> params;
    int param_index(const std::string &pname) const;
};

// A decoded message ("params" dict in klippy)
struct MsgParams {
    const MsgFormat *fmt = nullptr;   // nullptr for unknown messages
    std::string name;                 // '#name' ("#unknown" / "#output")
    std::vector<int64_t> ints;        // per parameter (0 for buffers)
    std::vector<std::string> bufs;    // per parameter (empty for ints)
    std::string output;               // formatted text for "#output"
    double sent_time = 0., receive_time = 0.;

    bool has(const char *pname) const;
    int64_t get(const char *pname, int64_t def = 0) const;
    // Buffer parameter, or the enumeration name of an int parameter
    std::string get_str(const char *pname) const;
    std::string to_string() const;
};

// One command argument for MessageParser::encode()
struct MsgArg {
    int64_t i = 0;
    std::string s;     // buffer data, or an enumeration value name
    bool is_str = false;
    template <typename T,
              typename = std::enable_if_t<std::is_integral<T>::value>>
    MsgArg(T v) : i((int64_t)v) {}
    MsgArg(const char *v) : s(v), is_str(true) {}
    MsgArg(std::string v) : s(std::move(v)), is_str(true) {}
};

class MessageParser {
public:
    MessageParser();

    // Load the (already decompressed) JSON data dictionary
    bool process_identify(const std::string &json, std::string *err);

    // klippy MessageParser.lookup_command(): the format must match exactly
    // (this is how Klipper detects incompatible firmware versions)
    const MsgFormat *lookup_command(const std::string &format,
                                    std::string *err = nullptr) const;
    const MsgFormat *lookup_name(const std::string &name) const;

    // Encode a command from "name a=1 pin=gpio11 data=0a0b" text
    bool create_command(const std::string &text, std::vector<uint8_t> *out,
                        std::string *err) const;
    // Encode a command from positional arguments
    bool encode(const MsgFormat &fmt, const std::vector<MsgArg> &args,
                std::vector<uint8_t> *out, std::string *err) const;

    // Decode one received message block (header + payload + trailer)
    bool parse(const uint8_t *msg, int len, MsgParams *out,
               std::string *err) const;
    // Text of every message in a block (klippy MessageParser.dump())
    std::vector<std::string> dump(const uint8_t *msg, int len) const;

    // Constants from the dictionary "config" section
    bool has_constant(const std::string &name) const;
    std::string get_constant(const std::string &name,
                             const std::string &def = "") const;
    double get_constant_float(const std::string &name, double def = 0.) const;
    int64_t get_constant_int(const std::string &name, int64_t def = 0) const;
    const std::map<std::string, std::string> &constants() const {
        return config_;
    }
    bool lookup_enum(const std::string &enum_name, const std::string &value,
                     int64_t *out) const;
    const std::string &version() const { return version_; }
    const std::string &build_versions() const { return build_versions_; }
    size_t message_count() const { return messages_.size(); }

    // VLQ helpers (msgproto.py PT_uint32)
    static void encode_int(std::vector<uint8_t> &out, int64_t v);
    static bool parse_int(const uint8_t *s, int len, int *pos, bool is_signed,
                          int64_t *v);

private:
    // Decode one message at *pos (advanced past it); end = trailer offset
    bool parse_at(const uint8_t *s, int end, int *pos, MsgParams *out,
                  std::string *err) const;
    void add_message(const std::string &format, int32_t msgid,
                     MsgFormat::Kind kind);
    bool parse_params(const std::string &format, MsgFormat *mf,
                      std::string *err);

    std::vector<std::unique_ptr<MsgFormat>> messages_;
    std::map<int32_t, MsgFormat *> by_id_;
    std::map<std::string, MsgFormat *> by_name_;
    std::map<std::string, std::map<std::string, int64_t>> enumerations_;
    std::map<std::string, std::map<int64_t, std::string>> reverse_enums_;
    std::map<std::string, std::string> config_;
    std::string version_, build_versions_;
};

// zlib inflate (ROM miniz on ESP32, system zlib on the host)
bool zlib_inflate(const uint8_t *data, size_t len, std::string *out);

} // namespace kl
