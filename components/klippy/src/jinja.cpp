// A small Jinja2 subset for Klipper G-code macros (see jinja.hpp).
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "klippy/jinja.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace kl {

/****************************************************************
 * Values
 ****************************************************************/

JValue
JValue::make_list(JList l)
{
    JValue v;
    v.t = T::List;
    v.list = std::make_shared<JList>(std::move(l));
    return v;
}

JValue
JValue::make_dict()
{
    JValue v;
    v.t = T::Dict;
    v.dict = std::make_shared<JDict>();
    return v;
}

JValue
JValue::function(JFunc fn)
{
    JValue v;
    v.t = T::Func;
    v.func = std::make_shared<JFunc>(std::move(fn));
    return v;
}

const JValue *
JDict::find(const std::string &k) const
{
    for (const auto &kv : items)
        if (kv.first == k)
            return &kv.second;
    return nullptr;
}

JValue *
JDict::find(const std::string &k)
{
    for (auto &kv : items)
        if (kv.first == k)
            return &kv.second;
    return nullptr;
}

void
JDict::set(const std::string &k, JValue v)
{
    if (JValue *e = find(k))
        *e = std::move(v);
    else
        items.emplace_back(k, std::move(v));
}

bool
JValue::truthy() const
{
    switch (t) {
    case T::Undef: case T::None: return false;
    case T::Bool: return b;
    case T::Int: return i != 0;
    case T::Float: return f != 0.;
    case T::Str: return !s.empty();
    case T::List: return !list->empty();
    case T::Dict: return !dict->items.empty();
    default: return true;
    }
}

// Python float repr(): shortest string that round-trips
static std::string
float_repr(double d)
{
    if (std::isnan(d))
        return "nan";
    if (std::isinf(d))
        return d > 0 ? "inf" : "-inf";
    if (d == 0.)
        return std::signbit(d) ? "-0.0" : "0.0";
    char buf[40];
    int prec;
    for (prec = 1; prec <= 17; prec++) {
        snprintf(buf, sizeof(buf), "%.*e", prec - 1, d);
        if (strtod(buf, nullptr) == d)
            break;
    }
    // buf = "[-]d.ddde[+-]XX"
    std::string m = buf;
    bool neg = m[0] == '-';
    if (neg)
        m = m.substr(1);
    size_t epos = m.find('e');
    int exp10 = atoi(m.c_str() + epos + 1);
    std::string digits;
    for (size_t k = 0; k < epos; k++)
        if (isdigit((unsigned char)m[k]))
            digits += m[k];
    while (digits.size() > 1 && digits.back() == '0')
        digits.pop_back();
    std::string out;
    if (exp10 >= -4 && exp10 < 16) {
        if (exp10 >= 0) {
            std::string ip = digits.substr(0, std::min(digits.size(),
                                                       (size_t)exp10 + 1));
            while (ip.size() < (size_t)exp10 + 1)
                ip += '0';
            std::string fp = digits.size() > (size_t)exp10 + 1
                ? digits.substr(exp10 + 1) : "0";
            out = ip + "." + fp;
        } else {
            out = "0." + std::string(-exp10 - 1, '0') + digits;
        }
    } else {
        out = digits.substr(0, 1);
        if (digits.size() > 1)
            out += "." + digits.substr(1);
        snprintf(buf, sizeof(buf), "e%c%02d", exp10 < 0 ? '-' : '+',
                 std::abs(exp10));
        out += buf;
    }
    return neg ? "-" + out : out;
}

static std::string
str_repr(const std::string &s)
{
    char q = (s.find('\'') != std::string::npos
              && s.find('"') == std::string::npos) ? '"' : '\'';
    std::string out(1, q);
    for (char c : s) {
        if (c == q || c == '\\')
            out += '\\';
        if (c == '\n')
            out += "\\n";
        else
            out += c;
    }
    return out + q;
}

std::string
JValue::repr() const
{
    if (t == T::Str)
        return str_repr(s);
    return to_str();
}

std::string
JValue::to_str() const
{
    switch (t) {
    case T::Undef: return "";
    case T::None: return "None";
    case T::Bool: return b ? "True" : "False";
    case T::Int: return std::to_string(i);
    case T::Float: return float_repr(f);
    case T::Str: return s;
    case T::List: {
        bool tuple = s == "tuple"; // see JExpr::TupleLit
        std::string out = tuple ? "(" : "[";
        for (size_t k = 0; k < list->size(); k++)
            out += (k ? ", " : "") + (*list)[k].repr();
        if (tuple && list->size() == 1)
            out += ",";
        return out + (tuple ? ")" : "]");
    }
    case T::Dict: {
        std::string out = "{";
        for (size_t k = 0; k < dict->items.size(); k++)
            out += (k ? ", " : "") + str_repr(dict->items[k].first) + ": "
                   + dict->items[k].second.repr();
        return out + "}";
    }
    case T::Object: return "<object>";
    case T::Func: return "<function>";
    }
    return "";
}

/****************************************************************
 * Expression lexer / parser
 ****************************************************************/

namespace {

struct Tok {
    enum K { End, Name, Int, Float, Str, Op } k = End;
    std::string s;
    int64_t i = 0;
    double f = 0.;
};

struct Lexer {
    const std::string &src;
    size_t pos;
    std::string err;

    bool next(Tok *t) {
        while (pos < src.size() && isspace((unsigned char)src[pos]))
            pos++;
        *t = Tok();
        if (pos >= src.size())
            return true;
        char c = src[pos];
        if (isalpha((unsigned char)c) || c == '_') {
            size_t st = pos;
            while (pos < src.size() && (isalnum((unsigned char)src[pos])
                                        || src[pos] == '_'))
                pos++;
            t->k = Tok::Name;
            t->s = src.substr(st, pos - st);
            return true;
        }
        if (isdigit((unsigned char)c)
            || (c == '.' && pos + 1 < src.size()
                && isdigit((unsigned char)src[pos + 1]))) {
            size_t st = pos;
            bool flt = false;
            while (pos < src.size()) {
                char d = src[pos];
                if (isdigit((unsigned char)d) || d == '_') {
                    pos++;
                } else if (d == '.' && !flt) {
                    flt = true;
                    pos++;
                } else if ((d == 'e' || d == 'E')) {
                    flt = true;
                    pos++;
                    if (pos < src.size() && (src[pos] == '+' || src[pos] == '-'))
                        pos++;
                } else {
                    break;
                }
            }
            std::string num = src.substr(st, pos - st);
            num.erase(std::remove(num.begin(), num.end(), '_'), num.end());
            if (flt) {
                t->k = Tok::Float;
                t->f = strtod(num.c_str(), nullptr);
            } else {
                t->k = Tok::Int;
                t->i = strtoll(num.c_str(), nullptr, 10);
            }
            return true;
        }
        if (c == '\'' || c == '"') {
            std::string out;
            pos++;
            while (pos < src.size() && src[pos] != c) {
                char d = src[pos++];
                if (d == '\\' && pos < src.size()) {
                    char e = src[pos++];
                    switch (e) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case '\\': case '\'': case '"': out += e; break;
                    default: out += '\\'; out += e; break;
                    }
                } else {
                    out += d;
                }
            }
            if (pos >= src.size()) {
                err = "unterminated string";
                return false;
            }
            pos++;
            t->k = Tok::Str;
            t->s = out;
            return true;
        }
        static const char *ops2[] = {"**", "//", "==", "!=", "<=", ">="};
        for (const char *o : ops2)
            if (src.compare(pos, 2, o) == 0) {
                t->k = Tok::Op;
                t->s = o;
                pos += 2;
                return true;
            }
        if (strchr("+-*/%()[]{},:.|<>=~", c)) {
            t->k = Tok::Op;
            t->s = std::string(1, c);
            pos++;
            return true;
        }
        err = std::string("unexpected character '") + c + "'";
        return false;
    }
};

} // namespace

