#ifndef __SIM_INTERNAL_H
#define __SIM_INTERNAL_H
// Simulated RP2040 MCU running as a macOS/Linux process (development aid).
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <stdint.h> // uint32_t
#include "autoconf.h" // CONFIG_CLOCK_FREQ

#define SIM_NUM_GPIO 30

// console.c
void report_errno(char *where, int rc);
int set_non_blocking(int fd);
int console_setup(char *name);
int console_setup_fd(int fd);
// Wait up to 'timeout_us' for console input (wakes the console task)
void console_poll(int timeout_us);

// gpio.c
void sim_gpio_report(void);
void sim_trace_flush(void);

#endif // internal.h
