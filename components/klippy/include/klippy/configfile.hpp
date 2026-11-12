// Port of klippy/configfile.py: printer.cfg reading.
//
// Follows Python configparser as Klipper configures it (strict=False,
// inline_comment_prefixes=('#', ';')): duplicate sections merge with
// later values winning, option names are case-insensitive, indented lines
// continue the previous value, comment lines inside values are dropped.
// The "#*# SAVE_CONFIG" block at the end is parsed and overrides the
// regular config, and [include file] is resolved through a callback.
//
// Errors do not throw: the first error is recorded (ConfigFile::error())
// and getters return their default.
//
// This file may be distributed under the terms of the GNU GPLv3 license.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace kl {

class ConfigFile;

class ConfigSection {
public:
    ConfigSection(ConfigFile *cf, std::string name)
        : cf_(cf), name_(std::move(name)) {}
    const std::string &get_name() const { return name_; }
    bool has(const std::string &option) const;

    // A missing option without a default is an error (Klipper "must be
    // specified"); pass a default to make it optional.
    std::string get(const std::string &option) const;
    std::string get(const std::string &option, const std::string &def) const;
    double getfloat(const std::string &option) const;
    double getfloat(const std::string &option, double def, double minval = NAN_,
                    double maxval = NAN_, double above = NAN_,
                    double below = NAN_) const;
    int64_t getint(const std::string &option) const;
    int64_t getint(const std::string &option, int64_t def,
                   int64_t minval = INT64_MIN_, int64_t maxval = INT64_MAX_) const;
    bool getboolean(const std::string &option, bool def) const;
    std::string getchoice(const std::string &option,
                          const std::vector<std::string> &choices,
                          const std::string &def) const;
    std::vector<std::string> getlist(const std::string &option,
                                     const std::vector<std::string> &def,
                                     char sep = ',') const;
    std::vector<std::string> options() const;
    std::vector<std::string> prefix_options(const std::string &prefix) const;

    static constexpr double NAN_ = __builtin_nan("");
    static constexpr int64_t INT64_MIN_ = -0x7fffffffffffffffLL - 1;
    static constexpr int64_t INT64_MAX_ = 0x7fffffffffffffffLL;

private:
    const std::string *raw(const std::string &option, bool note_valid) const;
    ConfigFile *cf_;
    std::string name_;
};

class ConfigFile {
public:
    // Reads an [include] file by path; returns false if missing
    using IncludeReader = std::function<bool(const std::string &path,
                                             std::string *data)>;

    // Parse printer.cfg text.  'filename' is used to resolve includes.
    bool load(const std::string &data, const std::string &filename = "",
              IncludeReader reader = nullptr);

    bool has_section(const std::string &name) const;
    ConfigSection getsection(const std::string &name);
    std::vector<ConfigSection> get_prefix_sections(const std::string &prefix);
    std::vector<std::string> sections() const;

    // Error handling
    void set_error(const std::string &msg);
    bool has_error() const { return !error_.empty(); }
    const std::string &error() const { return error_; }
    // Options set in the file that no module read (Klipper reports these
    // as errors; here they become warnings for unsupported features)
    std::vector<std::string> unused_options() const;

    // Raw access (used by ConfigSection)
    const std::string *lookup(const std::string &section,
                              const std::string &option, bool note_valid);
    const std::map<std::string, std::string> *section_data(
        const std::string &section) const;

private:
    bool parse(const std::string &data, const std::string &filename,
               IncludeReader &reader, int depth);
    std::vector<std::string> order_;
    std::map<std::string, std::map<std::string, std::string>> data_;
    std::map<std::string, std::map<std::string, bool>> accessed_;
    std::string error_;
};

} // namespace kl