struct JExpr {
    enum K { Lit, Name, Attr, Item, Slice, Call, Filter, Neg, Pos, Not, Bin,
             And, Or, Cmp, Cond, ListLit, TupleLit, DictLit, Test } k;
    std::shared_ptr<JValue> lit; // Lit only (keeps the other nodes small)
    std::string name;          // Name, Attr, Filter, Bin op, Test name
    std::vector<std::shared_ptr<JExpr>> args;  // operands / call args
    std::vector<std::pair<std::string, std::shared_ptr<JExpr>>> kwargs;
    std::vector<std::string> ops; // Cmp chain operators
    bool negate = false;          // "is not"
};
using ExprP = std::shared_ptr<JExpr>;

namespace {

struct ExprParser {
    Lexer lex;
    Tok tok;
    std::string err;
    bool stop_at_brace; // template "{ expr }" ends at a top-level '}'
    int depth = 0;

    explicit ExprParser(const std::string &s, size_t pos, bool stop)
        : lex{s, pos, {}}, stop_at_brace(stop) {}

    bool advance() {
        if (!lex.next(&tok)) {
            err = lex.err;
            return false;
        }
        return true;
    }
    bool is_op(const char *o) const { return tok.k == Tok::Op && tok.s == o; }
    bool is_name(const char *n) const { return tok.k == Tok::Name && tok.s == n; }
    bool at_end() const {
        return tok.k == Tok::End || (stop_at_brace && depth == 0 && is_op("}"));
    }
    ExprP fail(const std::string &m) {
        if (err.empty())
            err = m;
        return nullptr;
    }
    bool expect(const char *o) {
        if (!is_op(o)) {
            err = std::string("expected '") + o + "'";
            return false;
        }
        if (!strcmp(o, ")") || !strcmp(o, "]") || !strcmp(o, "}"))
            depth--;
        return advance();
    }
    static ExprP node(JExpr::K k) {
        auto e = std::make_shared<JExpr>();
        e->k = k;
        return e;
    }

    ExprP parse_expr() { return parse_cond(); }

    ExprP parse_cond() {
        ExprP a = parse_or();
        if (!a)
            return nullptr;
        if (is_name("if")) {
            if (!advance())
                return nullptr;
            ExprP c = parse_or();
            if (!c)
                return nullptr;
            ExprP e = node(JExpr::Cond);
            e->args = {c, a, nullptr};
            if (is_name("else")) {
                if (!advance())
                    return nullptr;
                ExprP b = parse_cond();
                if (!b)
                    return nullptr;
                e->args[2] = b;
            }
            return e;
        }
        return a;
    }
    ExprP parse_or() {
        ExprP a = parse_and();
        while (a && is_name("or")) {
            if (!advance())
                return nullptr;
            ExprP b = parse_and();
            if (!b)
                return nullptr;
            ExprP e = node(JExpr::Or);
            e->args = {a, b};
            a = e;
        }
        return a;
    }
    ExprP parse_and() {
        ExprP a = parse_not();
        while (a && is_name("and")) {
            if (!advance())
                return nullptr;
            ExprP b = parse_not();
            if (!b)
                return nullptr;
            ExprP e = node(JExpr::And);
            e->args = {a, b};
            a = e;
        }
        return a;
    }
    ExprP parse_not() {
        if (is_name("not")) {
            if (!advance())
                return nullptr;
            ExprP a = parse_not();
            if (!a)
                return nullptr;
            ExprP e = node(JExpr::Not);
            e->args = {a};
            return e;
        }
        return parse_cmp();
    }
    ExprP parse_cmp() {
        ExprP a = parse_concat();
        if (!a)
            return nullptr;
        ExprP e;
        for (;;) {
            std::string op;
            if (tok.k == Tok::Op && (tok.s == "==" || tok.s == "!=" || tok.s == "<"
                                     || tok.s == ">" || tok.s == "<="
                                     || tok.s == ">="))
                op = tok.s;
            else if (is_name("in"))
                op = "in";
            else if (is_name("not")) {
                // "not in"
                size_t save = lex.pos;
                Tok t2 = tok;
                if (!advance())
                    return nullptr;
                if (!is_name("in")) {
                    lex.pos = save;
                    tok = t2;
                    break;
                }
                op = "not in";
            } else if (is_name("is")) {
                if (!advance())
                    return nullptr;
                ExprP t = node(JExpr::Test);
                if (is_name("not")) {
                    t->negate = true;
                    if (!advance())
                        return nullptr;
                }
                if (tok.k != Tok::Name)
                    return fail("expected test name");
                t->name = tok.s;
                if (!advance())
                    return nullptr;
                t->args = {e ? e : a};
                a = t;
                e = nullptr;
                continue;
            } else {
                break;
            }
            if (!advance())
                return nullptr;
            ExprP b = parse_concat();
            if (!b)
                return nullptr;
            if (!e) {
                e = node(JExpr::Cmp);
                e->args.push_back(a);
            }
            e->ops.push_back(op);
            e->args.push_back(b);
        }
        return e ? e : a;
    }
    ExprP parse_concat() {
        ExprP a = parse_add();
        while (a && is_op("~")) {
            if (!advance())
                return nullptr;
            ExprP b = parse_add();
            if (!b)
                return nullptr;
            ExprP e = node(JExpr::Bin);
            e->name = "~";
            e->args = {a, b};
            a = e;
        }
        return a;
    }
    ExprP parse_add() {
        ExprP a = parse_mul();
        while (a && (is_op("+") || is_op("-"))) {
            std::string op = tok.s;
            if (!advance())
                return nullptr;
            ExprP b = parse_mul();
            if (!b)
                return nullptr;
            ExprP e = node(JExpr::Bin);
            e->name = op;
            e->args = {a, b};
            a = e;
        }
        return a;
    }
    ExprP parse_mul() {
        ExprP a = parse_unary();
        while (a && (is_op("*") || is_op("/") || is_op("//") || is_op("%"))) {
            std::string op = tok.s;
            if (!advance())
                return nullptr;
            ExprP b = parse_unary();
            if (!b)
                return nullptr;
            ExprP e = node(JExpr::Bin);
            e->name = op;
            e->args = {a, b};
            a = e;
        }
        return a;
    }
    ExprP parse_unary() {
        if (is_op("-") || is_op("+")) {
            bool neg = is_op("-");
            if (!advance())
                return nullptr;
            ExprP a = parse_unary();
            if (!a)
                return nullptr;
            ExprP e = node(neg ? JExpr::Neg : JExpr::Pos);
            e->args = {a};
            return e;
        }
        return parse_pow();
    }
    ExprP parse_pow() {
        ExprP a = parse_postfix();
        if (a && is_op("**")) {
            if (!advance())
                return nullptr;
            ExprP b = parse_unary();
            if (!b)
                return nullptr;
            ExprP e = node(JExpr::Bin);
            e->name = "**";
            e->args = {a, b};
            return e;
        }
        return a;
    }
    bool parse_args(ExprP call) {
        // after '('
        while (!is_op(")")) {
            if (tok.k == Tok::Name) {
                size_t save = lex.pos;
                Tok t2 = tok;
                if (!advance())
                    return false;
                if (is_op("=")) {
                    if (!advance())
                        return false;
                    ExprP v = parse_expr();
                    if (!v)
                        return false;
                    call->kwargs.emplace_back(t2.s, v);
                    goto next_arg;
                }
                lex.pos = save;
                tok = t2;
            }
            {
                ExprP v = parse_expr();
                if (!v)
                    return false;
                call->args.push_back(v);
            }
        next_arg:
            if (is_op(",")) {
                if (!advance())
                    return false;
                continue;
            }
            if (!is_op(")")) {
                err = "expected ')'";
                return false;
            }
        }
        return expect(")");
    }
    ExprP parse_postfix() {
        ExprP a = parse_primary();
        while (a) {
            if (is_op(".")) {
                if (!advance())
                    return nullptr;
                if (tok.k != Tok::Name && tok.k != Tok::Int)
                    return fail("expected attribute name");
                ExprP e = node(JExpr::Attr);
                e->name = tok.k == Tok::Name ? tok.s : std::to_string(tok.i);
                e->args = {a};
                if (!advance())
                    return nullptr;
                a = e;
            } else if (is_op("[")) {
                depth++;
                if (!advance())
                    return nullptr;
                ExprP lo, hi;
                bool slice = false;
                if (!is_op(":")) {
                    lo = parse_expr();
                    if (!lo)
                        return nullptr;
                }
                if (is_op(":")) {
                    slice = true;
                    if (!advance())
                        return nullptr;
                    if (!is_op("]")) {
                        hi = parse_expr();
                        if (!hi)
                            return nullptr;
                    }
                }
                if (!expect("]"))
                    return nullptr;
                ExprP e = node(slice ? JExpr::Slice : JExpr::Item);
                e->args = {a, lo, hi};
                a = e;
            } else if (is_op("(")) {
                depth++;
                if (!advance())
                    return nullptr;
                ExprP e = node(JExpr::Call);
                e->args = {a};
                if (!parse_args(e))
                    return nullptr;
                a = e;
            } else if (is_op("|")) {
                if (!advance())
                    return nullptr;
                if (tok.k != Tok::Name)
                    return fail("expected filter name");
                ExprP e = node(JExpr::Filter);
                e->name = tok.s;
                e->args = {a};
                if (!advance())
                    return nullptr;
                if (is_op("(")) {
                    depth++;
                    if (!advance())
                        return nullptr;
                    if (!parse_args(e))
                        return nullptr;
                }
                a = e;
            } else {
                break;
            }
        }
        return a;
    }
    ExprP parse_primary() {
        ExprP e;
        switch (tok.k) {
        case Tok::Int:
            e = node(JExpr::Lit);
            e->lit = std::make_shared<JValue>(JValue::integer(tok.i));
            break;
        case Tok::Float:
            e = node(JExpr::Lit);
            e->lit = std::make_shared<JValue>(JValue::number(tok.f));
            break;
        case Tok::Str: {
            e = node(JExpr::Lit);
            std::string s = tok.s;
            if (!advance())
                return nullptr;
            while (tok.k == Tok::Str) { // adjacent string literals
                s += tok.s;
                if (!advance())
                    return nullptr;
            }
            e->lit = std::make_shared<JValue>(JValue::str(s));
            return e;
        }
        case Tok::Name:
            e = node(JExpr::Lit);
            if (tok.s == "True" || tok.s == "true")
                e->lit = std::make_shared<JValue>(JValue::boolean(true));
            else if (tok.s == "False" || tok.s == "false")
                e->lit = std::make_shared<JValue>(JValue::boolean(false));
            else if (tok.s == "None" || tok.s == "none")
                e->lit = std::make_shared<JValue>(JValue::none());
            else {
                e->k = JExpr::Name;
                e->name = tok.s;
            }
            break;
        case Tok::Op:
            if (tok.s == "(" || tok.s == "[") {
                bool paren = tok.s == "(";
                const char *close = paren ? ")" : "]";
                depth++;
                if (!advance())
                    return nullptr;
                std::vector<ExprP> items;
                bool comma = false;
                while (!is_op(close)) {
                    ExprP v = parse_expr();
                    if (!v)
                        return nullptr;
                    items.push_back(v);
                    if (is_op(",")) {
                        comma = true;
                        if (!advance())
                            return nullptr;
                    } else if (!is_op(close)) {
                        return fail(std::string("expected '") + close + "'");
                    }
                }
                if (!expect(close))
                    return nullptr;
                if (paren && items.size() == 1 && !comma)
                    return items[0];
                e = node(paren ? JExpr::TupleLit : JExpr::ListLit);
                e->args = items;
                return e;
            }
            if (tok.s == "{") {
                depth++;
                if (!advance())
                    return nullptr;
                e = node(JExpr::DictLit);
                while (!is_op("}")) {
                    ExprP k = parse_expr();
                    if (!k || !expect(":"))
                        return nullptr;
                    ExprP v = parse_expr();
                    if (!v)
                        return nullptr;
                    e->args.push_back(k);
                    e->args.push_back(v);
                    if (is_op(",")) {
                        if (!advance())
                            return nullptr;
                    } else if (!is_op("}")) {
                        return fail("expected '}'");
                    }
                }
                if (!expect("}"))
                    return nullptr;
                return e;
            }
            return fail("unexpected '" + tok.s + "'");
        case Tok::End:
            return fail("unexpected end of expression");
        }
        if (!advance())
            return nullptr;
        return e;
    }
};

} // namespace

