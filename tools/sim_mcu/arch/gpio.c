// Virtual GPIO for the simulated MCU.
//
// Every output pin keeps its level and counts its edges, so a test can
// check the step pulses that the real Klipper stepper code produced.
// Input pins read their pull-up level unless a test overrides them with
// the "sim_set_input" command (used later for endstop / DIAG tests).
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <stdio.h> // fprintf
#include <stdlib.h> // getenv
#include "compiler.h" // container_of
#include "board/irq.h" // irq_disable
#include "board/misc.h" // timer_read_time
#include "command.h" // DECL_COMMAND
#include "gpio.h" // gpio_out_setup
#include "internal.h" // SIM_NUM_GPIO
#include "sched.h" // shutdown

DECL_ENUMERATION_RANGE("pin", "gpio0", 0, SIM_NUM_GPIO);

static struct sim_pin {
    uint8_t level, is_output, forced, forced_level;
    uint32_t edges;
} pins[SIM_NUM_GPIO];

static struct sim_pin *
get_pin(uint32_t pin)
{
    if (pin >= SIM_NUM_GPIO)
        shutdown("Not a valid pin");
    return &pins[pin];
}

// Step-triggered input changes (sim_trigger_on_steps): a stand-in for a
// carriage that reaches an endstop / stalls after moving N steps in one
// direction (direction given by up to two dir pin levels, for CoreXY)
#define NUM_WATCH 8
static struct step_watch {
    uint8_t active, pin, value, step_pin, dir_pin, dir_value, dir2_pin,
        dir2_value;
    uint32_t remaining;
} watches[NUM_WATCH];

static void
check_watches(uint8_t step_pin)
{
    for (int i = 0; i < NUM_WATCH; i++) {
        struct step_watch *w = &watches[i];
        if (!w->active || w->step_pin != step_pin
            || pins[w->dir_pin].level != w->dir_value
            || pins[w->dir2_pin].level != w->dir2_value)
            continue;
        if (--w->remaining == 0) {
            pins[w->pin].forced = 1;
            pins[w->pin].forced_level = w->value;
            w->active = 0;
        }
    }
}

// Pin trace (SIM_TRACE=file, tools/plot_view.py draws it): one 8-byte
// record per output pin change, appended (survives the "reset" command's
// re-exec): u32 clock (CONFIG_CLOCK_FREQ, wraps), u8 pin, u8 level,
// u16 kind (0 = change, 1 = pin set up as output)
static FILE *trace;
static uint32_t trace_flushed;

static void
trace_pin(uint8_t pin, uint8_t level, uint16_t kind)
{
    static int inited;
    if (!inited) {
        inited = 1;
        const char *path = getenv("SIM_TRACE");
        if (path && *path) {
            trace = fopen(path, "ab");
            if (trace)
                setvbuf(trace, NULL, _IOFBF, 64 * 1024);
        }
    }
    if (!trace)
        return;
    uint32_t now = timer_read_time();
    uint8_t rec[8] = {now, now >> 8, now >> 16, now >> 24, pin, level,
                      kind, kind >> 8};
    fwrite(rec, sizeof(rec), 1, trace);
    // Flush often enough for a live view (every 50 ms of MCU time)
    if (now - trace_flushed > CONFIG_CLOCK_FREQ / 20) {
        trace_flushed = now;
        fflush(trace);
    }
}

void
sim_trace_flush(void)
{
    if (trace)
        fflush(trace);
}

static void
set_level(struct sim_pin *p, uint8_t val)
{
    val = !!val;
    if (p->level != val) {
        p->edges++;
        p->level = val;
        check_watches(p - pins);
        if (p->is_output)
            trace_pin(p - pins, val, 0);
    }
    p->level = val;
}

struct gpio_out
gpio_out_setup(uint32_t pin, uint8_t val)
{
    struct sim_pin *p = get_pin(pin);
    p->is_output = 1;
    p->level = !!val;
    trace_pin(pin, p->level, 1);
    return (struct gpio_out){ .pin = pin };
}

void
gpio_out_reset(struct gpio_out g, uint8_t val)
{
    set_level(&pins[g.pin], val);
}

void
gpio_out_toggle_noirq(struct gpio_out g)
{
    struct sim_pin *p = &pins[g.pin];
    set_level(p, !p->level);
}

void
gpio_out_toggle(struct gpio_out g)
{
    gpio_out_toggle_noirq(g);
}

