// Stand-in for klippy/chelper/serialqueue.c used by the M1 benchmark.
//
// steppersync.c hands finished MCU commands (queue_step, set_next_step_dir,
// queue_digital_out, ...) to serialqueue_send_batch().  Instead of writing
// them to a UART, this stub accounts for them (count, bytes, hash) so the
// host and ESP32 builds can be compared, and then frees them.
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <stddef.h> // offsetof
#include <stdlib.h> // malloc
#include <string.h> // memset
#include "serialqueue.h" // serialqueue_send_batch
#include "chelper_api.h" // bench_sink_stats

struct command_queue {
    int dummy;
};

static struct bench_sink_stats sink_stats;
static bench_sink_cb sink_cb;
static void *sink_cb_data;

#define FNV_OFFSET 0xcbf29ce484222325ULL
#define FNV_PRIME  0x100000001b3ULL

static uint64_t
fnv1a(uint64_t h, const uint8_t *p, int len)
{
    int i;
    for (i = 0; i < len; i++) {
        h ^= p[i];
        h *= FNV_PRIME;
    }
    return h;
}

void
bench_sink_reset(void)
{
    memset(&sink_stats, 0, sizeof(sink_stats));
    sink_stats.hash = FNV_OFFSET;
}

void
bench_sink_get(struct bench_sink_stats *out)
{
    *out = sink_stats;
}

void
bench_sink_set_callback(bench_sink_cb cb, void *data)
{
    sink_cb = cb;
    sink_cb_data = data;
}

struct command_queue *
serialqueue_alloc_commandqueue(void)
{
    struct command_queue *cq = malloc(sizeof(*cq));
    memset(cq, 0, sizeof(*cq));
    return cq;
}

void
serialqueue_free_commandqueue(struct command_queue *cq)
{
    free(cq);
}

void
serialqueue_send_batch(struct serialqueue *sq, struct command_queue *cq
                       , struct list_head *msgs)
{
    (void)sq;
    (void)cq;
    sink_stats.batches++;
    while (!list_empty(msgs)) {
        struct queue_message *qm = list_first_entry(
            msgs, struct queue_message, node);
        list_del(&qm->node);
        uint64_t req_clock = qm->req_clock;
        sink_stats.messages++;
        sink_stats.bytes += qm->len;
        sink_stats.hash = fnv1a(sink_stats.hash, qm->msg, qm->len);
        sink_stats.hash = fnv1a(sink_stats.hash, (uint8_t *)&req_clock
                                , sizeof(req_clock));
        if (sink_cb)
            sink_cb(qm->msg, qm->len, req_clock, sink_cb_data);
        message_free(qm);
    }
}