/****************************************************************
 * Template nodes
 ****************************************************************/

struct JNode {
    enum K { Body, Text, Output, If, For, Set } k = Body;
    std::string text;
    ExprP expr;
    std::vector<std::shared_ptr<JNode>> children;  // Body
    // If: conds[i] guards bodies[i]; bodies.size() may be conds.size()+1
    std::vector<ExprP> conds;
    std::vector<std::shared_ptr<JNode>> bodies;
    std::vector<std::string> targets;               // For / Set
};
using NodeP = std::shared_ptr<JNode>;

namespace {

struct Scope {
    JDict vars;
    const Scope *parent = nullptr;
    const JDict *globals = nullptr;
    // for-loop scope: "loop" is built only when the template uses it
    bool has_loop = false;
    size_t loop_index = 0, loop_length = 0;
    mutable JValue loop_cache;
    mutable bool loop_built = false;
    const JValue *lookup(const std::string &n) const {
        for (const Scope *s = this; s; s = s->parent) {
            if (const JValue *v = s->vars.find(n))
                return v;
            if (s->has_loop && n == "loop") {
                if (!s->loop_built) {
                    JValue loop = JValue::make_dict();
                    size_t k = s->loop_index, len = s->loop_length;
                    loop.dict->set("index", JValue::integer((int64_t)k + 1));
                    loop.dict->set("index0", JValue::integer((int64_t)k));
                    loop.dict->set("first", JValue::boolean(k == 0));
                    loop.dict->set("last", JValue::boolean(k + 1 == len));
                    loop.dict->set("length", JValue::integer((int64_t)len));
                    loop.dict->set("revindex", JValue::integer((int64_t)(len - k)));
                    s->loop_cache = loop;
                    s->loop_built = true;
                }
                return &s->loop_cache;
            }
            if (s->globals)
                if (const JValue *v = s->globals->find(n))
                    return v;
        }
        return nullptr;
    }
};

bool eval(const ExprP &e, const Scope &sc, JValue *out, std::string *err);

bool
is_num(const JValue &v)
{
    return v.t == JValue::T::Int || v.t == JValue::T::Float
           || v.t == JValue::T::Bool;
}

double
num(const JValue &v)
{
    return v.t == JValue::T::Float ? v.f
        : v.t == JValue::T::Bool ? (double)v.b : (double)v.i;
}

int64_t
inum(const JValue &v)
{
    return v.t == JValue::T::Bool ? (int64_t)v.b : v.i;
}

bool
is_int(const JValue &v)
{
    return v.t == JValue::T::Int || v.t == JValue::T::Bool;
}

std::string
type_name(const JValue &v)
{
    switch (v.t) {
    case JValue::T::Undef: return "Undefined";
    case JValue::T::None: return "NoneType";
    case JValue::T::Bool: return "bool";
    case JValue::T::Int: return "int";
    case JValue::T::Float: return "float";
    case JValue::T::Str: return "str";
    case JValue::T::List: return "list";
    case JValue::T::Dict: return "dict";
    default: return "object";
    }
}

bool
undef_error(const JValue &v, std::string *err)
{
    *err = v.undef_name.empty() ? "value is undefined"
                                : "'" + v.undef_name + "' is undefined";
    return false;
}

bool
values_equal(const JValue &a, const JValue &b)
{
    if (is_num(a) && is_num(b))
        return num(a) == num(b);
    if (a.t != b.t)
        return false;
    switch (a.t) {
    case JValue::T::Undef: case JValue::T::None: return true;
    case JValue::T::Str: return a.s == b.s;
    case JValue::T::List:
        if (a.list->size() != b.list->size())
            return false;
        for (size_t k = 0; k < a.list->size(); k++)
            if (!values_equal((*a.list)[k], (*b.list)[k]))
                return false;
        return true;
    case JValue::T::Dict:
        if (a.dict->items.size() != b.dict->items.size())
            return false;
        for (const auto &kv : a.dict->items) {
            const JValue *o = b.dict->find(kv.first);
            if (!o || !values_equal(kv.second, *o))
                return false;
        }
        return true;
    default:
        return a.obj == b.obj && a.func == b.func;
    }
}

// Python "%" string formatting
bool
py_format(const std::string &fmt, const JValue &argv, JValue *out,
          std::string *err)
{
    std::vector<JValue> args;
    if (argv.t == JValue::T::List && argv.list && argv.s == "tuple")
        args = *argv.list;
    else
        args.push_back(argv);
    std::string res;
    size_t ai = 0;
    for (size_t k = 0; k < fmt.size(); k++) {
        if (fmt[k] != '%') {
            res += fmt[k];
            continue;
        }
        if (++k >= fmt.size()) {
            *err = "incomplete format";
            return false;
        }
        if (fmt[k] == '%') {
            res += '%';
            continue;
        }
        std::string spec = "%";
        while (k < fmt.size() && strchr("-+ #0", fmt[k]))
            spec += fmt[k++];
        while (k < fmt.size() && isdigit((unsigned char)fmt[k]))
            spec += fmt[k++];
        if (k < fmt.size() && fmt[k] == '.') {
            spec += fmt[k++];
            while (k < fmt.size() && isdigit((unsigned char)fmt[k]))
                spec += fmt[k++];
        }
        if (k >= fmt.size()) {
            *err = "incomplete format";
            return false;
        }
        char conv = fmt[k];
        if (ai >= args.size()) {
            *err = "not enough arguments for format string";
            return false;
        }
        const JValue &v = args[ai++];
        char buf[512];
        if (conv == 's' || conv == 'r') {
            std::string sv = conv == 's' ? v.to_str() : v.repr();
            snprintf(buf, sizeof(buf), (spec + "s").c_str(), sv.c_str());
        } else if (strchr("dixXoc", conv)) {
            if (!is_num(v)) {
                *err = "%" + std::string(1, conv)
                       + " format: a number is required, not " + type_name(v);
                return false;
            }
            long long iv = v.t == JValue::T::Float ? (long long)v.f : inum(v);
            if (conv == 'c')
                snprintf(buf, sizeof(buf), (spec + "c").c_str(), (int)iv);
            else
                snprintf(buf, sizeof(buf), (spec + "ll" + std::string(1, conv
                         == 'i' ? 'd' : conv)).c_str(), iv);
        } else if (strchr("eEfFgG", conv)) {
            if (!is_num(v)) {
                *err = "must be real number, not " + type_name(v);
                return false;
            }
            snprintf(buf, sizeof(buf), (spec + conv).c_str(), num(v));
        } else {
            *err = std::string("unsupported format character '") + conv + "'";
            return false;
        }
        res += buf;
    }
    if (ai < args.size()) {
        *err = "not all arguments converted during string formatting";
        return false;
    }
    *out = JValue::str(res);
    return true;
}

double
py_mod(double a, double b)
{
    double r = std::fmod(a, b);
    if (r != 0. && ((r < 0.) != (b < 0.)))
        r += b;
    return r;
}

bool
binop(const std::string &op, const JValue &a, const JValue &b, JValue *out,
      std::string *err)
{
    if (a.is_undef())
        return undef_error(a, err);
    if (b.is_undef())
        return undef_error(b, err);
    if (op == "~") {
        *out = JValue::str(a.to_str() + b.to_str());
        return true;
    }
    if (op == "%" && a.t == JValue::T::Str)
        return py_format(a.s, b, out, err);
    if (op == "+") {
        if (a.t == JValue::T::Str && b.t == JValue::T::Str) {
            *out = JValue::str(a.s + b.s);
            return true;
        }
        if (a.t == JValue::T::List && b.t == JValue::T::List) {
            JList l = *a.list;
            l.insert(l.end(), b.list->begin(), b.list->end());
            *out = JValue::make_list(l);
            return true;
        }
    }
    if (op == "*") {
        const JValue *seq = a.t == JValue::T::Str || a.t == JValue::T::List ? &a
            : b.t == JValue::T::Str || b.t == JValue::T::List ? &b : nullptr;
        const JValue *cnt = seq == &a ? &b : &a;
        if (seq && is_int(*cnt)) {
            int64_t n = std::max<int64_t>(0, inum(*cnt));
            if (seq->t == JValue::T::Str) {
                std::string s;
                for (int64_t k = 0; k < n; k++)
                    s += seq->s;
                *out = JValue::str(s);
            } else {
                JList l;
                for (int64_t k = 0; k < n; k++)
                    l.insert(l.end(), seq->list->begin(), seq->list->end());
                *out = JValue::make_list(l);
            }
            return true;
        }
    }
    if (!is_num(a) || !is_num(b)) {
        *err = "unsupported operand type(s) for " + op + ": '" + type_name(a)
               + "' and '" + type_name(b) + "'";
        return false;
    }
    bool ints = is_int(a) && is_int(b);
    if (op == "/") {
        if (num(b) == 0.) {
            *err = "division by zero";
            return false;
        }
        *out = JValue::number(num(a) / num(b));
        return true;
    }
    if (op == "//" || op == "%") {
        if (num(b) == 0.) {
            *err = "integer division or modulo by zero";
            return false;
        }
        if (ints) {
            int64_t x = inum(a), y = inum(b);
            int64_t q = x / y, r = x % y;
            if (r != 0 && ((r < 0) != (y < 0))) {
                q--;
                r += y;
            }
            *out = JValue::integer(op == "//" ? q : r);
        } else {
            double x = num(a), y = num(b);
            *out = JValue::number(op == "//" ? std::floor(x / y)
                                             : py_mod(x, y));
        }
        return true;
    }
    if (op == "**") {
        if (ints && inum(b) >= 0) {
            int64_t r = 1, base = inum(a);
            for (int64_t k = 0; k < inum(b); k++)
                r *= base;
            *out = JValue::integer(r);
        } else {
            *out = JValue::number(std::pow(num(a), num(b)));
        }
        return true;
    }
    if (ints) {
        int64_t x = inum(a), y = inum(b);
        *out = JValue::integer(op == "+" ? x + y : op == "-" ? x - y : x * y);
    } else {
        double x = num(a), y = num(b);
        *out = JValue::number(op == "+" ? x + y : op == "-" ? x - y : x * y);
    }
    return true;
}

bool
contains(const JValue &container, const JValue &item, bool *res,
         std::string *err)
{
    switch (container.t) {
    case JValue::T::Str:
        if (item.t != JValue::T::Str) {
            *err = "'in <string>' requires string as left operand";
            return false;
        }
        *res = container.s.find(item.s) != std::string::npos;
        return true;
    case JValue::T::List:
        *res = false;
        for (const JValue &v : *container.list)
            if (values_equal(v, item))
                *res = true;
        return true;
    case JValue::T::Dict:
        *res = item.t == JValue::T::Str && container.dict->find(item.s);
        return true;
    case JValue::T::Object:
        *res = false;
        if (container.obj->keys && item.t == JValue::T::Str)
            for (const auto &k : container.obj->keys())
                if (k == item.s)
                    *res = true;
        return true;
    case JValue::T::Undef:
        return undef_error(container, err);
    default:
        *err = "argument of type '" + type_name(container)
               + "' is not iterable";
        return false;
    }
}

bool
compare(const std::string &op, const JValue &a, const JValue &b, bool *res,
        std::string *err)
{
    if (op == "==") { *res = values_equal(a, b); return true; }
    if (op == "!=") { *res = !values_equal(a, b); return true; }
    if (op == "in" || op == "not in") {
        if (!contains(b, a, res, err))
            return false;
        if (op == "not in")
            *res = !*res;
        return true;
    }
    int c;
    if (is_num(a) && is_num(b))
        c = num(a) < num(b) ? -1 : num(a) > num(b) ? 1 : 0;
    else if (a.t == JValue::T::Str && b.t == JValue::T::Str)
        c = a.s.compare(b.s);
    else {
        if (a.is_undef())
            return undef_error(a, err);
        if (b.is_undef())
            return undef_error(b, err);
        *err = "'" + op + "' not supported between instances of '"
               + type_name(a) + "' and '" + type_name(b) + "'";
        return false;
    }
    *res = op == "<" ? c < 0 : op == ">" ? c > 0 : op == "<=" ? c <= 0 : c >= 0;
    return true;
}

bool
to_list(const JValue &v, JList *out, std::string *err)
{
    out->clear();
    switch (v.t) {
    case JValue::T::List: *out = *v.list; return true;
    case JValue::T::Dict:
        for (const auto &kv : v.dict->items)
            out->push_back(JValue::str(kv.first));
        return true;
    case JValue::T::Str:
        for (char c : v.s)
            out->push_back(JValue::str(std::string(1, c)));
        return true;
    case JValue::T::Object:
        if (v.obj->keys)
            for (const auto &k : v.obj->keys())
                out->push_back(JValue::str(k));
        return true;
    case JValue::T::Undef: return true; // iterating Undefined = empty
    default:
        *err = "'" + type_name(v) + "' object is not iterable";
        return false;
    }
}

bool
parse_float(const std::string &s, double *out)
{
    std::string t = s;
    while (!t.empty() && isspace((unsigned char)t.back()))
        t.pop_back();
    size_t b = t.find_first_not_of(" \t");
    if (b == std::string::npos)
        return false;
    t = t.substr(b);
    char *end;
    *out = strtod(t.c_str(), &end);
    return *end == '\0';
}

bool
apply_filter(const std::string &name, const JValue &v,
             const std::vector<JValue> &args, JValue *out, std::string *err)
{
    auto arg = [&](size_t k, JValue def) { return k < args.size() ? args[k] : def; };
    if (name == "default" || name == "d") {
        bool boolean = arg(1, JValue::boolean(false)).truthy();
        *out = (v.is_undef() || (boolean && !v.truthy()))
            ? arg(0, JValue::str("")) : v;
        return true;
    }
    if (name == "float") {
        JValue def = arg(0, JValue::number(0.));
        double d;
        if (is_num(v))
            *out = JValue::number(num(v));
        else if (v.t == JValue::T::Str && parse_float(v.s, &d))
            *out = JValue::number(d);
        else
            *out = is_num(def) ? JValue::number(num(def)) : def;
        return true;
    }
    if (name == "int") {
        JValue def = arg(0, JValue::integer(0));
        double d;
        if (v.t == JValue::T::Float)
            *out = JValue::integer((int64_t)v.f);
        else if (is_int(v))
            *out = JValue::integer(inum(v));
        else if (v.t == JValue::T::Str && parse_float(v.s, &d))
            *out = JValue::integer((int64_t)d);
        else
            *out = def;
        return true;
    }
    if (v.is_undef() && name != "string" && name != "length")
        return undef_error(v, err);
    if (name == "string") { *out = JValue::str(v.to_str()); return true; }
    if (name == "lower" || name == "upper") {
        std::string s = v.to_str();
        for (auto &c : s)
            c = (char)(name == "lower" ? tolower((unsigned char)c)
                                       : toupper((unsigned char)c));
        *out = JValue::str(s);
        return true;
    }
    if (name == "trim") {
        std::string s = v.to_str();
        size_t b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
        *out = JValue::str(b == std::string::npos ? "" : s.substr(b, e - b + 1));
        return true;
    }
    if (name == "abs") {
        if (!is_num(v)) {
            *err = "bad operand type for abs()";
            return false;
        }
        *out = is_int(v) ? JValue::integer(std::llabs(inum(v)))
                         : JValue::number(std::fabs(v.f));
        return true;
    }
    if (name == "round") {
        int prec = (int)inum(arg(0, JValue::integer(0)));
        std::string method = arg(1, JValue::str("common")).to_str();
        double p = std::pow(10., prec), x = num(v) * p;
        double r = method == "ceil" ? std::ceil(x) : method == "floor"
            ? std::floor(x) : std::round(x);
        *out = JValue::number(r / p);
        return true;
    }
    if (name == "length" || name == "count") {
        if (v.t == JValue::T::Str)
            *out = JValue::integer((int64_t)v.s.size());
        else {
            JList l;
            if (!to_list(v, &l, err))
                return false;
            *out = JValue::integer((int64_t)l.size());
        }
        return true;
    }
    if (name == "list") {
        JList l;
        if (!to_list(v, &l, err))
            return false;
        *out = JValue::make_list(l);
        return true;
    }
    if (name == "join") {
        JList l;
        if (!to_list(v, &l, err))
            return false;
        std::string sep = arg(0, JValue::str("")).to_str(), s;
        for (size_t k = 0; k < l.size(); k++)
            s += (k ? sep : "") + l[k].to_str();
        *out = JValue::str(s);
        return true;
    }
    if (name == "first" || name == "last" || name == "min" || name == "max"
        || name == "sum") {
        JList l;
        if (!to_list(v, &l, err))
            return false;
        if (name == "sum") {
            JValue acc = JValue::integer(0);
            for (const auto &x : l)
                if (!binop("+", acc, x, &acc, err))
                    return false;
            *out = acc;
            return true;
        }
        if (l.empty()) {
            *out = JValue::undefined();
            return true;
        }
        if (name == "first") { *out = l.front(); return true; }
        if (name == "last") { *out = l.back(); return true; }
        JValue best = l[0];
        for (const auto &x : l) {
            bool lt;
            if (!compare("<", x, best, &lt, err))
                return false;
            if ((name == "min") == lt && !values_equal(x, best))
                best = x;
        }
        *out = best;
        return true;
    }
    if (name == "replace") {
        std::string s = v.to_str(), from = arg(0, JValue::str("")).to_str(),
                    to = arg(1, JValue::str("")).to_str();
        if (!from.empty())
            for (size_t pos = 0; (pos = s.find(from, pos)) != std::string::npos;
                 pos += to.size())
                s.replace(pos, from.size(), to);
        *out = JValue::str(s);
        return true;
    }
    *err = "No filter named '" + name + "'";
    return false;
}

// Methods on str / dict values ("x.lower()", "params.get('S', 0)")
bool
get_method(const JValue &self, const std::string &name, JValue *out)
{
    JValue me = self;
    if (self.t == JValue::T::Str) {
        static const char *str_methods[] = {"lower", "upper", "strip",
            "split", "startswith", "endswith", "replace", "find", "isdigit"};
        if (std::find_if(std::begin(str_methods), std::end(str_methods),
                         [&](const char *m) { return name == m; })
            == std::end(str_methods))
            return false;
        *out = JValue::function([me, name](const std::vector<JValue> &a,
                                           const std::vector<std::pair<std::string, JValue>> &,
                                           JValue *ret, std::string *err) {
            const std::string &s = me.s;
            auto sarg = [&](size_t k) { return k < a.size() ? a[k].to_str() : ""; };
            if (name == "lower" || name == "upper")
                return apply_filter(name, me, {}, ret, err);
            if (name == "strip")
                return apply_filter("trim", me, {}, ret, err);
            if (name == "replace")
                return apply_filter("replace", me, a, ret, err);
            if (name == "startswith") {
                *ret = JValue::boolean(s.compare(0, sarg(0).size(), sarg(0)) == 0);
                return true;
            }
            if (name == "endswith") {
                std::string x = sarg(0);
                *ret = JValue::boolean(s.size() >= x.size()
                    && s.compare(s.size() - x.size(), x.size(), x) == 0);
                return true;
            }
            if (name == "find") {
                size_t p = s.find(sarg(0));
                *ret = JValue::integer(p == std::string::npos ? -1 : (int64_t)p);
                return true;
            }
            if (name == "isdigit") {
                *ret = JValue::boolean(!s.empty() && std::all_of(s.begin(),
                    s.end(), [](char c) { return isdigit((unsigned char)c); }));
                return true;
            }
            // split
            JList parts;
            std::string sep = sarg(0);
            if (sep.empty()) {
                size_t k = 0;
                while (k < s.size()) {
                    while (k < s.size() && isspace((unsigned char)s[k]))
                        k++;
                    size_t st = k;
                    while (k < s.size() && !isspace((unsigned char)s[k]))
                        k++;
                    if (k > st)
                        parts.push_back(JValue::str(s.substr(st, k - st)));
                }
            } else {
                size_t st = 0, p;
                while ((p = s.find(sep, st)) != std::string::npos) {
                    parts.push_back(JValue::str(s.substr(st, p - st)));
                    st = p + sep.size();
                }
                parts.push_back(JValue::str(s.substr(st)));
            }
            *ret = JValue::make_list(parts);
            return true;
        });
        return true;
    }
    if (self.t == JValue::T::Dict && (name == "keys" || name == "values"
                                      || name == "items" || name == "get")) {
        *out = JValue::function([me, name](const std::vector<JValue> &a,
                                           const std::vector<std::pair<std::string, JValue>> &,
                                           JValue *ret, std::string *) {
            if (name == "get") {
                const JValue *v = a.empty() ? nullptr : me.dict->find(a[0].to_str());
                *ret = v ? *v : (a.size() > 1 ? a[1] : JValue::none());
                return true;
            }
            JList l;
            for (const auto &kv : me.dict->items) {
                if (name == "keys")
                    l.push_back(JValue::str(kv.first));
                else if (name == "values")
                    l.push_back(kv.second);
                else {
                    JValue t = JValue::make_list({JValue::str(kv.first), kv.second});
                    t.s = "tuple";
                    l.push_back(t);
                }
            }
            *ret = JValue::make_list(l);
            return true;
        });
        return true;
    }
    return false;
}

bool
get_attr(const JValue &v, const std::string &name, JValue *out,
         std::string *err)
{
    switch (v.t) {
    case JValue::T::Undef:
        return undef_error(v, err);
    case JValue::T::Dict:
        if (const JValue *e = v.dict->find(name)) {
            *out = *e;
            return true;
        }
        if (get_method(v, name, out))
            return true;
        *out = JValue::undefined(name);
        return true;
    case JValue::T::Object:
        *out = v.obj->get(name);
        if (out->is_undef())
            out->undef_name = name;
        return true;
    default:
        if (get_method(v, name, out))
            return true;
        *out = JValue::undefined(name);
        return true;
    }
}

bool
get_item(const JValue &v, const JValue &key, JValue *out, std::string *err)
{
    switch (v.t) {
    case JValue::T::Undef:
        return undef_error(v, err);
    case JValue::T::List:
    case JValue::T::Str: {
        if (!is_int(key)) {
            *err = "indices must be integers";
            return false;
        }
        int64_t n = v.t == JValue::T::List ? (int64_t)v.list->size()
                                           : (int64_t)v.s.size();
        int64_t k = inum(key);
        if (k < 0)
            k += n;
        if (k < 0 || k >= n) {
            *out = JValue::undefined();
            return true;
        }
        *out = v.t == JValue::T::List ? (*v.list)[k]
                                      : JValue::str(std::string(1, v.s[k]));
        return true;
    }
    default:
        return get_attr(v, key.to_str(), out, err);
    }
}

bool
call_value(const JValue &f, const std::vector<JValue> &args,
           const std::vector<std::pair<std::string, JValue>> &kwargs,
           JValue *out, std::string *err)
{
    if (f.t != JValue::T::Func) {
        if (f.is_undef())
            return undef_error(f, err);
        *err = "'" + type_name(f) + "' object is not callable";
        return false;
    }
    return (*f.func)(args, kwargs, out, err);
}

bool
eval(const ExprP &e, const Scope &sc, JValue *out, std::string *err)
{
    JValue a, b;
    switch (e->k) {
    case JExpr::Lit:
        *out = *e->lit;
        return true;
    case JExpr::Name:
        if (const JValue *v = sc.lookup(e->name)) {
            *out = *v;
            return true;
        }
        if (e->name == "range") {
            *out = JValue::function([](const std::vector<JValue> &args,
                                       const std::vector<std::pair<std::string, JValue>> &,
                                       JValue *ret, std::string *err) {
                int64_t st = 0, en = 0, step = 1;
                for (const auto &x : args)
                    if (!is_int(x)) {
                        *err = "range() integer arguments expected";
                        return false;
                    }
                if (args.size() == 1)
                    en = inum(args[0]);
                else if (args.size() >= 2) {
                    st = inum(args[0]);
                    en = inum(args[1]);
                    if (args.size() > 2)
                        step = inum(args[2]);
                }
                if (step == 0) {
                    *err = "range() arg 3 must not be zero";
                    return false;
                }
                JList l;
                for (int64_t k = st; step > 0 ? k < en : k > en; k += step) {
                    l.push_back(JValue::integer(k));
                    if (l.size() > 100000) {
                        *err = "range too large";
                        return false;
                    }
                }
                *ret = JValue::make_list(l);
                return true;
            });
            return true;
        }
        *out = JValue::undefined(e->name);
        return true;
    case JExpr::Attr:
        return eval(e->args[0], sc, &a, err) && get_attr(a, e->name, out, err);
    case JExpr::Item:
        return eval(e->args[0], sc, &a, err) && eval(e->args[1], sc, &b, err)
               && get_item(a, b, out, err);
    case JExpr::Slice: {
        if (!eval(e->args[0], sc, &a, err))
            return false;
        int64_t n = a.t == JValue::T::List ? (int64_t)a.list->size()
            : a.t == JValue::T::Str ? (int64_t)a.s.size() : -1;
        if (n < 0) {
            *err = "object is not subscriptable";
            return false;
        }
        int64_t lo = 0, hi = n;
        if (e->args[1]) {
            if (!eval(e->args[1], sc, &b, err))
                return false;
            lo = inum(b);
        }
        if (e->args[2]) {
            if (!eval(e->args[2], sc, &b, err))
                return false;
            hi = inum(b);
        }
        if (lo < 0) lo += n;
        if (hi < 0) hi += n;
        lo = std::max<int64_t>(0, std::min(lo, n));
        hi = std::max<int64_t>(lo, std::min(hi, n));
        if (a.t == JValue::T::Str)
            *out = JValue::str(a.s.substr(lo, hi - lo));
        else
            *out = JValue::make_list(JList(a.list->begin() + lo,
                                           a.list->begin() + hi));
        return true;
    }
    case JExpr::Call:
    case JExpr::Filter: {
        std::vector<JValue> args;
        std::vector<std::pair<std::string, JValue>> kwargs;
        if (!eval(e->args[0], sc, &a, err))
            return false;
        for (size_t k = 1; k < e->args.size(); k++) {
            if (!eval(e->args[k], sc, &b, err))
                return false;
            args.push_back(b);
        }
        for (const auto &kv : e->kwargs) {
            if (!eval(kv.second, sc, &b, err))
                return false;
            kwargs.emplace_back(kv.first, b);
        }
        if (e->k == JExpr::Call)
            return call_value(a, args, kwargs, out, err);
        for (const auto &kv : kwargs)
            if (kv.first == "boolean" && e->name == "default") {
                while (args.size() < 2)
                    args.push_back(args.empty() ? JValue::str("")
                                                : JValue::boolean(false));
                args[1] = kv.second;
            }
        return apply_filter(e->name, a, args, out, err);
    }
    case JExpr::Neg:
    case JExpr::Pos:
        if (!eval(e->args[0], sc, &a, err))
            return false;
        if (!is_num(a)) {
            if (a.is_undef())
                return undef_error(a, err);
            *err = "bad operand type for unary -: '" + type_name(a) + "'";
            return false;
        }
        if (e->k == JExpr::Pos)
            *out = is_int(a) ? JValue::integer(inum(a)) : a;
        else
            *out = is_int(a) ? JValue::integer(-inum(a)) : JValue::number(-a.f);
        return true;
    case JExpr::Not:
        if (!eval(e->args[0], sc, &a, err))
            return false;
        *out = JValue::boolean(!a.truthy());
        return true;
    case JExpr::And:
        if (!eval(e->args[0], sc, &a, err))
            return false;
        if (!a.truthy()) {
            *out = a;
            return true;
        }
        return eval(e->args[1], sc, out, err);
    case JExpr::Or:
        if (!eval(e->args[0], sc, &a, err))
            return false;
        if (a.truthy()) {
            *out = a;
            return true;
        }
        return eval(e->args[1], sc, out, err);
    case JExpr::Bin:
        return eval(e->args[0], sc, &a, err) && eval(e->args[1], sc, &b, err)
               && binop(e->name, a, b, out, err);
    case JExpr::Cmp: {
        if (!eval(e->args[0], sc, &a, err))
            return false;
        for (size_t k = 0; k < e->ops.size(); k++) {
            if (!eval(e->args[k + 1], sc, &b, err))
                return false;
            bool r;
            if (!compare(e->ops[k], a, b, &r, err))
                return false;
            if (!r) {
                *out = JValue::boolean(false);
                return true;
            }
            a = b;
        }
        *out = JValue::boolean(true);
        return true;
    }
    case JExpr::Test: {
        if (!eval(e->args[0], sc, &a, err))
            return false;
        bool r;
        const std::string &n = e->name;
        if (n == "defined") r = !a.is_undef();
        else if (n == "undefined") r = a.is_undef();
        else if (n == "none") r = a.t == JValue::T::None;
        else if (n == "number") r = a.t == JValue::T::Int || a.t == JValue::T::Float;
        else if (n == "string") r = a.t == JValue::T::Str;
        else if (n == "even" || n == "odd") {
            if (!is_int(a)) {
                *err = "even/odd test needs an integer";
                return false;
            }
            r = (inum(a) % 2 == 0) == (n == "even");
        } else {
            *err = "No test named '" + n + "'";
            return false;
        }
        *out = JValue::boolean(r != e->negate);
        return true;
    }
    case JExpr::Cond:
        if (!eval(e->args[0], sc, &a, err))
            return false;
        if (a.truthy())
            return eval(e->args[1], sc, out, err);
        if (!e->args[2]) {
            *out = JValue::undefined();
            return true;
        }
        return eval(e->args[2], sc, out, err);
    case JExpr::ListLit:
    case JExpr::TupleLit: {
        JList l;
        for (const auto &x : e->args) {
            if (!eval(x, sc, &a, err))
                return false;
            l.push_back(a);
        }
        *out = JValue::make_list(l);
        if (e->k == JExpr::TupleLit)
            out->s = "tuple"; // marks tuples for "%" formatting
        return true;
    }
    case JExpr::DictLit: {
        *out = JValue::make_dict();
        for (size_t k = 0; k + 1 < e->args.size(); k += 2) {
            if (!eval(e->args[k], sc, &a, err)
                || !eval(e->args[k + 1], sc, &b, err))
                return false;
            out->dict->set(a.to_str(), b);
        }
        return true;
    }
    }
    *err = "bad expression";
    return false;
}

/****************************************************************
 * Template parsing
 ****************************************************************/

struct TemplateParser {
    const std::string &src;
    size_t pos = 0;
    std::string err;

