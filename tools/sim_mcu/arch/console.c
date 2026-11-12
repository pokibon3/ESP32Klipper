// Pseudo-tty serial port for the simulated MCU.
//
// Based on src/linux/console.c.  The host connects to the pty through
// the symlink given with -I (default /tmp/klipper_sim_mcu).
//
// Copyright (C) 2017-2021  Kevin O'Connor <kevin@koconnor.net>
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <errno.h> // errno
#include <fcntl.h> // fcntl
#include <poll.h> // poll
#include <stdio.h> // fprintf
#include <string.h> // memmove
#include <sys/stat.h> // chmod
#include <termios.h> // struct termios
#include <unistd.h> // ttyname
#ifdef __APPLE__
#include <util.h> // openpty
#else
#include <pty.h> // openpty
#endif
#include "board/irq.h" // irq_wait
#include "board/misc.h" // console_sendf
#include "command.h" // command_find_and_dispatch
#include "internal.h" // console_setup
#include "sched.h" // sched_wake_task

static int tty_fd = -1, slave_fd = -1;

void
report_errno(char *where, int rc)
{
    int e = errno;
    fprintf(stderr, "Got error %d in %s: (%d)%s\n", rc, where, e, strerror(e));
}

int
set_non_blocking(int fd)
{
    int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        report_errno("fcntl", flags);
        return -1;
    }
    return 0;
}

int
console_setup(char *name)
{
    struct termios ti;
    memset(&ti, 0, sizeof(ti));
    int mfd, sfd, ret = openpty(&mfd, &sfd, NULL, &ti, NULL);
    if (ret) {
        report_errno("openpty", ret);
        return -1;
    }
    if (set_non_blocking(mfd))
        return -1;
    fcntl(mfd, F_SETFD, FD_CLOEXEC);
    fcntl(sfd, F_SETFD, FD_CLOEXEC);
    tty_fd = mfd;
    // Keep the slave open so the master does not see a hangup while the
    // host is disconnected (macOS reports POLLHUP otherwise).
    slave_fd = sfd;

    unlink(name);
    char *tname = ttyname(sfd);
    if (!tname || symlink(tname, name)) {
        report_errno("symlink", -1);
        return -1;
    }
    chmod(tname, 0660);
    fprintf(stderr, "sim_mcu: serial port %s -> %s\n", name, tname);
    return 0;
}

// Use an already connected fd (socketpair from the host test harness)
int
console_setup_fd(int fd)
{
    tty_fd = fd;
    return set_non_blocking(fd);
}

static struct task_wake console_wake;
static uint8_t receive_buf[4096];
static int receive_pos;

void *
console_receive_buffer(void)
{
    return receive_buf;
}

void
console_task(void)
{
    if (!sched_check_wake(&console_wake))
        return;

    int ret = read(tty_fd, &receive_buf[receive_pos]
                   , sizeof(receive_buf) - receive_pos);
    if (ret < 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN || errno == EIO) {
            ret = 0;
        } else {
            report_errno("read", ret);
            return;
        }
    }

    int len = receive_pos + ret;
    uint_fast8_t pop_count, msglen = len > MESSAGE_MAX ? MESSAGE_MAX : len;
    ret = command_find_and_dispatch(receive_buf, msglen, &pop_count);
    if (ret) {
        len -= pop_count;
        if (len) {
            memmove(receive_buf, &receive_buf[pop_count], len);
            sched_wake_task(&console_wake);
        }
    }
    receive_pos = len;
}
DECL_TASK(console_task);

void
console_sendf(const struct command_encoder *ce, va_list args)
{
    uint8_t buf[MESSAGE_MAX];
    uint_fast8_t msglen = command_encode_and_frame(buf, sizeof(buf), ce, args);
    if (!msglen)
        return;
    int ret = write(tty_fd, buf, msglen);
    if (ret < 0)
        report_errno("write", ret);
}

void
console_poll(int timeout_us)
{
    struct pollfd pfd = { .fd = tty_fd, .events = POLLIN };
    int ret = poll(&pfd, 1, timeout_us / 1000);
    if (ret > 0 && (pfd.revents & POLLIN))
        sched_wake_task(&console_wake);
}