void
gpio_out_write(struct gpio_out g, uint8_t val)
{
    set_level(&pins[g.pin], val);
}

struct gpio_in
gpio_in_setup(uint32_t pin, int8_t pull_up)
{
    struct sim_pin *p = get_pin(pin);
    p->is_output = 0;
    p->level = pull_up > 0;
    return (struct gpio_in){ .pin = pin };
}

void
gpio_in_reset(struct gpio_in g, int8_t pull_up)
{
    pins[g.pin].level = pull_up > 0;
}

uint8_t
gpio_in_read(struct gpio_in g)
{
    struct sim_pin *p = &pins[g.pin];
    return p->forced ? p->forced_level : p->level;
}

// Test hook: report a pin's level and edge count
void
command_sim_get_pin(uint32_t *args)
{
    uint8_t pin = args[0];
    struct sim_pin *p = get_pin(pin);
    sendf("sim_pin pin=%c level=%c edges=%u", pin, p->level, p->edges);
}
DECL_COMMAND_FLAGS(command_sim_get_pin, HF_IN_SHUTDOWN, "sim_get_pin pin=%c");

// Test hook: force the value an input pin reads (forced=0 releases it)
void
command_sim_set_input(uint32_t *args)
{
    struct sim_pin *p = get_pin(args[0]);
    p->forced = args[1];
    p->forced_level = !!args[2];
}
DECL_COMMAND_FLAGS(command_sim_set_input, HF_IN_SHUTDOWN,
                   "sim_set_input pin=%c forced=%c value=%c");

// Test hook: change a (forced) input value after a delay, e.g. to make an
// endstop or a TMC DIAG pin trigger part way through a homing move
#define NUM_DELAYED 4
static struct delayed_change {
    struct timer timer;
    uint8_t pin, value, active;
} delayed[NUM_DELAYED];

static uint_fast8_t
delayed_event(struct timer *t)
{
    struct delayed_change *d = container_of(t, struct delayed_change, timer);
    struct sim_pin *p = &pins[d->pin];
    p->forced = 1;
    p->forced_level = d->value;
    d->active = 0;
    return SF_DONE;
}

// Up to NUM_DELAYED pending changes; delay_ms=0 with value 255 cancels all
void
command_sim_set_input_delayed(uint32_t *args)
{
    if (args[1] == 255) {
        for (int i = 0; i < NUM_DELAYED; i++)
            if (delayed[i].active) {
                sched_del_timer(&delayed[i].timer);
                delayed[i].active = 0;
            }
        return;
    }
    struct delayed_change *d = NULL;
    for (int i = 0; i < NUM_DELAYED && !d; i++)
        if (!delayed[i].active)
            d = &delayed[i];
    if (!d)
        shutdown("Too many sim_set_input_delayed");
    d->pin = args[0];
    get_pin(d->pin);
    d->value = !!args[1];
    d->timer.func = delayed_event;
    d->timer.waketime = timer_read_time() + timer_from_us(args[2] * 1000);
    d->active = 1;
    sched_add_timer(&d->timer);
}
DECL_COMMAND_FLAGS(command_sim_set_input_delayed, HF_IN_SHUTDOWN,
                   "sim_set_input_delayed pin=%c value=%c delay_ms=%u");

void
command_sim_trigger_on_steps(uint32_t *args)
{
    struct step_watch *w = NULL;
    for (int i = 0; i < NUM_WATCH && !w; i++)
        if (!watches[i].active)
            w = &watches[i];
    if (!w)
        shutdown("Too many sim_trigger_on_steps");
    w->pin = args[0];
    w->value = !!args[1];
    w->step_pin = args[2];
    w->dir_pin = args[3];
    w->dir_value = !!args[4];
    w->dir2_pin = args[5];
    w->dir2_value = !!args[6];
    w->remaining = args[7] ? args[7] : 1;
    get_pin(w->pin);
    w->active = 1;
}
DECL_COMMAND_FLAGS(command_sim_trigger_on_steps, HF_IN_SHUTDOWN,
                   "sim_trigger_on_steps pin=%c value=%c step_pin=%c"
                   " dir_pin=%c dir_value=%c dir2_pin=%c dir2_value=%c"
                   " steps=%u");

void
sim_gpio_report(void)
{
    for (int i = 0; i < SIM_NUM_GPIO; i++)
        if (pins[i].edges)
            fprintf(stderr, "  gpio%d: level %d, %u edges\n", i,
                    pins[i].level, pins[i].edges);
}