    // Read a "{% ... %}" statement starting at pos (after "{%")
    bool read_stmt(std::string *stmt, bool *strip_before, bool *strip_after) {
        *strip_before = pos < src.size() && src[pos] == '-';
        if (*strip_before)
            pos++;
        size_t st = pos;
        char quote = 0;
        while (pos + 1 < src.size()) {
            char c = src[pos];
            if (quote) {
                if (c == quote)
                    quote = 0;
            } else if (c == '\'' || c == '"') {
                quote = c;
            } else if (c == '%' && src[pos + 1] == '}') {
                size_t en = pos;
                *strip_after = en > st && src[en - 1] == '-';
                if (*strip_after)
                    en--;
                *stmt = src.substr(st, en - st);
                pos += 2;
                return true;
            }
            pos++;
        }
        err = "unterminated '{%'";
        return false;
    }

    static std::string first_word(const std::string &s, std::string *rest) {
        size_t b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) {
            *rest = "";
            return "";
        }
        size_t e = b;
        while (e < s.size() && (isalnum((unsigned char)s[e]) || s[e] == '_'))
            e++;
        *rest = s.substr(e);
        return s.substr(b, e - b);
    }

    ExprP parse_expr_str(const std::string &s) {
        ExprParser p(s, 0, false);
        if (!p.advance()) {
            err = p.err;
            return nullptr;
        }
        ExprP e = p.parse_expr();
        if (!e) {
            err = p.err.empty() ? "bad expression" : p.err;
            return nullptr;
        }
        if (p.tok.k != Tok::End) {
            err = "unexpected '" + p.tok.s + "' in expression";
            return nullptr;
        }
        return e;
    }

