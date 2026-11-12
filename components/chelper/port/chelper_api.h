// C API of the Klipper chelper functions used from C++ code.
//
// The original chelper headers rely on GNU C extensions in list.h, so C++
// code includes this header (opaque structs only) instead of them.  The
// prototypes must match third_party/klipper/klippy/chelper/*.h.
//
// This file may be distributed under the terms of the GNU GPLv3 license.
#ifndef CHELPER_API_H
#define CHELPER_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct trapq;
struct stepper_kinematics;
struct stepcompress;
struct syncemitter;
struct steppersync;
struct steppersyncmgr;
struct serialqueue;

// trapq.h
struct trapq *trapq_alloc(void);
void trapq_free(struct trapq *tq);
void trapq_append(struct trapq *tq, double print_time
                  , double accel_t, double cruise_t, double decel_t
                  , double start_pos_x, double start_pos_y, double start_pos_z
                  , double axes_r_x, double axes_r_y, double axes_r_z
                  , double start_v, double cruise_v, double accel);
void trapq_finalize_moves(struct trapq *tq, double print_time
                          , double clear_history_time);
void trapq_set_position(struct trapq *tq, double print_time
                        , double pos_x, double pos_y, double pos_z);
#ifndef TRAPQ_H
struct pull_move {
    double print_time, move_t;
    double start_v, accel;
    double start_x, start_y, start_z;
    double x_r, y_r, z_r;
};
#endif
int trapq_extract_old(struct trapq *tq, struct pull_move *p, int max
                      , double start_time, double end_time);

// itersolve.h
void itersolve_set_trapq(struct stepper_kinematics *sk, struct trapq *tq
                         , double step_dist);
void itersolve_set_position(struct stepper_kinematics *sk
                            , double x, double y, double z);
double itersolve_get_commanded_pos(struct stepper_kinematics *sk);
double itersolve_get_gen_steps_pre_active(struct stepper_kinematics *sk);
double itersolve_get_gen_steps_post_active(struct stepper_kinematics *sk);

// kin_*.c
struct stepper_kinematics *corexy_stepper_alloc(char type);
struct stepper_kinematics *cartesian_stepper_alloc(char axis);
struct stepper_kinematics *input_shaper_alloc(void);
int input_shaper_set_sk(struct stepper_kinematics *sk
                        , struct stepper_kinematics *orig_sk);
int input_shaper_set_shaper_params(struct stepper_kinematics *sk, char axis
                                   , int n, double a[], double t[]);

// stepcompress.h
void stepcompress_fill(struct stepcompress *sc, uint32_t oid
                       , uint32_t max_error, int32_t queue_step_msgtag
                       , int32_t set_next_step_dir_msgtag);
void stepcompress_set_invert_sdir(struct stepcompress *sc
                                  , uint32_t invert_sdir);

// steppersync.h
struct stepcompress *syncemitter_get_stepcompress(struct syncemitter *se);
void syncemitter_set_stepper_kinematics(struct syncemitter *se
                                        , struct stepper_kinematics *sk);
void syncemitter_queue_msg(struct syncemitter *se, uint64_t req_clock
                           , uint32_t *data, int len);
struct syncemitter *steppersync_alloc_syncemitter(
    struct steppersync *ss, char name[16], int alloc_stepcompress);
void steppersync_setup_movequeue(struct steppersync *ss
                                 , struct serialqueue *sq, int move_num);
void steppersync_set_time(struct steppersync *ss, double time_offset
                          , double mcu_freq);
struct steppersyncmgr *steppersyncmgr_alloc(void);
void steppersyncmgr_free(struct steppersyncmgr *ssm);
struct steppersync *steppersyncmgr_alloc_steppersync(
    struct steppersyncmgr *ssm);
struct syncemitter *steppersyncmgr_gen_steps(struct steppersyncmgr *ssm
                                             , double flush_time
                                             , double gen_steps_time
                                             , double clear_history_time);

double itersolve_calc_position_from_coord(struct stepper_kinematics *sk
                                          , double x, double y, double z);
int64_t stepcompress_find_past_position(struct stepcompress *sc
                                        , uint64_t clock);
int stepcompress_reset(struct stepcompress *sc, uint64_t last_step_clock);
int stepcompress_set_last_position(struct stepcompress *sc, uint64_t clock
                                   , int64_t last_position);

// pyhelper_port.c
double get_monotonic(void);

// serialqueue.h (real serial queue, mcu_link component)
struct command_queue;
#define CHELPER_MESSAGE_MAX 64
#ifndef SERIALQUEUE_H // same layout as serialqueue.h
struct pull_queue_message {
    uint8_t msg[CHELPER_MESSAGE_MAX];
    int len;
    double sent_time, receive_time;
    uint64_t notify_id;
};
#endif
struct serialqueue *serialqueue_alloc(int serial_fd, char serial_fd_type
                                      , int client_id, char name[16]);
void serialqueue_exit(struct serialqueue *sq);
void serialqueue_free(struct serialqueue *sq);
struct command_queue *serialqueue_alloc_commandqueue(void);
void serialqueue_free_commandqueue(struct command_queue *cq);
void serialqueue_send(struct serialqueue *sq, struct command_queue *cq
                      , uint8_t *msg, int len, uint64_t min_clock
                      , uint64_t req_clock, uint64_t notify_id);
void serialqueue_pull(struct serialqueue *sq, struct pull_queue_message *pqm);
void serialqueue_set_wire_frequency(struct serialqueue *sq, double frequency);
void serialqueue_set_receive_window(struct serialqueue *sq, int receive_window);
void serialqueue_set_clock_est(struct serialqueue *sq, double est_freq
                               , double conv_time, uint64_t conv_clock);
void serialqueue_get_stats(struct serialqueue *sq, char *buf, int len);
int serialqueue_extract_old(struct serialqueue *sq, int sentq
                            , struct pull_queue_message *q, int max);

// trdispatch.c (homing trigger dispatch)
struct trdispatch;
struct trdispatch_mcu;
struct trdispatch *trdispatch_alloc(void);
struct trdispatch_mcu *trdispatch_mcu_alloc(
    struct trdispatch *td, struct serialqueue *sq, struct command_queue *cq
    , uint32_t trsync_oid, uint32_t set_timeout_msgtag
    , uint32_t trigger_msgtag, uint32_t state_msgtag);
void trdispatch_mcu_setup(struct trdispatch_mcu *tdm
                          , uint64_t last_status_clock, uint64_t expire_clock
                          , uint64_t expire_ticks, uint64_t min_extend_ticks);
void trdispatch_start(struct trdispatch *td, uint32_t dispatch_reason);
void trdispatch_stop(struct trdispatch *td);

// serialqueue_stub.c (benchmark message sink)
struct bench_sink_stats {
    uint64_t messages, bytes, batches, hash;
};
typedef void (*bench_sink_cb)(const uint8_t *msg, int len, uint64_t req_clock
                              , void *data);
void bench_sink_reset(void);
void bench_sink_get(struct bench_sink_stats *out);
void bench_sink_set_callback(bench_sink_cb cb, void *data);

#ifdef __cplusplus
}
#endif

#endif // chelper_api.h
