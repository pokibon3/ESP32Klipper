// Serial port opening for the host (macOS/Linux) build.
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "mcu_link/mcu.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

extern char **environ;

namespace kl {

static speed_t
baud_to_speed(int baud)
{
    switch (baud) {
    case 115200: return B115200;
    case 230400: return B230400;
    default: return B115200; // pty: ignored
    }
}

// "sim:<path to sim_mcu>": start the simulated MCU as a child process
// connected through a socketpair (no pty needed)
static pid_t sim_pid = -1;

static int
open_sim(const std::string &exe, std::string *err)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        if (err)
            *err = std::string("socketpair: ") + strerror(errno);
        return -1;
    }
    fcntl(sv[0], F_SETFD, FD_CLOEXEC);
    std::string fdarg = std::to_string(sv[1]);
    const char *argv[] = {exe.c_str(), "-F", fdarg.c_str(), nullptr};
    int ret = posix_spawn(&sim_pid, exe.c_str(), nullptr, nullptr,
                          (char *const *)argv, environ);
    close(sv[1]);
    if (ret) {
        close(sv[0]);
        if (err)
            *err = "Unable to start " + exe + ": " + strerror(ret);
        return -1;
    }
    fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL) | O_NONBLOCK);
    return sv[0];
}

int
mcu_open_serial(const std::string &port, int baud, std::string *err)
{
    if (port.compare(0, 4, "sim:") == 0)
        return open_sim(port.substr(4), err);
    int fd = open(port.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        if (err)
            *err = "Unable to open " + port + ": " + strerror(errno);
        return -1;
    }
    struct termios t;
    if (tcgetattr(fd, &t) == 0) {
        cfmakeraw(&t);
        cfsetspeed(&t, baud_to_speed(baud));
        t.c_cflag |= CLOCAL | CREAD;
        tcsetattr(fd, TCSANOW, &t);
    }
    // Non-standard rates (250000) via IOSSIOSPEED on macOS are only
    // needed for real USB-serial adapters; ptys ignore the baud rate.
    return fd;
}

void
mcu_close_serial(int fd)
{
    close(fd);
    if (sim_pid > 0) {
        kill(sim_pid, SIGINT);
        waitpid(sim_pid, nullptr, 0);
        sim_pid = -1;
    }
}

} // namespace kl