    // Parse nodes until one of the end keywords; *endword gets it
    bool parse_body(NodeP body, const std::vector<std::string> &ends,
                    std::string *endword, std::string *endrest) {
        bool strip_next = false;
        while (pos < src.size()) {
            size_t open = pos;
            for (;;) {
                open = src.find('{', open);
                if (open == std::string::npos)
                    break;
                break;
            }
            std::string text = src.substr(pos, (open == std::string::npos
                                                ? src.size() : open) - pos);
            if (strip_next) {
                size_t b = text.find_first_not_of(" \t\r\n");
                text = b == std::string::npos ? "" : text.substr(b);
                strip_next = false;
            }
            if (open == std::string::npos) {
                add_text(body, text);
                pos = src.size();
                break;
            }
            char nx = open + 1 < src.size() ? src[open + 1] : 0;
            pos = open + 1;
            if (nx == '#') {
                size_t close = src.find("#}", pos + 1);
                if (close == std::string::npos) {
                    err = "unterminated comment";
                    return false;
                }
                bool sb = src[pos + 1] == '-';
                if (sb)
                    text = rstrip(text);
                add_text(body, text);
                strip_next = close > 0 && src[close - 1] == '-';
                pos = close + 2;
                continue;
            }
            if (nx == '%') {
                pos++;
                std::string stmt;
                bool sb, sa;
                if (!read_stmt(&stmt, &sb, &sa))
                    return false;
                if (sb)
                    text = rstrip(text);
                add_text(body, text);
                strip_next = sa;
                std::string rest;
                std::string kw = first_word(stmt, &rest);
                if (std::find(ends.begin(), ends.end(), kw) != ends.end()) {
                    *endword = kw;
                    *endrest = rest;
                    pending_strip = sa;
                    return true;
                }
                if (!parse_stmt(body, kw, rest))
                    return false;
                if (pending_strip) {
                    strip_next = true;
                    pending_strip = false;
                }
                continue;
            }
            // "{ expr }"
            add_text(body, text);
            ExprParser p(src, pos, true);
            if (!p.advance()) {
                err = p.err;
                return false;
            }
            ExprP e = p.parse_expr();
            if (!e) {
                err = p.err.empty() ? "bad expression" : p.err;
                return false;
            }
            if (!p.is_op("}")) {
                err = "expected '}' after expression";
                return false;
            }
            pos = p.lex.pos;
            auto n = std::make_shared<JNode>();
            n->k = JNode::Output;
            n->expr = e;
            body->children.push_back(n);
        }
        if (!ends.empty()) {
            err = "missing {% " + ends.back() + " %}";
            return false;
        }
        return true;
    }
    bool pending_strip = false;

