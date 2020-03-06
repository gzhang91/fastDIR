#ifndef _FDIR_SERVER_TYPES_H
#define _FDIR_SERVER_TYPES_H

#include <time.h>
#include <pthread.h>
#include "fastcommon/common_define.h"
#include "fastcommon/fast_task_queue.h"
#include "fastcommon/fast_mblock.h"
#include "fastcommon/fast_allocator.h"
#include "fastcommon/uniq_skiplist.h"
#include "fastcommon/server_id_func.h"
#include "common/fdir_types.h"

#define FDIR_CLUSTER_ID_BITS                 10
#define FDIR_CLUSTER_ID_MAX                  ((1 << FDIR_CLUSTER_ID_BITS) - 1)

#define FDIR_SERVER_DEFAULT_RELOAD_INTERVAL       500
#define FDIR_SERVER_DEFAULT_CHECK_ALIVE_INTERVAL  300
#define FDIR_NAMESPACE_HASHTABLE_CAPACITY        1361
#define FDIR_DEFAULT_DATA_THREAD_COUNT              1

typedef void (*server_free_func)(void *ptr);
typedef void (*server_free_func_ex)(void *ctx, void *ptr);

typedef struct fdir_dentry_status {
    int mode;
    int ctime;  /* create time */
    int mtime;  /* modify time */
    int64_t size;   /* file size in bytes */
} FDIRDEntryStatus;

typedef struct fdir_path_info {
    string_t paths[FDIR_MAX_PATH_COUNT];   //splited path parts
    int count;
} FDIRPathInfo;

struct fdir_server_dentry;
typedef struct fdir_server_dentry_array {
    int alloc;
    int count;
    struct fdir_server_dentry **entries;
} FDIRServerDentryArray;

typedef struct fdir_cluster_server_info {
    FCServerInfo *server;
    char key[FDIR_REPLICA_KEY_SIZE];   //for slave server
    char status;                       //the slave status
} FDIRClusterServerInfo;

typedef struct fdir_cluster_server_array {
    int count;
    FDIRClusterServerInfo *servers;
} FDIRClusterServerArray;

struct fdir_binlog_record;

typedef struct server_task_arg {
    volatile int64_t task_version;
    int64_t req_start_time;
    struct {
        FDIRServerDentryArray array;
        int64_t token;
        int offset;
        time_t expires;  //expire time
    } dentry_list_cache; //for dentry_list

    struct {
        FDIRRequestInfo request;
        FDIRResponseInfo response;

        struct fdir_binlog_record *record;
        int (*deal_func)(struct fast_task_info *task);
        volatile int waiting_rpc_count;
        bool response_done;
        bool log_error;
    } context;

    FDIRClusterServerInfo *cluster_peer;  //the peer server in the cluster
} FDIRServerTaskArg;

typedef struct fdir_server_context {
    struct fast_mblock_man record_allocator;
} FDIRServerContext;

typedef struct fdir_slave_array {
    int count;
    FDIRClusterServerInfo **servers;
} FDIRServerSlaveArray;

typedef struct fdir_server_cluster {
    int64_t version;
    struct {
        FDIRServerSlaveArray inactives;
        FDIRServerSlaveArray actives;
    } slaves;
    FDIRClusterServerInfo *master;
} FDIRServerCluster;

#endif
