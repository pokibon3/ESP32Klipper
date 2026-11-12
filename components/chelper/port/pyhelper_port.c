// Replacement for klippy/chelper/pyhelper.c on ESP-IDF and host builds.
// Only the functions referenced by the chelper files we compile are provided.
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <stdarg.h> // va_start
#include <stdio.h>  // vfprintf
#include <string.h> // strerror
#include <time.h>   // clock_gettime

double
get_monotonic(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * .000000001;
}

void
errorf(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "chelper error: ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}

void
report_errno(char *where, int rc)
{
    fprintf(stderr, "chelper error: %s rc=%d (%s)\n", where, rc, strerror(rc));
}

int
set_thread_name(char name[16])
{
    (void)name;
    return 0;
}
