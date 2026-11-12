// Force-included into Klipper's serialqueue.c and pollreactor.c on
// ESP-IDF (see CMakeLists.txt).
//
// serialqueue wakes its background thread by writing to a pipe that the
// thread poll()s together with the serial fd.  ESP-IDF has no pipe(), but
// its eventfd supports select()/poll() alongside UART fds.  These macros
// route the pipe calls to an eventfd (one fd used for both pipe ends) and
// leave all other fds (the UART) untouched.
//
// This file may be distributed under the terms of the GNU GPLv3 license.
#ifndef SQ_PORT_ESP_H
#define SQ_PORT_ESP_H

// Pull in every system header the two files use before renaming
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <termios.h>
#include <unistd.h>

int sq_port_pipe(int fds[2]);
ssize_t sq_port_read(int fd, void *buf, size_t n);
ssize_t sq_port_write(int fd, const void *buf, size_t n);
int sq_port_fcntl(int fd, int cmd, ...);
int sq_port_close(int fd);

#define pipe sq_port_pipe
#define read sq_port_read
#define write sq_port_write
#define fcntl sq_port_fcntl
#define close sq_port_close

#endif // sq_port_esp.h
