// Simulated TMC2209 drivers behind Klipper's tmcuart commands.
//
// Replaces src/tmcuart.c in the simulated MCU.  Command and response
// formats are identical; instead of bit-banging a pin, the request is
// decoded (start/stop bits, CRC8) and answered by up to four emulated
// TMC2209 register files (UART addresses 0-3), like drivers on a shared
// SKR Pico UART.  Writes bump IFCNT, so Klipper's write verification
// works; "sim_tmc_get" lets tests read the register values back.
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <string.h> // memcpy
#include "basecmd.h" // oid_alloc
#include "command.h" // DECL_COMMAND
#include "sched.h" // DECL_TASK

#define NUM_DRIVERS 4
static struct tmc_sim {
    uint32_t regs[128];
    uint8_t ifcnt;
} drivers[NUM_DRIVERS];

struct tmcuart_s {
    uint8_t pending, read_len, data[10];
};

static struct task_wake tmcuart_wake;

static uint8_t
calc_crc8(const uint8_t *data, int len)
{
    // tmc_uart.py _calc_crc8()
    uint8_t crc = 0;
    for (int i = 0; i < len; i++) {
        uint8_t b = data[i];
        for (int j = 0; j < 8; j++) {
            if ((crc >> 7) ^ (b & 0x01))
                crc = (crc << 1) ^ 0x07;
            else
                crc = crc << 1;
            b >>= 1;
        }
    }
    return crc;
}

// Undo tmc_uart.py _add_serial_bits(): 10 bits per byte (start, 8 data
// bits LSB first, stop), packed LSB first
static int
decode_serial(const uint8_t *in, int in_len, uint8_t *out, int max)
{
    int nbits = in_len * 8, count = 0;
    for (int pos = 0; pos + 10 <= nbits && count < max; pos += 10) {
        uint32_t v = 0;
        for (int b = 0; b < 10; b++)
            v |= ((in[(pos + b) / 8] >> ((pos + b) % 8)) & 1) << b;
        if ((v & 1) || !(v & 0x200))
            return -1; // bad start/stop bit
        out[count++] = (v >> 1) & 0xff;
    }
    return count;
}

static void
encode_serial(const uint8_t *in, int len, uint8_t *out)
{
    memset(out, 0, (len * 10 + 7) / 8);
    for (int i = 0; i < len; i++) {
        uint32_t b = ((uint32_t)in[i] << 1) | 0x200;
        for (int k = 0; k < 10; k++)
            if (b & (1 << k)) {
                int pos = i * 10 + k;
                out[pos / 8] |= 1 << (pos % 8);
            }
    }
}

static void
reset_driver(struct tmc_sim *d)
{
    memset(d, 0, sizeof(*d));
    d->regs[0x00] = 0x00000041;         // GCONF (pdn_disable, i_scale_analog)
    d->regs[0x01] = 0x00000001;         // GSTAT: reset
    d->regs[0x06] = 0x21000000 | 0x40;  // IOIN: version 0x21
    d->regs[0x6c] = 0x10000053;         // CHOPCONF power-on default
    d->regs[0x6f] = 0x80000000;         // DRV_STATUS: stst
}

void
command_config_tmcuart(uint32_t *args)
{
    oid_alloc(args[0], command_config_tmcuart, sizeof(struct tmcuart_s));
    static int init;
    if (!init) {
        for (int i = 0; i < NUM_DRIVERS; i++)
            reset_driver(&drivers[i]);
        init = 1;
    }
}
DECL_COMMAND(command_config_tmcuart,
             "config_tmcuart oid=%c rx_pin=%u pull_up=%c"
             " tx_pin=%u bit_time=%u");

void
command_tmcuart_send(uint32_t *args)
{
    struct tmcuart_s *t = oid_lookup(args[0], command_config_tmcuart);
    uint8_t write_len = args[1];
    uint8_t *write = command_decode_ptr(args[2]);
    uint8_t read_len = args[3];
    if (write_len > sizeof(t->data) || read_len > sizeof(t->data))
        shutdown("tmcuart data too large");
    uint8_t msg[8];
    int n = decode_serial(write, write_len, msg, sizeof(msg));
    t->read_len = 0;
    if (n == 8 && calc_crc8(msg, 7) == msg[7] && msg[1] < NUM_DRIVERS
        && (msg[2] & 0x80)) {
        // Write datagram: sync, addr, reg|0x80, data[4], crc
        struct tmc_sim *d = &drivers[msg[1]];
        uint8_t reg = msg[2] & 0x7f;
        uint32_t val = ((uint32_t)msg[3] << 24) | ((uint32_t)msg[4] << 16)
                       | ((uint32_t)msg[5] << 8) | msg[6];
        if (reg == 0x01)
            d->regs[reg] &= ~val; // GSTAT: write 1 to clear
        else
            d->regs[reg] = val;
        d->ifcnt++;
    } else if (n == 4 && calc_crc8(msg, 3) == msg[3] && msg[1] < NUM_DRIVERS
               && read_len == 10) {
        // Read request: sync, addr, reg, crc -> reply 0x05 0xff reg data crc
        struct tmc_sim *d = &drivers[msg[1]];
        uint8_t reg = msg[2] & 0x7f;
        uint32_t val = reg == 0x02 ? d->ifcnt : d->regs[reg];
        uint8_t rep[8] = {0x05, 0xff, reg, val >> 24, val >> 16, val >> 8, val};
        rep[7] = calc_crc8(rep, 7);
        encode_serial(rep, 8, t->data);
        t->read_len = 10;
    }
    // No reply (bad CRC, unknown address) -> empty read like a timeout
    t->pending = 1;
    sched_wake_task(&tmcuart_wake);
    if (read_len && !t->read_len) {
        t->read_len = read_len;
        memset(t->data, 0, sizeof(t->data));
    }
}
DECL_COMMAND(command_tmcuart_send, "tmcuart_send oid=%c write=%*s read=%c");

void
tmcuart_task(void)
{
    if (!sched_check_wake(&tmcuart_wake))
        return;
    uint8_t oid;
    struct tmcuart_s *t;
    foreach_oid(oid, t, command_config_tmcuart) {
        if (!t->pending)
            continue;
        t->pending = 0;
        sendf("tmcuart_response oid=%c read=%*s", oid, t->read_len, t->data);
    }
}
DECL_TASK(tmcuart_task);

// Test hook: read an emulated driver register
void
command_sim_tmc_get(uint32_t *args)
{
    uint8_t addr = args[0], reg = args[1] & 0x7f;
    if (addr >= NUM_DRIVERS)
        shutdown("Invalid sim tmc address");
    uint32_t val = reg == 0x02 ? drivers[addr].ifcnt : drivers[addr].regs[reg];
    sendf("sim_tmc_reg addr=%c reg=%c val=%u", addr, reg, val);
}
DECL_COMMAND_FLAGS(command_sim_tmc_get, HF_IN_SHUTDOWN,
                   "sim_tmc_get addr=%c reg=%c");
