// Unit test of the Jinja2 subset (expected values follow Python/Jinja2).
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <cstdio>
#include <string>

#include "klippy/jinja.hpp"

using namespace kl;

int
main()
{
    JDict ctx;
    JValue params = JValue::make_dict();
    params.dict->set("G", JValue::str("01"));
    params.dict->set("X", JValue::str("10.5"));
    params.dict->set("Y", JValue::str("-3"));
    params.dict->set("S", JValue::str("255"));
    ctx.set("params", params);
    struct Case { const char *tmpl, *want; } cases[] = {
        // Fraxinus0pen printer.cfg macros
        {"G1 {% for p in params %}{'%s%s' % (p, params[p])}{% endfor %}",
         "G1 G01X10.5Y-3S255"},
        {"{% set S = params.S|default(0.0)|float %}SET_PIN VALUE={S / 255.0}",
         "SET_PIN VALUE=1.0"},
        {"{% set home_all = 'X' not in params and 'Y' not in params %}"
         "{% if home_all or 'X' in params %}_HOME_X{% endif %}"
         "{% if home_all or 'Z' in params %}_HOME_Z{% endif %}", "_HOME_X"},
        {"{% for i in range(3) %}G1 X{i * 0.5} Y{i * 0.5}\n{% endfor %}",
         "G1 X0.0 Y0.0\nG1 X0.5 Y0.5\nG1 X1.0 Y1.0\n"},
        // Python number semantics
        {"{1/2} {7//2} {-7//2} {-7 % 3} {7 % -3} {2**10} {2**-1}",
         "0.5 3 -4 2 -2 1024 0.5"},
        {"{0.1 + 0.2} {1.0} {1e20} {1e-5} {123456789.0 * 10} {3 * 1.5}",
         "0.30000000000000004 1.0 1e+20 1e-05 1234567890.0 4.5"},
        {"{'%.3f' % 1.23456} {'%5d|' % 42} {'%-4s|' % 'ab'} {'%x' % 255}",
         "1.235    42| ab  | ff"},
        {"{[1, 'a', None, True]} {{'k': 2}} {(1, 2)}",
         "[1, 'a', None, True] {'k': 2} (1, 2)"},
        {"{params.Q|default('none')} {params.X|float + 1} {params.X|int}",
         "none 11.5 10"},
        {"{ 'abc'|upper } {'a,b'.split(',')} {'Hello'[1:3]} {[3,1,2]|max}",
         "ABC ['a', 'b'] el 3"},
        {"{% for k, v in {'a': 1, 'b': 2}.items() %}{k}={v};{% endfor %}",
         "a=1;b=2;"},
        {"{% for x in [] %}x{% else %}empty{% endfor %} {loop}",
         "empty "},
        {"{% if 1 < 2 < 3 %}chain{% endif %} {1 if 0 else 2} {not 0}",
         "chain 2 True"},
        {"{# comment #}a  {%- if true %} b {%- endif %}  c", "a b  c"},
        {"{% for i in range(3) %}{loop.index}{% if not loop.last %},{% endif %}{% endfor %}",
         "1,2,3"},
        {"{ undefined_var }|{ 'x' ~ 1 ~ 2.5 }", "|x12.5"},
    };
    int fails = 0;
    for (const auto &c : cases) {
        JTemplate t;
        std::string out, err;
        if (!t.parse(c.tmpl, &err) || !t.render(ctx, &out, &err)) {
            printf("FAIL %s\n  error: %s\n", c.tmpl, err.c_str());
            fails++;
            continue;
        }
        if (out != c.want) {
            printf("FAIL %s\n  got:  [%s]\n  want: [%s]\n", c.tmpl, out.c_str(),
                   c.want);
            fails++;
        }
    }
    // Errors that Jinja2 reports
    const char *errs[] = {"{ nothere.attr }", "{ 1 + 'a' }", "{% if %}",
                          "{ 1/0 }", "{% for x in y %}"};
    for (const char *e : errs) {
        JTemplate t;
        std::string out, err;
        if (t.parse(e, &err) && t.render(ctx, &out, &err)) {
            printf("FAIL (expected error) %s -> [%s]\n", e, out.c_str());
            fails++;
        }
    }
    printf("%s (%d failures)\n", fails ? "jinja FAIL" : "jinja PASS", fails);
    return fails ? 1 : 0;
}
