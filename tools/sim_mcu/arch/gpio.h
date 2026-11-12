#ifndef __SIM_GPIO_H
#define __SIM_GPIO_H
// Virtual GPIO for the simulated MCU.  Pins are named gpio0..gpio29 like
// the RP2040 so an SKR Pico printer.cfg can be used unchanged.
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <stdint.h> // uint8_t

struct gpio_out {
    uint8_t pin;
};
struct gpio_out gpio_out_setup(uint32_t pin, uint8_t val);
void gpio_out_reset(struct gpio_out g, uint8_t val);
void gpio_out_toggle_noirq(struct gpio_out g);
void gpio_out_toggle(struct gpio_out g);
void gpio_out_write(struct gpio_out g, uint8_t val);

struct gpio_in {
    uint8_t pin;
};
struct gpio_in gpio_in_setup(uint32_t pin, int8_t pull_up);
void gpio_in_reset(struct gpio_in g, int8_t pull_up);
uint8_t gpio_in_read(struct gpio_in g);

#endif // gpio.h
