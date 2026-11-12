// Port of klippy/configfile.py (see configfile.hpp).
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "klippy/configfile.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>

namespace kl {

static const char AUTOSAVE_HEADER[] =
    "\n#*# <---------------------- SAVE_CONFIG ---------------------->\n"
    "#*# DO NOT EDIT THIS BLOCK OR BELOW. The contents are auto-generated.\n"
    "#*#\n";

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
lower(std::string s)
{
    for (auto &c : s)
        c = (char)tolower((unsigned char)c);
    return s;
}

// configparser inline comments: '#' or ';' preceded by whitespace
static std::string
strip_inline_comment(const std::string &line)
{
    for (size_t i = 1; i < line.size(); i++)
        if ((line[i] == '#' || line[i] == ';')
            && isspace((unsigned char)line[i - 1]))
            return line.substr(0, i);
    return line;
}

static std::string
dirname_of(const std::string &path)
{
    size_t p = path.rfind('/');
    return p == std::string::npos ? "" : path.substr(0, p + 1);
}

/****************************************************************
 * Parsing
 ****************************************************************/

bool
ConfigFile::parse(const std::string &data, const std::string &filename,
                  IncludeReader &reader, int depth)
{
    if (depth > 8) {
        set_error("Recursive include of config file '" + filename + "'");
        return false;
    }
    std::istringstream in(data);
    std::string line, section;
    std::string *cur_value = nullptr;
    size_t cur_indent = 0;
    std::vector<std::string> pending_empty;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        std::string s = strip(line);
        // Full line comments are dropped (also inside multi-line values)
        if (!s.empty() && (s[0] == '#' || s[0] == ';'))
            continue;
        std::string value = strip(strip_inline_comment(line));
        size_t indent = line.find_first_not_of(" \t");
        if (value.empty()) {
            // Blank lines may be part of a multi-line value
            if (cur_value)
                pending_empty.push_back("");
            continue;
        }
        if (cur_value && indent != std::string::npos && indent > cur_indent) {
            // Continuation of a multi-line value
            for (auto &e : pending_empty)
                *cur_value += "\n";
            pending_empty.clear();
            if (!cur_value->empty())
                *cur_value += "\n";
            *cur_value += value;
            continue;
        }
        pending_empty.clear();
        cur_value = nullptr;
        if (value[0] == '[') {
            size_t close = value.rfind(']');
            if (close == std::string::npos) {
                set_error("Bad section header '" + value + "' in " + filename);
                return false;
            }
            std::string header = strip(value.substr(1, close - 1));
            if (header.compare(0, 8, "include ") == 0) {
                std::string spec = strip(header.substr(8));
                std::string path = dirname_of(filename) + spec;
                std::string inc;
                if (!reader || !reader(path, &inc)) {
                    set_error("Include file '" + path + "' does not exist");
                    return false;
                }
                if (!parse(inc, path, reader, depth + 1))
                    return false;
                section.clear();
                continue;
            }
            section = header;
            if (!data_.count(section))
                order_.push_back(section);
            data_[section];
            continue;
        }
        if (section.empty()) {
            set_error("Option outside of a section in " + filename + ": "
                      + value);
            return false;
        }
        size_t sep = value.find_first_of(":=");
        if (sep == std::string::npos) {
            set_error("Bad line in section [" + section + "]: " + value);
            return false;
        }
        std::string key = lower(strip(value.substr(0, sep)));
        cur_value = &data_[section][key];
        *cur_value = strip(value.substr(sep + 1));
        cur_indent = indent == std::string::npos ? 0 : indent;
    }
    return true;
}

bool
ConfigFile::load(const std::string &data, const std::string &filename,
                 IncludeReader reader)
{
    // Split off the SAVE_CONFIG block (ConfigAutoSave._find_autosave_data)
    std::string regular = data, autosave;
    size_t pos = data.find(AUTOSAVE_HEADER);
    if (pos != std::string::npos) {
        regular = data.substr(0, pos);
        std::istringstream in(data.substr(pos + sizeof(AUTOSAVE_HEADER) - 1));
        std::string line;
        bool ok = true;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty())
                continue;
            if (line.compare(0, 3, "#*#") != 0
                || (line.size() >= 4 && line.compare(0, 4, "#*# ") != 0)) {
                ok = false; // modified after the header: ignore autosave
                break;
            }
            autosave += (line.size() > 4 ? line.substr(4) : "") + "\n";
        }
        if (!ok)
            autosave.clear();
    }
    if (!parse(regular, filename, reader, 0))
        return false;
    // Autosave values override the regular config
    return parse(autosave, filename, reader, 0);
}

/****************************************************************
 * Access
 ****************************************************************/

void
ConfigFile::set_error(const std::string &msg)
{
    if (error_.empty())
        error_ = msg;
}

bool
ConfigFile::has_section(const std::string &name) const
{
    return data_.count(name) != 0;
}

ConfigSection
ConfigFile::getsection(const std::string &name)
{
    return ConfigSection(this, name);
}

std::vector<ConfigSection>
ConfigFile::get_prefix_sections(const std::string &prefix)
{
    std::vector<ConfigSection> out;
    for (const auto &s : order_)
        if (s.compare(0, prefix.size(), prefix) == 0)
            out.emplace_back(this, s);
    return out;
}

std::vector<std::string>
ConfigFile::sections() const
{
    return order_;
}

const std::string *
ConfigFile::lookup(const std::string &section, const std::string &option,
                   bool note_valid)
{
    auto s = data_.find(section);
    if (s == data_.end())
        return nullptr;
    std::string key = lower(option);
    auto o = s->second.find(key);
    if (o == s->second.end())
        return nullptr;
    if (note_valid)
        accessed_[section][key] = true;
    return &o->second;
}

