// G-code macros: port of klippy/extras/gcode_macro.py ([gcode_macro],
// SET_GCODE_VARIABLE) and extras/homing_override.py.
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <cctype>
#include <cstdio>

#include "klippy/printer.hpp"
#include "printer_internal.hpp"

namespace kl {

static std::string
upper(std::string s)
{
    for (auto &c : s)
        c = (char)toupper((unsigned char)c);
    return s;
}

JDict
Printer::template_context()
{
    // gcode_macro.py TemplateWrapper.create_template_context()
    JDict ctx;
    auto obj = std::make_shared<JObject>();
    obj->get = [this](const std::string &n) { return status_object(n); };
    obj->keys = [this]() {
        std::vector<std::string> k = {"toolhead", "gcode_move"};
        for (auto &m : macros_)
            k.push_back("gcode_macro " + m->name);
        return k;
    };
    JValue printer;
    printer.t = JValue::T::Object;
    printer.obj = obj;
    ctx.set("printer", printer);
    using Args = std::vector<JValue>;
    using Kw = std::vector<std::pair<std::string, JValue>>;
    ctx.set("action_respond_info", JValue::function(
        [this](const Args &a, const Kw &, JValue *ret, std::string *) {
            gcode_.respond_info(a.empty() ? "" : a[0].to_str());
            *ret = JValue::str("");
            return true;
        }));
    ctx.set("action_raise_error", JValue::function(
        [](const Args &a, const Kw &, JValue *, std::string *err) {
            *err = a.empty() ? "" : a[0].to_str();
            return false;
        }));
    ctx.set("action_emergency_stop", JValue::function(
        [this](const Args &a, const Kw &, JValue *ret, std::string *) {
            invoke_shutdown("Shutdown due to " + (a.empty()
                ? std::string("action_emergency_stop") : a[0].to_str()));
            *ret = JValue::str("");
            return true;
        }));
    ctx.set("action_call_remote_method", JValue::function(
        [](const Args &, const Kw &, JValue *ret, std::string *) {
            *ret = JValue::str(""); // no API server yet (M5)
            return true;
        }));
    return ctx;
}

bool
Printer::run_template(const JTemplate &t, const std::string &name,
                      const std::vector<const JDict *> &layers,
                      std::string *err)
{
    if (!base_context_)
        base_context_.reset(new JDict(template_context()));
    std::vector<const JDict *> all = layers;
    all.push_back(base_context_.get());
    std::string script, e;
    double t0 = monotonic();
    bool rok = t.render(all, &script, &e);
    stats_.render_s += monotonic() - t0;
    stats_.renders++;
    if (!rok) {
        *err = "Error evaluating '" + name + "': " + e;
        return false;
    }
    return gcode_.run_script(script, err);
}

static void
set_params(JDict &ctx, const GCodeCommand &g)
{
    JValue params = JValue::make_dict();
    for (const auto &kv : g.ordered_params())
        params.dict->set(kv.first, JValue::str(kv.second));
    ctx.set("params", params);
    ctx.set("rawparams", JValue::str(g.get_raw_command_parameters()));
}

bool
Printer::load_macros(std::set<std::string> *handled, std::string *err)
{
    for (const auto &sname : config_.sections()) {
        if (sname.compare(0, 12, "gcode_macro ") != 0)
            continue;
        ConfigSection sec = config_.getsection(sname);
        auto m = std::make_unique<Macro>();
        m->name = sname.substr(12);
        m->alias = upper(m->name);
        if (!m->tmpl.parse(sec.get("gcode"), err)) {
            *err = "[" + sname + "] " + *err;
            return false;
        }
        for (const auto &opt : sec.prefix_options("variable_")) {
            JValue v;
            std::string e;
            if (!JTemplate::eval_expression(sec.get(opt), &v, &e)) {
                *err = "Option '" + opt + "' in section '" + sname
                       + "' is not a valid literal: " + e;
                return false;
            }
            m->variables.set(opt.substr(9), v);
        }
        m->rename_existing = sec.get("rename_existing", "");
        sec.get("description", "");
        Macro *mp = m.get();
        if (!m->rename_existing.empty()) {
            // gcode_macro.py _handle_connect(): keep the old command
            // under the new name
            if (!gcode_.has_command(m->alias)) {
                warnings_.push_back("gcode_macro " + m->name
                    + ": existing command '" + m->alias
                    + "' not found for rename_existing (not supported yet?)");
            } else {
                // Re-register the previous handler under the new name
                gcode_.rename_command(m->alias, upper(m->rename_existing));
            }
        }
        gcode_.register_command(
            m->alias, [this, mp](GCodeCommand &g, std::string *e) {
                // gcode_macro.py GCodeMacro.cmd()
                if (mp->in_script) {
                    *e = "Macro " + mp->alias + " called recursively";
                    return false;
                }
                JDict ctx;
                set_params(ctx, g);
                mp->in_script = true;
                bool ok = run_template(mp->tmpl, "gcode_macro " + mp->name
                                       + ":gcode", {&ctx, &mp->variables}, e);
                mp->in_script = false;
                return ok;
            });
        macros_.push_back(std::move(m));
        handled->insert(sname);
    }
    if (!macros_.empty())
        gcode_.register_command(
            "SET_GCODE_VARIABLE", [this](GCodeCommand &g, std::string *e) {
                std::string name, var, value;
                if (!g.get("MACRO", &name, e) || !g.get("VARIABLE", &var, e)
                    || !g.get("VALUE", &value, e))
                    return false;
                Macro *mp = nullptr;
                for (auto &m : macros_)
                    if (m->name == name)
                        mp = m.get();
                if (!mp) {
                    *e = "The value '" + name + "' is not valid for MACRO";
                    return false;
                }
                if (!mp->variables.find(var)) {
                    *e = "Unknown gcode_macro variable '" + var + "'";
                    return false;
                }
                JValue v;
                std::string pe;
                if (!JTemplate::eval_expression(value, &v, &pe)) {
                    *e = "Unable to parse '" + value + "' as a literal: " + pe;
                    return false;
                }
                mp->variables.set(var, v);
                return true;
            });

    // [homing_override]
    if (config_.has_section("homing_override")) {
        ConfigSection sec = config_.getsection("homing_override");
        auto ho = std::make_unique<HomingOverride>();
        const char *axes = "xyz";
        for (int i = 0; i < 3; i++) {
            std::string opt = std::string("set_position_") + axes[i];
            ho->has_start[i] = sec.has(opt);
            ho->start_pos[i] = sec.getfloat(opt, 0.);
        }
        ho->axes = upper(sec.get("axes", "XYZ"));
        if (!ho->tmpl.parse(sec.get("gcode"), err)) {
            *err = "[homing_override] " + *err;
            return false;
        }
        ho->prev_G28 = gcode_.get_handler("G28");
        HomingOverride *hp = ho.get();
        gcode_.register_command("G28", [this, hp](GCodeCommand &g,
                                                  std::string *e) {
            // homing_override.py cmd_G28()
            if (hp->in_script)
                return hp->prev_G28(g, e); // the real G28
            bool no_axis = !g.has("X") && !g.has("Y") && !g.has("Z");
            bool override = no_axis;
            for (char a : hp->axes)
                if (g.has(std::string(1, a)))
                    override = true;
            if (!override)
                return hp->prev_G28(g, e);
            // Forced position (set_position_<axis>) marks those axes homed
            toolhead_->get_last_move_time();
            double pos[3];
            std::string homing_axes;
            for (int i = 0; i < 3; i++) {
                pos[i] = toolhead_->commanded_pos()[i];
                if (hp->has_start[i]) {
                    pos[i] = hp->start_pos[i];
                    homing_axes += (char)('x' + i);
                }
            }
            set_position(pos, homing_axes.c_str());
            JDict ctx;
            set_params(ctx, g);
            hp->in_script = true;
            bool ok = run_template(hp->tmpl, "homing_override:gcode", {&ctx}, e);
            hp->in_script = false;
            return ok;
        });
        homing_override_ = std::move(ho);
        handled->insert("homing_override");
    }
    return true;
}

} // namespace kl
