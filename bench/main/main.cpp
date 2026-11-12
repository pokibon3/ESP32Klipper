// M1 benchmark firmware entry point (CYD / M5Stack CoreS3).
//
// Runs the standard suite (see klippy_lite/bench.hpp) on the embedded
// card_opt.gcode twice:
//   A) step generation threads may run on both cores
//   B) step generation threads pinned to core 1 (core 0 left for WiFi/UI,
//      as planned in the design document)
// The benchmark task itself runs on core 1.  Results go to the console.
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <cstdio>

#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_pthread.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "klippy_lite/bench.hpp"
#include "klippy_lite/platform.hpp"

extern const char gcode_start[] asm("_binary_card_opt_gcode_start");
extern const char gcode_end[] asm("_binary_card_opt_gcode_end");

namespace kl {
uint64_t now_us() { return (uint64_t)esp_timer_get_time(); }
int64_t heap_free_bytes() { return heap_caps_get_free_size(MALLOC_CAP_8BIT); }
int64_t heap_min_free_bytes()
{
    return heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
}
void heap_monitor_start() { heap_caps_monitor_local_minimum_free_size_start(); }
int64_t heap_monitor_stop()
{
    int64_t v = heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
    heap_caps_monitor_local_minimum_free_size_stop();
    return v;
}
std::string task_report() { return ""; }
} // namespace kl

static void
set_stepgen_thread_core(int core)
{
    esp_pthread_cfg_t cfg = esp_pthread_get_default_config();
    cfg.stack_size = CONFIG_PTHREAD_TASK_STACK_SIZE_DEFAULT;
    cfg.prio = 5;
    cfg.pin_to_core = core;
    cfg.thread_name = "stepgen";
    esp_pthread_set_cfg(&cfg);
}

static void
bench_task(void *)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    // EMBED_TXTFILES appends a NUL terminator
    size_t len = (size_t)(gcode_end - gcode_start) - 1;
    printf("\nM1 benchmark (%s rev %d, %d cores, %d MHz, IDF %s)\n",
           CONFIG_IDF_TARGET, chip.revision, chip.cores,
           CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, esp_get_idf_version());
    printf("card_opt.gcode: %u bytes, heap free %u bytes\n", (unsigned)len,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));

    struct Mode {
        const char *name;
        int core;
    };
    const Mode modes[] = {
        {"A: step generation threads on any core", tskNO_AFFINITY},
        {"B: step generation threads pinned to core 1", 1},
    };
    for (const Mode &m : modes) {
        printf("\n##### %s #####\n", m.name);
        set_stepgen_thread_core(m.core);
        for (const kl::BenchConfig &cfg : kl::default_suite()) {
            kl::BenchResult r = kl::run_bench(gcode_start, len, cfg);
            kl::print_result(r, stdout);
            fflush(stdout);
        }
    }
    printf("\nM1 benchmark done. Lowest free heap since boot: %u bytes\n",
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
    vTaskDelete(nullptr);
}

// chelper does not check malloc() results, so report the failure and stop
// before it dereferences NULL.
static void
alloc_failed(size_t size, uint32_t caps, const char *function_name)
{
    esp_rom_printf("\n*** malloc(%u) failed in %s: free %u, largest block %u,"
                   " lowest free since boot %u\n", (unsigned)size,
                   function_name ? function_name : "?",
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                   (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
    abort();
}

extern "C" void
app_main(void)
{
    heap_caps_register_failed_alloc_callback(alloc_failed);
    xTaskCreatePinnedToCore(bench_task, "bench", 16384, nullptr, 5, nullptr, 1);
}
