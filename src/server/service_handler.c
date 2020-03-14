//service_handler.c

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include "fastcommon/logger.h"
#include "fastcommon/sockopt.h"
#include "fastcommon/shared_func.h"
#include "fastcommon/pthread_func.h"
#include "fastcommon/sched_thread.h"
#include "fastcommon/ioevent_loop.h"
#include "fastcommon/json_parser.h"
#include "sf/sf_util.h"
#include "sf/sf_func.h"
#include "sf/sf_nio.h"
#include "sf/sf_global.h"
#include "common/fdir_proto.h"
#include "binlog/binlog_producer.h"
#include "binlog/binlog_pack.h"
#include "server_global.h"
#include "server_func.h"
#include "dentry.h"
#include "cluster_relationship.h"
#include "cluster_topology.h"
#include "service_handler.h"

static volatile int64_t next_token;   //next token for dentry list

int service_handler_init()
{
    next_token = ((int64_t)g_current_time) << 32;
    return 0;
}

int service_handler_destroy()
{   
    return 0;
}

void server_task_finish_cleanup(struct fast_task_info *task)
{
    //FDIRServerTaskArg *task_arg;

    //task_arg = (FDIRServerTaskArg *)task->arg;

    logInfo("file: "__FILE__", line: %d task: %p", __LINE__, task);

    dentry_array_free(&DENTRY_LIST_CACHE.array);

    __sync_add_and_fetch(&((FDIRServerTaskArg *)task->arg)->task_version, 1);
    sf_task_finish_clean_up(task);
}

static int server_deal_actvie_test(struct fast_task_info *task)
{
    return server_expect_body_length(task, 0);
}

static int server_parse_dentry_info(struct fast_task_info *task,
        char *start, FDIRDEntryFullName *fullname)
{
    FDIRProtoDEntryInfo *proto_dentry;

    proto_dentry = (FDIRProtoDEntryInfo *)start;
    fullname->ns.len = proto_dentry->ns_len;
    fullname->ns.str = proto_dentry->ns_str;
    fullname->path.len = buff2short(proto_dentry->path_len);
    fullname->path.str = proto_dentry->ns_str + fullname->ns.len;

    if (fullname->ns.len <= 0) {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "invalid namespace length: %d <= 0",
                fullname->ns.len);
        return EINVAL;
    }

    if (fullname->path.len <= 0) {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "invalid path length: %d <= 0",
                fullname->path.len);
        return EINVAL;
    }
    if (fullname->path.len > PATH_MAX) {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "invalid path length: %d > %d",
                fullname->path.len, PATH_MAX);
        return EINVAL;
    }

    if (fullname->path.str[0] != '/') {
        RESPONSE.error.length = snprintf(
                RESPONSE.error.message,
                sizeof(RESPONSE.error.message),
                "invalid path: %.*s", fullname->path.len,
                fullname->path.str);
        return EINVAL;
    }

    return 0;
}

static int server_check_and_parse_dentry(struct fast_task_info *task,
        const int front_part_size, const int fixed_part_size,
        FDIRDEntryFullName *fullname)
{
    int result;
    int req_body_len;

    if ((result=server_check_body_length(task,
                    fixed_part_size + 1, fixed_part_size +
                    NAME_MAX + PATH_MAX)) != 0)
    {
        return result;
    }

    if ((result=server_parse_dentry_info(task, REQUEST.body +
                    front_part_size, fullname)) != 0)
    {
        return result;
    }

    req_body_len = fixed_part_size + fullname->ns.len +
        fullname->path.len;
    if (req_body_len != REQUEST.header.body_len) {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "body length: %d != expect: %d",
                REQUEST.header.body_len, req_body_len);
        return EINVAL;
    }

    return 0;
}

/*
static void server_get_dentry_hashcode(FDIRPathInfo *path_info,
        const bool include_last)
{
    char logic_path[NAME_MAX + PATH_MAX + 2];
    int len;
    string_t *part;
    string_t *end;
    char *p;

    p = logic_path;
    memcpy(p, path_info->fullname.ns.str, path_info->fullname.ns.len);
    p += path_info->fullname.ns.len;

    if (include_last) {
        end = path_info->paths + path_info->count;
    } else {
        end = path_info->paths + path_info->count - 1;
    }

    for (part=path_info->paths; part<end; part++) {
        *p++ = '/';
        memcpy(p, part->str, part->len);
        p += part->len;
    }

    len = p - logic_path;
    //logInfo("logic_path for hash code: %.*s", len, logic_path);
    path_info->hash_code = simple_hash(logic_path, len);
}

#define server_get_parent_hashcode(path_info)  \
    server_get_dentry_hashcode(path_info, false)

#define server_get_my_hashcode(path_info)  \
    server_get_dentry_hashcode(path_info, true)
*/