    static std::string rstrip(const std::string &t) {
        size_t e = t.find_last_not_of(" \t\r\n");
        return e == std::string::npos ? "" : t.substr(0, e + 1);
    }
    static void add_text(NodeP body, const std::string &t) {
        if (t.empty())
            return;
        auto n = std::make_shared<JNode>();
        n->k = JNode::Text;
        n->text = t;
        body->children.push_back(n);
    }

    bool parse_targets(const std::string &s, std::vector<std::string> *out) {
        std::string t;
        for (char c : s) {
            if (c == ',') {
                out->push_back(t);
                t.clear();
            } else if (!isspace((unsigned char)c) && c != '(' && c != ')') {
                t += c;
            }
        }
        if (!t.empty())
            out->push_back(t);
        return !out->empty();
    }

    bool parse_stmt(NodeP body, const std::string &kw, const std::string &rest) {
        auto n = std::make_shared<JNode>();
        if (kw == "if") {
            n->k = JNode::If;
            std::string cond = rest;
            for (;;) {
                ExprP c = parse_expr_str(cond);
                if (!c)
                    return false;
                n->conds.push_back(c);
                auto b = std::make_shared<JNode>();
                std::string ew, er;
                if (!parse_body(b, {"elif", "else", "endif"}, &ew, &er))
                    return false;
                n->bodies.push_back(b);
                if (ew == "elif") {
                    cond = er;
                    continue;
                }
                if (ew == "else") {
                    auto eb = std::make_shared<JNode>();
                    if (!parse_body(eb, {"endif"}, &ew, &er))
                        return false;
                    n->bodies.push_back(eb);
                }
                break;
            }
        } else if (kw == "for") {
            n->k = JNode::For;
            size_t in = std::string::npos;
            for (size_t k = 0; k + 4 <= rest.size(); k++)
                if (rest.compare(k, 4, " in ") == 0) {
                    in = k;
                    break;
                }
            if (in == std::string::npos
                || !parse_targets(rest.substr(0, in), &n->targets)) {
                err = "bad for statement";
                return false;
            }
            n->expr = parse_expr_str(rest.substr(in + 4));
            if (!n->expr)
                return false;
            auto b = std::make_shared<JNode>();
            std::string ew, er;
            if (!parse_body(b, {"else", "endfor"}, &ew, &er))
                return false;
            n->bodies.push_back(b);
            if (ew == "else") {
                auto eb = std::make_shared<JNode>();
                if (!parse_body(eb, {"endfor"}, &ew, &er))
                    return false;
                n->bodies.push_back(eb);
            }
        } else if (kw == "set") {
            n->k = JNode::Set;
            size_t eq = std::string::npos;
            char quote = 0;
            for (size_t k = 0; k < rest.size(); k++) {
                char c = rest[k];
                if (quote) {
                    if (c == quote)
                        quote = 0;
                } else if (c == '\'' || c == '"') {
                    quote = c;
                } else if (c == '=' && (k + 1 >= rest.size() || rest[k + 1] != '=')
                           && (k == 0 || !strchr("=!<>", rest[k - 1]))) {
                    eq = k;
                    break;
                }
            }
            if (eq == std::string::npos
                || !parse_targets(rest.substr(0, eq), &n->targets)) {
                err = "bad set statement (block set is not supported)";
                return false;
            }
            n->expr = parse_expr_str(rest.substr(eq + 1));
            if (!n->expr)
                return false;
        } else {
            err = "unknown tag '" + kw + "'";
            return false;
        }
        body->children.push_back(n);
        return true;
    }
};

