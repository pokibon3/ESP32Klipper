// Simulated RP2040 MCU: the unmodified Klipper MCU code (src/*.c) running
// as a host process, talking the Klipper protocol over a pseudo-tty.
//
// Used to develop and test the ESP32 host without an SKR Pico.  Pins are
// virtual (see gpio.c); "reset" restarts the process like a real MCU.
//
// Copyright (C) 2017  Kevin O'Connor <kevin@koconnor.net>
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <signal.h> // signal
#include <stdio.h> // fprintf
#include <stdlib.h> // exit
#include <unistd.h> // getopt
#include "board/misc.h" // console_sendf
#include "command.h" // DECL_CONSTANT
#include "internal.h" // console_setup
#include "sched.h" // sched_main

DECL_CONSTANT_STR("MCU", CONFIG_MCU);

static char **orig_argv;

// Same command name as the RP2040 port (src/generic/armcm_reset.c)
void
command_reset(uint32_t *args)
{
    fprintf(stderr, "sim_mcu: reset\n");
    sim_gpio_report();
    sim_trace_flush();
    execv(orig_argv[0], orig_argv);
    report_errno("execv", -1);
}
DECL_COMMAND_FLAGS(command_reset, HF_IN_SHUTDOWN, "reset");

static void
on_sigint(int sig)
{
    fprintf(stderr, "sim_mcu: exit\n");
    sim_gpio_report();
    sim_trace_flush();
    exit(0);
}

int
main(int argc, char **argv)
{
    orig_argv = argv;
    int opt, fd = -1;
    char *serial = "/tmp/klipper_sim_mcu";
    while ((opt = getopt(argc, argv, "I:F:")) != -1) {
        switch (opt) {
        case 'I':
            serial = optarg;
            break;
        case 'F':
            fd = atoi(optarg);
            break;
        default:
            fprintf(stderr, "Usage: %s [-I pty_path | -F fd]\n", argv[0]);
            return -1;
        }
    }
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);
    if (fd >= 0 ? console_setup_fd(fd) : console_setup(serial))
        return -1;
    sched_main();
    return 0;
}