static int server_binlog_produce(struct fast_task_info *task)
{
    ServerBinlogRecordBuffer *rbuffer;
    int result;

    if ((rbuffer=server_binlog_alloc_rbuffer()) == NULL) {
        return ENOMEM;
    }

    TASK_ARG->context.deal_func = NULL;
    rbuffer->data_version = RECORD->data_version;
    rbuffer->args = task;
    RECORD->timestamp = g_current_time;

    fast_buffer_reset(&rbuffer->buffer);
    result = binlog_pack_record(RECORD, &rbuffer->buffer);

    fast_mblock_free_object(&((FDIRServerContext *)task->thread_data->arg)->
            service.record_allocator, RECORD);
    RECORD = NULL;

    if (result == 0) {
        result = server_binlog_dispatch(rbuffer);
    }

    if (result == 0) {
        return SLAVE_SERVER_COUNT > 0 ? TASK_STATUS_CONTINUE : result;
    } else {
        return result;
    }
}

static inline int alloc_record_object(struct fast_task_info *task)
{
    RECORD = (FDIRBinlogRecord *)fast_mblock_alloc_object(
            &((FDIRServerContext *)task->thread_data->arg)->
            service.record_allocator);
    if (RECORD == NULL) {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "system busy, please try later");
        return EBUSY;
    }

    return 0;
}

static void record_deal_done_notify(const int result, void *args)
{
    struct fast_task_info *task;
    task = (struct fast_task_info *)args;
    RESPONSE_STATUS = result;
    sf_nio_notify(task, SF_NIO_STAGE_CONTINUE);
}

static int handle_record_deal_done(struct fast_task_info *task)
{
    if (RESPONSE_STATUS == 0) {
        return server_binlog_produce(task);
    } else {
        return RESPONSE_STATUS;
    }
}

static inline int push_record_to_data_thread_queue(struct fast_task_info *task)
{
    int result;

    RECORD->notify.func = record_deal_done_notify;
    RECORD->notify.args = task;

    TASK_ARG->context.deal_func = handle_record_deal_done;
    result = push_to_data_thread_queue(RECORD);
    return result == 0 ? TASK_STATUS_CONTINUE : result;
}

#define SERVER_SET_RECORD_PATH_INFO()  \
    do {   \
        RECORD->hash_code = simple_hash(RECORD->fullname.ns.str,  \
                RECORD->fullname.ns.len);  \
        RECORD->inode = RECORD->data_version = 0;  \
        RECORD->options.flags = 0;   \
        RECORD->options.path_info.flags = BINLOG_OPTIONS_PATH_ENABLED;  \
    } while (0)

static int server_deal_create_dentry(struct fast_task_info *task)
{
    int result;
    FDIRProtoCreateDEntryFront *proto_front;
    //int flags;

    if ((result=alloc_record_object(task)) != 0) {
        return result;
    }

    if ((result=server_check_and_parse_dentry(task,
                    sizeof(FDIRProtoCreateDEntryFront),
                    sizeof(FDIRProtoCreateDEntryBody),
                    &RECORD->fullname)) != 0)
    {
        return result;
    }

    SERVER_SET_RECORD_PATH_INFO();

    proto_front = (FDIRProtoCreateDEntryFront *)REQUEST.body;
    //flags = buff2int(proto_front->flags);
    RECORD->stat.mode = buff2int(proto_front->mode);

    RECORD->operation = BINLOG_OP_CREATE_DENTRY_INT;
    RECORD->stat.ctime = RECORD->stat.mtime = g_current_time;
    RECORD->options.ctime = RECORD->options.mtime = 1;
    RECORD->options.mode = 1;
    return push_record_to_data_thread_queue(task);
}

static int server_deal_remove_dentry(struct fast_task_info *task)
{
    int result;

    if ((result=alloc_record_object(task)) != 0) {
        return result;
    }

    if ((result=server_check_and_parse_dentry(task,
                    0, sizeof(FDIRProtoRemoveDEntry),
                    &RECORD->fullname)) != 0)
    {
        return result;
    }

    SERVER_SET_RECORD_PATH_INFO();
    RECORD->operation = BINLOG_OP_REMOVE_DENTRY_INT;
    return push_record_to_data_thread_queue(task);
}