bool assign(Scope &sc, const std::vector<std::string> &targets,
            const JValue &v, std::string *err)
{
    if (targets.size() == 1) {
        sc.vars.set(targets[0], v);
        return true;
    }
    JList l;
    if (!to_list(v, &l, err))
        return false;
    if (l.size() != targets.size()) {
        *err = "cannot unpack " + std::to_string(l.size()) + " values into "
               + std::to_string(targets.size());
        return false;
    }
    for (size_t k = 0; k < l.size(); k++)
        sc.vars.set(targets[k], l[k]);
    return true;
}

bool
render_node(const NodeP &n, Scope &sc, std::string *out, std::string *err)
{
    switch (n->k) {
    case JNode::Body:
        for (const auto &c : n->children)
            if (!render_node(c, sc, out, err))
                return false;
        return true;
    case JNode::Text:
        *out += n->text;
        return true;
    case JNode::Output: {
        JValue v;
        if (!eval(n->expr, sc, &v, err))
            return false;
        *out += v.to_str();
        return true;
    }
    case JNode::If:
        for (size_t k = 0; k < n->conds.size(); k++) {
            JValue v;
            if (!eval(n->conds[k], sc, &v, err))
                return false;
            if (v.truthy())
                return render_node(n->bodies[k], sc, out, err);
        }
        if (n->bodies.size() > n->conds.size())
            return render_node(n->bodies.back(), sc, out, err);
        return true;
    case JNode::For: {
        JValue it;
        if (!eval(n->expr, sc, &it, err))
            return false;
        JList items;
        if (!to_list(it, &items, err))
            return false;
        Scope inner; // reused for every iteration
        inner.parent = &sc;
        inner.has_loop = true;
        inner.loop_length = items.size();
        for (size_t k = 0; k < items.size(); k++) {
            inner.vars.items.clear();
            inner.loop_index = k;
            inner.loop_built = false;
            if (!assign(inner, n->targets, items[k], err))
                return false;
            if (!render_node(n->bodies[0], inner, out, err))
                return false;
        }
        if (items.empty() && n->bodies.size() > 1)
            return render_node(n->bodies[1], sc, out, err);
        return true;
    }
    case JNode::Set: {
        JValue v;
        if (!eval(n->expr, sc, &v, err))
            return false;
        return assign(sc, n->targets, v, err);
    }
    }
    return true;
}

} // namespace

