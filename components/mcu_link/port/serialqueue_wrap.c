// Klipper's serialqueue.c, compiled unmodified, plus one helper that needs
// its private struct definitions.
//
// serialqueue keeps the last 100 sent and 100 received messages for
// debug dumps (~22 KB of heap).  On the ESP32 that is too much, so the
// host trims the lists right after serialqueue_alloc(); the remaining
// entries keep cycling exactly as before.
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "serialqueue.c"

static void
trim_list(struct list_head *root, int keep)
{
    int count = 0;
    struct queue_message *qm, *tmp;
    list_for_each_entry_safe(qm, tmp, root, node) {
        if (++count > keep) {
            list_del(&qm->node);
            message_free(qm);
        }
    }
}

void
serialqueue_trim_debug(struct serialqueue *sq, int keep)
{
    if (keep < 1)
        keep = 1;
    pthread_mutex_lock(&sq->lock);
    trim_list(&sq->old_sent, keep);
    pthread_mutex_unlock(&sq->lock);
    pthread_mutex_lock(&sq->receiver.lock);
    trim_list(&sq->receiver.old_receive, keep);
    pthread_mutex_unlock(&sq->receiver.lock);
}
