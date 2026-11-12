// Timers for the simulated MCU.
//
// Based on src/linux/timer.c, but without POSIX timers or signals (not
// available on macOS): the main loop waits in console_poll() until the
// next timer is due, then spins for the last ~1 ms for accuracy.
//
// Copyright (C) 2017-2021  Kevin O'Connor <kevin@koconnor.net>
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <time.h> // clock_gettime
#include "autoconf.h" // CONFIG_CLOCK_FREQ
#include "board/irq.h" // irq_disable
#include "board/misc.h" // timer_from_us
#include "command.h" // DECL_CONSTANT
#include "internal.h" // console_poll
#include "sched.h" // DECL_INIT

DECL_CONSTANT("CLOCK_FREQ", CONFIG_CLOCK_FREQ);

static struct {
    struct timespec start;
    uint32_t last_read_time;
    uint32_t next_wake;
    int must_wake;
} TimerInfo;

uint32_t
timer_from_us(uint32_t us)
{
    return us * (CONFIG_CLOCK_FREQ / 1000000);
}

uint8_t
timer_is_before(uint32_t time1, uint32_t time2)
{
    return (int32_t)(time1 - time2) < 0;
}

uint32_t
timer_read_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t ns = (uint64_t)(ts.tv_sec - TimerInfo.start.tv_sec) * 1000000000ULL
                  + ts.tv_nsec - TimerInfo.start.tv_nsec;
    uint32_t t = (uint32_t)(ns * (CONFIG_CLOCK_FREQ / 1000000) / 1000);
    TimerInfo.last_read_time = t;
    return t;
}

void
timer_kick(void)
{
    TimerInfo.must_wake = 1;
}

#define TIMER_IDLE_REPEAT_COUNT 100
#define TIMER_REPEAT_COUNT 20
#define TIMER_MIN_TRY_TICKS timer_from_us(2)

// Invoke timers (same logic as src/linux/timer.c timer_dispatch)
static void
timer_dispatch(void)
{
    uint32_t repeat_count = TIMER_REPEAT_COUNT, next;
    for (;;) {
        next = sched_timer_dispatch();

        repeat_count--;
        uint32_t lrt = TimerInfo.last_read_time;
        if (!timer_is_before(lrt, next) && repeat_count)
            continue;

        uint32_t now = timer_read_time();
        int32_t diff = next - now;
        if (diff > (int32_t)TIMER_MIN_TRY_TICKS)
            break;

        if (unlikely(!repeat_count)) {
            if (diff < (int32_t)(-timer_from_us(100000)))
                try_shutdown("Rescheduled timer in the past");
            if (sched_check_set_tasks_busy())
                return;
            repeat_count = TIMER_IDLE_REPEAT_COUNT;
        }

        while (unlikely(diff > 0))
            diff = next - timer_read_time();
    }
    TimerInfo.next_wake = next;
    TimerInfo.must_wake = 0;
}

void
timer_init(void)
{
    clock_gettime(CLOCK_MONOTONIC, &TimerInfo.start);
    TimerInfo.next_wake = timer_read_time();
    timer_kick();
}
DECL_INIT(timer_init);


/****************************************************************
 * Interrupt wrappers
 ****************************************************************/

void irq_disable(void) { }
void irq_enable(void) { }
irqstatus_t irq_save(void) { return 0; }
void irq_restore(irqstatus_t flag) { }

static int
timer_due(void)
{
    return TimerInfo.must_wake
        || !timer_is_before(timer_read_time(), TimerInfo.next_wake);
}

void
irq_wait(void)
{
    if (timer_due()) {
        irq_poll();
        return;
    }
    // Sleep until ~1ms before the next timer (or console input), then
    // return and let the main loop spin until the timer is due.
    int32_t diff = TimerInfo.next_wake - timer_read_time();
    int32_t us = diff / (CONFIG_CLOCK_FREQ / 1000000) - 1000;
    console_poll(us > 0 ? us : 0);
    irq_poll();
}

void
irq_poll(void)
{
    if (timer_due())
        timer_dispatch();
}