/****************************************************************
 * JTemplate
 ****************************************************************/

JTemplate::JTemplate() = default;
JTemplate::~JTemplate() = default;

bool
JTemplate::parse(const std::string &source, std::string *err)
{
    TemplateParser tp{source};
    auto root = std::make_shared<JNode>();
    std::string ew, er;
    if (!tp.parse_body(root, {}, &ew, &er)) {
        if (err)
            *err = "Error loading template: " + tp.err;
        return false;
    }
    root_ = root;
    return true;
}

bool
JTemplate::render(const JDict &ctx, std::string *out, std::string *err) const
{
    out->clear();
    if (!root_)
        return true;
    Scope sc;
    sc.globals = &ctx;
    std::string e;
    if (!render_node(root_, sc, out, &e)) {
        if (err)
            *err = "Error evaluating template: " + e;
        return false;
    }
    return true;
}

bool
JTemplate::render(const std::vector<const JDict *> &layers, std::string *out,
                  std::string *err) const
{
    out->clear();
    if (!root_)
        return true;
    // Scope chain: last layer outermost
    std::vector<Scope> chain(layers.size() + 1);
    const Scope *parent = nullptr;
    for (size_t k = layers.size(); k-- > 0;) {
        chain[k + 1].globals = layers[k];
        chain[k + 1].parent = parent;
        parent = &chain[k + 1];
    }
    chain[0].parent = parent; // template locals ({% set %})
    std::string e;
    if (!render_node(root_, chain[0], out, &e)) {
        if (err)
            *err = "Error evaluating template: " + e;
        return false;
    }
    return true;
}

bool
JTemplate::eval_expression(const std::string &expr, JValue *out,
                           std::string *err)
{
    TemplateParser tp{expr};
    ExprP e = tp.parse_expr_str(expr);
    if (!e) {
        if (err)
            *err = tp.err;
        return false;
    }
    Scope sc;
    return eval(e, sc, out, err);
}

} // namespace kl