static int server_list_dentry_output(struct fast_task_info *task)
{
    FDIRProtoListDEntryRespBodyHeader *body_header;
    FDIRServerDentry **dentry;
    FDIRServerDentry **start;
    FDIRServerDentry **end;
    FDIRProtoListDEntryRespBodyPart *body_part;
    char *p;
    char *buf_end;
    int remain_count;
    int count;

    remain_count = DENTRY_LIST_CACHE.array.count -
        DENTRY_LIST_CACHE.offset;

    buf_end = task->data + task->size;
    p = REQUEST.body + sizeof(FDIRProtoListDEntryRespBodyHeader);
    start = DENTRY_LIST_CACHE.array.entries +
        DENTRY_LIST_CACHE.offset;
    end = start + remain_count;
    for (dentry=start; dentry<end; dentry++) {
        if (buf_end - p < sizeof(FDIRProtoListDEntryRespBodyPart) +
                (*dentry)->name.len)
        {
            break;
        }
        body_part = (FDIRProtoListDEntryRespBodyPart *)p;
        body_part->name_len = (*dentry)->name.len;
        memcpy(body_part->name_str, (*dentry)->name.str, (*dentry)->name.len);
        p += sizeof(FDIRProtoListDEntryRespBodyPart) + (*dentry)->name.len;
    }
    count = dentry - start;
    RESPONSE.header.body_len = p - REQUEST.body;
    RESPONSE.header.cmd = FDIR_SERVICE_PROTO_LIST_DENTRY_RESP;

    body_header = (FDIRProtoListDEntryRespBodyHeader *)REQUEST.body;
    int2buff(count, body_header->count);
    if (count < remain_count) {
        DENTRY_LIST_CACHE.offset += count;
        DENTRY_LIST_CACHE.expires = g_current_time + 60;
        DENTRY_LIST_CACHE.token = __sync_add_and_fetch(&next_token, 1);

        body_header->is_last = 0;
        long2buff(DENTRY_LIST_CACHE.token, body_header->token);
    } else {
        body_header->is_last = 1;
        long2buff(0, body_header->token);
    }

    TASK_ARG->context.response_done = true;
    return 0;
}

static int server_deal_list_dentry_first(struct fast_task_info *task)
{
    int result;
    FDIRDEntryFullName fullname;

    if ((result=server_check_and_parse_dentry(task,
                    0, sizeof(FDIRProtoListDEntryFirstBody),
                    &fullname)) != 0)
    {
        return result;
    }

    if ((result=dentry_list(&fullname, &DENTRY_LIST_CACHE.array)) != 0) {
        return result;
    }

    DENTRY_LIST_CACHE.offset = 0;
    return server_list_dentry_output(task);
}

static int server_deal_list_dentry_next(struct fast_task_info *task)
{
    FDIRProtoListDEntryNextBody *next_body;
    int result;
    int offset;
    int64_t token;

    if ((result=server_expect_body_length(task,
                    sizeof(FDIRProtoListDEntryNextBody))) != 0)
    {
        return result;
    }

    if (DENTRY_LIST_CACHE.expires < g_current_time) {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "dentry list cache expires, please try again");
        return ETIMEDOUT;
    }

    next_body = (FDIRProtoListDEntryNextBody *)REQUEST.body;
    token = buff2long(next_body->token);
    offset = buff2int(next_body->offset);
    if (token != DENTRY_LIST_CACHE.token) {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "invalid token for next list");
        return EINVAL;
    }
    if (offset != DENTRY_LIST_CACHE.offset) {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "next list offset: %d != expected: %d",
                offset, DENTRY_LIST_CACHE.offset);
        return EINVAL;
    }
    return server_list_dentry_output(task);
}

static inline void init_task_context(struct fast_task_info *task)
{
    TASK_ARG->req_start_time = get_current_time_us();
    RESPONSE.header.cmd = FDIR_PROTO_ACK;
    RESPONSE.header.body_len = 0;
    RESPONSE.header.status = 0;
    RESPONSE.error.length = 0;
    RESPONSE.error.message[0] = '\0';
    TASK_ARG->context.log_error = true;
    TASK_ARG->context.response_done = false;

    REQUEST.header.cmd = ((FDIRProtoHeader *)task->data)->cmd;
    REQUEST.header.body_len = task->length - sizeof(FDIRProtoHeader);
    REQUEST.body = task->data + sizeof(FDIRProtoHeader);
}

static inline int service_check_master(struct fast_task_info *task)
{
    if (!MYSELF_IS_MASTER) {
        RESPONSE.error.length = sprintf(
                RESPONSE.error.message,
                "i am not master");
        return EINVAL;
    }

    return 0;
}

