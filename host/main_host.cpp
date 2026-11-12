// Host entry point for the M1 benchmark.
//
//   bench_host <file.gcode> [--dump out.txt]
//
// --dump writes every generated MCU message (req_clock + encoded bytes) of
// the "x1 feed, zv shaper" run, for diffing against other builds.
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#ifdef __APPLE__
#include <malloc/malloc.h>
#else
#include <malloc.h>
#endif

#include "chelper_api.h"
#include "klippy_lite/bench.hpp"
#include "klippy_lite/platform.hpp"

namespace kl {
uint64_t
now_us()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
}
// Report a fake "free heap" of 1 GiB minus the bytes in use, so the
// benchmark's peak-use calculation works the same way as on the ESP32.
static int64_t
heap_in_use()
{
#ifdef __APPLE__
    malloc_statistics_t st;
    malloc_zone_statistics(nullptr, &st);
    return (int64_t)st.size_in_use;
#else
    return (int64_t)mallinfo2().uordblks;
#endif
}
int64_t heap_free_bytes() { return (1LL << 30) - heap_in_use(); }
int64_t heap_min_free_bytes() { return -1; }
void heap_monitor_start() {}
int64_t heap_monitor_stop() { return -1; }
std::string task_report() { return ""; }
} // namespace kl

static void
dump_cb(const uint8_t *msg, int len, uint64_t req_clock, void *data)
{
    FILE *f = (FILE *)data;
    fprintf(f, "%llu", (unsigned long long)req_clock);
    for (int i = 0; i < len; i++)
        fprintf(f, " %02x", msg[i]);
    fprintf(f, "\n");
}

int
main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <file.gcode> [--dump out.txt]\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 1;
    }
    std::string gcode;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        gcode.append(buf, n);
    fclose(f);
    FILE *dump = nullptr;
    if (argc >= 4 && strcmp(argv[2], "--dump") == 0)
        dump = fopen(argv[3], "w");

    printf("M1 benchmark (host build), %s, %zu bytes\n", argv[1], gcode.size());
    int failures = 0;
    for (kl::BenchConfig cfg : kl::default_suite()) {
        // BENCH_ONLY=<text>: run only configurations whose label contains it
        const char *only = getenv("BENCH_ONLY");
        if (only && !strstr(cfg.label, only))
            continue;
        // BENCH_KLIPPER=1: use Klipper's original buffer sizes for comparison
        if (getenv("BENCH_KLIPPER")) {
            cfg.toolhead.buffer_time_high = kl::BUFFER_TIME_HIGH;
            cfg.queuing = kl::MotionQueuingConfig();
        }
        bool want_dump = dump && strcmp(cfg.label, "x1 feed, zv shaper") == 0;
        bench_sink_set_callback(want_dump ? dump_cb : nullptr, dump);
        kl::BenchResult r = kl::run_bench(gcode.data(), gcode.size(), cfg);
        kl::print_result(r, stdout);
        failures += !r.ok;
    }
    bench_sink_set_callback(nullptr, nullptr);
    if (dump)
        fclose(dump);
    return failures ? 1 : 0;
}
