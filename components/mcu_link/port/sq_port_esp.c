// eventfd based pipe() replacement for serialqueue.c (see sq_port_esp.h).
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_vfs_eventfd.h"
#include "freertos/FreeRTOS.h"

#define MAX_PIPES 4
static int pipe_fds[MAX_PIPES] = {-1, -1, -1, -1};
static portMUX_TYPE pipe_lock = portMUX_INITIALIZER_UNLOCKED;

static int
is_pipe(int fd)
{
    for (int i = 0; i < MAX_PIPES; i++)
        if (pipe_fds[i] == fd && fd >= 0)
            return 1;
    return 0;
}

int
sq_port_pipe(int fds[2])
{
    static int registered;
    if (!registered) {
        esp_vfs_eventfd_config_t cfg = ESP_VFS_EVENTD_CONFIG_DEFAULT();
        esp_err_t ret = esp_vfs_eventfd_register(&cfg);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            errno = ENOMEM;
            return -1;
        }
        registered = 1;
    }
    int efd = eventfd(0, 0);
    if (efd < 0)
        return -1;
    portENTER_CRITICAL(&pipe_lock);
    for (int i = 0; i < MAX_PIPES; i++)
        if (pipe_fds[i] < 0) {
            pipe_fds[i] = efd;
            break;
        }
    portEXIT_CRITICAL(&pipe_lock);
    fds[0] = fds[1] = efd;
    return 0;
}

ssize_t
sq_port_read(int fd, void *buf, size_t n)
{
    if (!is_pipe(fd))
        return read(fd, buf, n);
    uint64_t v;
    ssize_t ret = read(fd, &v, sizeof(v));
    return ret < 0 ? ret : (ssize_t)(v ? 1 : 0);
}

ssize_t
sq_port_write(int fd, const void *buf, size_t n)
{
    if (!is_pipe(fd))
        return write(fd, buf, n);
    uint64_t one = 1;
    ssize_t ret = write(fd, &one, sizeof(one));
    return ret < 0 ? ret : (ssize_t)n;
}

int
sq_port_fcntl(int fd, int cmd, ...)
{
    va_list ap;
    va_start(ap, cmd);
    int arg = va_arg(ap, int);
    va_end(ap);
    if (is_pipe(fd))
        return 0; // eventfd has no fcntl; select() never blocks reads
    return fcntl(fd, cmd, arg);
}

// serialqueue_free() closes both pipe ends, which are the same eventfd
// here: close it on the first call and ignore the immediate second one.
static int last_closed_pipe = -1;

int
sq_port_close(int fd)
{
    int was_pipe = 0;
    portENTER_CRITICAL(&pipe_lock);
    for (int i = 0; i < MAX_PIPES; i++)
        if (pipe_fds[i] == fd && fd >= 0) {
            pipe_fds[i] = -1;
            was_pipe = 1;
        }
    int second = !was_pipe && fd == last_closed_pipe;
    last_closed_pipe = was_pipe ? fd : -1;
    portEXIT_CRITICAL(&pipe_lock);
    if (second)
        return 0;
    return close(fd);
}
