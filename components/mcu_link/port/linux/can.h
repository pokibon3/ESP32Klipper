// Stand-in for <linux/can.h> so serialqueue.c builds on macOS and
// ESP-IDF.  CAN bus transport is not used by this project.
#ifndef PORT_LINUX_CAN_H
#define PORT_LINUX_CAN_H

#include <stdint.h>

struct can_frame {
    uint32_t can_id;
    uint8_t can_dlc;
    uint8_t __pad, __res0, __res1;
    uint8_t data[8] __attribute__((aligned(8)));
};

#endif
