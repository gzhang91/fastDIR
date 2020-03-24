//replica_consumer_thread.h

#ifndef _REPLICA_CONSUMER_THREAD_H_
#define _REPLICA_CONSUMER_THREAD_H_

#include "fastcommon/fast_mblock.h"
#include "binlog_types.h"
#include "binlog_replay.h"

#define REPLICA_CONSUMER_THREAD_INPUT_BUFFER_COUNT   32
#define REPLICA_CONSUMER_THREAD_OUTPUT_BUFFER_COUNT   4
#define REPLICA_CONSUMER_THREAD_BUFFER_COUNT      \
    (REPLICA_CONSUMER_THREAD_INPUT_BUFFER_COUNT + \
     REPLICA_CONSUMER_THREAD_OUTPUT_BUFFER_COUNT)

typedef struct replica_consumer_thread_result {
    short err_no;
    int64_t data_version;
} RecordProcessResult;

typedef struct replica_consumer_thread_context {
    volatile bool continue_flag;
    bool runnings[2];
    pthread_t tids[2];
    struct fast_mblock_man result_allocater;
    ServerBinlogRecordBuffer binlog_buffers[REPLICA_CONSUMER_THREAD_BUFFER_COUNT];
    struct {
        struct common_blocked_queue input_free;  //free ServerBinlogRecordBuffer ptr
        struct common_blocked_queue output_free; //free ServerBinlogRecordBuffer ptr

        struct common_blocked_queue input;  //input ServerBinlogRecordBuffer ptr
        struct common_blocked_queue output; //output ServerBinlogRecordBuffer ptr

        struct common_blocked_queue result; //record deal result
    } queues;
    struct fast_task_info *task;
    BinlogReplayContext replay_ctx;
} ReplicaConsumerThreadContext;

#ifdef __cplusplus
extern "C" {
#endif

ReplicaConsumerThreadContext *replica_consumer_thread_init(
        struct fast_task_info *task, const int buffer_size, int *err_no);

int deal_replica_push_request(ReplicaConsumerThreadContext *ctx);
int deal_replica_push_result(ReplicaConsumerThreadContext *ctx);

void replica_consumer_thread_terminate(ReplicaConsumerThreadContext *ctx);

#ifdef __cplusplus
}
#endif

#endif