const std::map<std::string, std::string> *
ConfigFile::section_data(const std::string &section) const
{
    auto s = data_.find(section);
    return s == data_.end() ? nullptr : &s->second;
}

std::vector<std::string>
ConfigFile::unused_options() const
{
    std::vector<std::string> out;
    for (const auto &sec : order_) {
        auto acc = accessed_.find(sec);
        for (const auto &kv : data_.at(sec)) {
            if (acc != accessed_.end() && acc->second.count(kv.first))
                continue;
            out.push_back("[" + sec + "] " + kv.first);
        }
    }
    return out;
}

/****************************************************************
 * ConfigSection getters
 ****************************************************************/

const std::string *
ConfigSection::raw(const std::string &option, bool note_valid) const
{
    return cf_->lookup(name_, option, note_valid);
}

bool
ConfigSection::has(const std::string &option) const
{
    return raw(option, false) != nullptr;
}

std::string
ConfigSection::get(const std::string &option) const
{
    const std::string *v = raw(option, true);
    if (!v) {
        cf_->set_error("Option '" + option + "' in section '" + name_
                       + "' must be specified");
        return "";
    }
    return *v;
}

std::string
ConfigSection::get(const std::string &option, const std::string &def) const
{
    const std::string *v = raw(option, true);
    return v ? *v : def;
}

static bool
parse_double(const std::string &s, double *out)
{
    if (s.empty())
        return false;
    char *end;
    *out = strtod(s.c_str(), &end);
    return *end == '\0';
}

double
ConfigSection::getfloat(const std::string &option) const
{
    if (!raw(option, false)) {
        cf_->set_error("Option '" + option + "' in section '" + name_
                       + "' must be specified");
        return 0.;
    }
    return getfloat(option, 0.);
}

double
ConfigSection::getfloat(const std::string &option, double def, double minval,
                        double maxval, double above, double below) const
{
    const std::string *v = raw(option, true);
    if (!v)
        return def;
    double d;
    if (!parse_double(*v, &d)) {
        cf_->set_error("Unable to parse option '" + option + "' in section '"
                       + name_ + "'");
        return def;
    }
    char buf[160];
    if (!std::isnan(minval) && d < minval) {
        snprintf(buf, sizeof(buf), "must have minimum of %g", minval);
    } else if (!std::isnan(maxval) && d > maxval) {
        snprintf(buf, sizeof(buf), "must have maximum of %g", maxval);
    } else if (!std::isnan(above) && d <= above) {
        snprintf(buf, sizeof(buf), "must be above %g", above);
    } else if (!std::isnan(below) && d >= below) {
        snprintf(buf, sizeof(buf), "must be below %g", below);
    } else {
        return d;
    }
    cf_->set_error("Option '" + option + "' in section '" + name_ + "' "
                   + buf);
    return def;
}

int64_t
ConfigSection::getint(const std::string &option) const
{
    if (!raw(option, false)) {
        cf_->set_error("Option '" + option + "' in section '" + name_
                       + "' must be specified");
        return 0;
    }
    return getint(option, 0);
}

int64_t
ConfigSection::getint(const std::string &option, int64_t def, int64_t minval,
                      int64_t maxval) const
{
    const std::string *v = raw(option, true);
    if (!v)
        return def;
    char *end;
    long long i = strtoll(v->c_str(), &end, 10);
    if (v->empty() || *end) {
        cf_->set_error("Unable to parse option '" + option + "' in section '"
                       + name_ + "'");
        return def;
    }
    if (i < minval || i > maxval) {
        cf_->set_error("Option '" + option + "' in section '" + name_
                       + "' is out of range");
        return def;
    }
    return i;
}

bool
ConfigSection::getboolean(const std::string &option, bool def) const
{
    const std::string *v = raw(option, true);
    if (!v)
        return def;
    std::string l = lower(*v);
    if (l == "1" || l == "yes" || l == "true" || l == "on")
        return true;
    if (l == "0" || l == "no" || l == "false" || l == "off")
        return false;
    cf_->set_error("Unable to parse option '" + option + "' in section '"
                   + name_ + "'");
    return def;
}

std::string
ConfigSection::getchoice(const std::string &option,
                         const std::vector<std::string> &choices,
                         const std::string &def) const
{
    std::string c = get(option, def);
    if (std::find(choices.begin(), choices.end(), c) == choices.end()) {
        cf_->set_error("Choice '" + c + "' for option '" + option
                       + "' in section '" + name_ + "' is not a valid choice");
        return def;
    }
    return c;
}

std::vector<std::string>
ConfigSection::getlist(const std::string &option,
                       const std::vector<std::string> &def, char sep) const
{
    const std::string *v = raw(option, true);
    if (!v)
        return def;
    std::vector<std::string> out;
    if (strip(*v).empty())
        return out;
    std::string part;
    std::istringstream in(*v);
    while (std::getline(in, part, sep))
        out.push_back(strip(part));
    return out;
}

std::vector<std::string>
ConfigSection::options() const
{
    std::vector<std::string> out;
    if (const auto *d = cf_->section_data(name_))
        for (const auto &kv : *d)
            out.push_back(kv.first);
    return out;
}

std::vector<std::string>
ConfigSection::prefix_options(const std::string &prefix) const
{
    std::vector<std::string> out;
    for (const auto &o : options())
        if (o.compare(0, prefix.size(), prefix) == 0)
            out.push_back(o);
    return out;
}

} // namespace kl