static int deal_task_done(struct fast_task_info *task)
{
    FDIRProtoHeader *proto_header;
    int r;
    int time_used;

    if (TASK_ARG->context.log_error && RESPONSE.error.length > 0) {
        logError("file: "__FILE__", line: %d, "
                "client ip: %s, cmd: %d, req body length: %d, %s",
                __LINE__, task->client_ip, REQUEST.header.cmd,
                REQUEST.header.body_len,
                RESPONSE.error.message);
    }

    proto_header = (FDIRProtoHeader *)task->data;
    if (!TASK_ARG->context.response_done) {
        RESPONSE.header.body_len = RESPONSE.error.length;
        if (RESPONSE.error.length > 0) {
            memcpy(task->data + sizeof(FDIRProtoHeader),
                    RESPONSE.error.message, RESPONSE.error.length);
        }
    }

    short2buff(RESPONSE_STATUS >= 0 ? RESPONSE_STATUS : -1 * RESPONSE_STATUS,
            proto_header->status);
    proto_header->cmd = RESPONSE.header.cmd;
    int2buff(RESPONSE.header.body_len, proto_header->body_len);
    task->length = sizeof(FDIRProtoHeader) + RESPONSE.header.body_len;

    r = sf_send_add_event(task);
    time_used = (int)(get_current_time_us() - TASK_ARG->req_start_time);
    if (time_used > 50 * 1000) {
        lwarning("process a request timed used: %d us, "
                "cmd: %d, req body len: %d, resp body len: %d",
                time_used, REQUEST.header.cmd,
                REQUEST.header.body_len,
                RESPONSE.header.body_len);
    }

    if (REQUEST.header.cmd != FDIR_CLUSTER_PROTO_PING_MASTER_REQ) {
    logInfo("file: "__FILE__", line: %d, "
            "client ip: %s, req cmd: %d, req body_len: %d, "
            "resp cmd: %d, status: %d, resp body_len: %d, "
            "time used: %d us", __LINE__,
            task->client_ip, REQUEST.header.cmd,
            REQUEST.header.body_len, RESPONSE.header.cmd,
            RESPONSE_STATUS, RESPONSE.header.body_len, time_used);
    }

    return r == 0 ? RESPONSE_STATUS : r;
}

int server_deal_task(struct fast_task_info *task)
{
    int result;

    /*
    logInfo("file: "__FILE__", line: %d, "
            "nio_stage: %d, SF_NIO_STAGE_CONTINUE: %d", __LINE__,
            task->nio_stage, SF_NIO_STAGE_CONTINUE);
            */

    if (task->nio_stage == SF_NIO_STAGE_CONTINUE) {
        task->nio_stage = SF_NIO_STAGE_SEND;
        if (TASK_ARG->context.deal_func != NULL) {
            result = TASK_ARG->context.deal_func(task);
        } else {
            result = RESPONSE_STATUS;
            if (result == TASK_STATUS_CONTINUE) {
                logError("file: "__FILE__", line: %d, "
                        "unexpect status: %d", __LINE__, result);
                result = EBUSY;
            }
        }
    } else {
        init_task_context(task);

        switch (REQUEST.header.cmd) {
            case FDIR_PROTO_ACTIVE_TEST_REQ:
                RESPONSE.header.cmd = FDIR_PROTO_ACTIVE_TEST_RESP;
                result = server_deal_actvie_test(task);
                break;
            case FDIR_SERVICE_PROTO_CREATE_DENTRY:
                if ((result=service_check_master(task)) == 0) {
                    result = server_deal_create_dentry(task);
                }
                break;
            case FDIR_SERVICE_PROTO_REMOVE_DENTRY:
                if ((result=service_check_master(task)) == 0) {
                    result = server_deal_remove_dentry(task);
                }
                break;
            case FDIR_SERVICE_PROTO_LIST_DENTRY_FIRST_REQ:
                result = server_deal_list_dentry_first(task);
                break;
            case FDIR_SERVICE_PROTO_LIST_DENTRY_NEXT_REQ:
                result = server_deal_list_dentry_next(task);
                break;
            default:
                RESPONSE.error.length = sprintf(
                        RESPONSE.error.message,
                        "unkown cmd: %d", REQUEST.header.cmd);
                result = -EINVAL;
                break;
        }
    }

    if (result == TASK_STATUS_CONTINUE) {
        return 0;
    } else {
        RESPONSE_STATUS = result;
        return deal_task_done(task);
    }
}

void *server_alloc_thread_extra_data(const int thread_index)
{
    FDIRServerContext *server_context;

    server_context = (FDIRServerContext *)malloc(sizeof(FDIRServerContext));
    if (server_context == NULL) {
        logError("file: "__FILE__", line: %d, "
                "malloc %d bytes fail, errno: %d, error info: %s",
                __LINE__, (int)sizeof(FDIRServerContext),
                errno, strerror(errno));
        return NULL;
    }

    memset(server_context, 0, sizeof(FDIRServerContext));
    if (fast_mblock_init_ex(&server_context->service.record_allocator,
                sizeof(FDIRBinlogRecord), 4 * 1024, NULL, NULL, false) != 0)
    {
        free(server_context);
        return NULL;
    }

    return server_context;
}
