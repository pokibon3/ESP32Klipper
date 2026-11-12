// A small Jinja2 subset for Klipper G-code macros.
//
// Klipper renders macros with jinja2.Environment('{%', '%}', '{', '}'):
// expressions use single braces.  Supported: {% if/elif/else %},
// {% for x in ... %} (with loop.*, tuple unpacking, else), {% set %},
// {# comments #}, whitespace control (-), expressions with Python
// semantics (int/float/str/list/dict/None/bool, + - * / // % **, string
// % formatting, comparisons, in / not in, and/or/not, ternary,
// attribute/item access, calls), common filters (default, float, int,
// string, lower, upper, round, abs, length, join, list, min, max, sum,
// replace, trim) and string/dict methods.  Values print like Python str().
//
// This file may be distributed under the terms of the GNU GPLv3 license.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace kl {

struct JValue;
using JList = std::vector<JValue>;
struct JDict;
struct JObject;
using JFunc = std::function<bool(const std::vector<JValue> &args,
                                 const std::vector<std::pair<std::string,
                                                             JValue>> &kwargs,
                                 JValue *ret, std::string *err)>;

struct JValue {
    enum class T : uint8_t { Undef, None, Bool, Int, Float, Str, List, Dict,
                             Object, Func };
    T t = T::Undef;
    bool b = false;
    int64_t i = 0;
    double f = 0.;
    std::string s;
    std::shared_ptr<JList> list;
    std::shared_ptr<JDict> dict;
    std::shared_ptr<JObject> obj;
    std::shared_ptr<JFunc> func;
    std::string undef_name; // for "'x' is undefined" messages

    static JValue none() { JValue v; v.t = T::None; return v; }
    static JValue boolean(bool x) { JValue v; v.t = T::Bool; v.b = x; return v; }
    static JValue integer(int64_t x) { JValue v; v.t = T::Int; v.i = x; return v; }
    static JValue number(double x) { JValue v; v.t = T::Float; v.f = x; return v; }
    static JValue str(std::string x) { JValue v; v.t = T::Str; v.s = std::move(x); return v; }
    static JValue make_list(JList l = {});
    static JValue make_dict();
    static JValue function(JFunc fn);
    static JValue undefined(std::string name = "") {
        JValue v; v.undef_name = std::move(name); return v;
    }

    bool is_undef() const { return t == T::Undef; }
    bool truthy() const;
    std::string to_str() const;   // Python str()
    std::string repr() const;     // Python repr()
};

// Ordered dict with string keys (Python dict insertion order)
struct JDict {
    std::vector<std::pair<std::string, JValue>> items;
    const JValue *find(const std::string &k) const;
    JValue *find(const std::string &k);
    void set(const std::string &k, JValue v);
};

// Lazily evaluated object (e.g. the "printer" status tree)
struct JObject {
    std::function<JValue(const std::string &)> get;
    std::function<std::vector<std::string>()> keys;
};

struct JNode;

class JTemplate {
public:
    JTemplate();
    ~JTemplate();
    bool parse(const std::string &source, std::string *err);
    // ctx: global variables (params, printer, actions, macro variables)
    bool render(const JDict &ctx, std::string *out, std::string *err) const;
    // Render with layered globals, looked up first to last (e.g. params,
    // macro variables, shared printer/action context) - nothing is copied
    bool render(const std::vector<const JDict *> &layers, std::string *out,
                std::string *err) const;
    // Evaluate one Python literal / expression (gcode_macro variable_*)
    static bool eval_expression(const std::string &expr, JValue *out,
                                std::string *err);

private:
    std::shared_ptr<JNode> root_;
};

} // namespace kl
