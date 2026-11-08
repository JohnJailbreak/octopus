/* oct_link.c - the transport adapter. See oct_link.h. */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#elif !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "oct_link.h"
#include "oct_log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_CONTAINER ((uint64_t)OCT_PTP_HEADER_SIZE + OCT_PTP_MAX_TRANSFER)

struct oct_link {
    pthread_mutex_t   lock;
    oct_link_driver   driver;
    oct_transport_ops transport_ops;
    uint32_t          max_packet;
    uint32_t          timeout_ms;
    bool              rehearsal;
    char              component[OCT_LOG_NAME_MAX + 1];
    oct_service      *service;
    bool              running;
    oct_buf           pending;          /* the container still arriving */
    uint64_t          discard;          /* bytes of an oversized container still to drop */
    int64_t           last_input_ms;
    bool              busy;             /* a container is with the responder */
    bool              cancelled;        /* until the next command, Get Device Status says so */
    bool              awaiting_data;    /* the command in progress waits for its data phase */
    bool              warned_awaiting;
    uint16_t          command;          /* the transaction in progress */
    uint32_t          command_tid;
    uint32_t          command_param;
    int64_t           command_started_ms;
    int64_t           command_started_us;
    uint64_t          command_bytes_in;
    oct_link_status   status;
};

/* ---- Helpers --------------------------------------------------------------------- */

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int64_t monotonic_us(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (int64_t)now.tv_sec * 1000000 + (int64_t)(now.tv_nsec / 1000);
}

int64_t oct_link_now_ms(void) { return monotonic_us() / 1000; }

static const char *operation_name(uint16_t code)
{
    switch (code) {
    case OCT_OP_GET_DEVICE_INFO: return "GetDeviceInfo";
    case OCT_OP_OPEN_SESSION: return "OpenSession";
    case OCT_OP_CLOSE_SESSION: return "CloseSession";
    case OCT_OP_GET_STORAGE_IDS: return "GetStorageIDs";
    case OCT_OP_GET_STORAGE_INFO: return "GetStorageInfo";
    case OCT_OP_GET_NUM_OBJECTS: return "GetNumObjects";
    case OCT_OP_GET_OBJECT_HANDLES: return "GetObjectHandles";
    case OCT_OP_GET_OBJECT_INFO: return "GetObjectInfo";
    case OCT_OP_GET_OBJECT: return "GetObject";
    case OCT_OP_GET_THUMB: return "GetThumb";
    case OCT_OP_DELETE_OBJECT: return "DeleteObject";
    case OCT_OP_SEND_OBJECT_INFO: return "SendObjectInfo";
    case OCT_OP_SEND_OBJECT: return "SendObject";
    case OCT_OP_GET_DEVICE_PROP_DESC: return "GetDevicePropDesc";
    case OCT_OP_GET_DEVICE_PROP_VALUE: return "GetDevicePropValue";
    case OCT_OP_MOVE_OBJECT: return "MoveObject";
    case OCT_OP_GET_PARTIAL_OBJECT: return "GetPartialObject";
    case OCT_OP_GET_OBJECT_PROPS_SUPPORTED: return "GetObjectPropsSupported";
    case OCT_OP_GET_OBJECT_PROP_DESC: return "GetObjectPropDesc";
    case OCT_OP_GET_OBJECT_PROP_VALUE: return "GetObjectPropValue";
    case OCT_OP_SET_OBJECT_PROP_VALUE: return "SetObjectPropValue";
    case OCT_OP_GET_OBJECT_PROP_LIST: return "GetObjectPropList";
    }
    return "Unknown";
}

static const char *response_name(uint16_t code)
{
    switch (code) {
    case OCT_RC_OK: return "OK";
    case OCT_RC_GENERAL_ERROR: return "GeneralError";
    case OCT_RC_SESSION_NOT_OPEN: return "SessionNotOpen";
    case OCT_RC_INVALID_TRANSACTION_ID: return "InvalidTransactionID";
    case OCT_RC_OPERATION_NOT_SUPPORTED: return "OperationNotSupported";
    case OCT_RC_INCOMPLETE_TRANSFER: return "IncompleteTransfer";
    case OCT_RC_INVALID_STORAGE_ID: return "InvalidStorageID";
    case OCT_RC_INVALID_OBJECT_HANDLE: return "InvalidObjectHandle";
    case OCT_RC_STORE_READ_ONLY: return "StoreReadOnly";
    case OCT_RC_ACCESS_DENIED: return "AccessDenied";
    case OCT_RC_INVALID_PARAMETER: return "InvalidParameter";
    case OCT_RC_SESSION_ALREADY_OPEN: return "SessionAlreadyOpen";
    case OCT_RC_DEVICE_BUSY: return "DeviceBusy";
    case OCT_RC_TRANSACTION_CANCELLED: return "TransactionCancelled";
    }
    return "Other";
}

/* A rehearsal (the self-test) logs everything as DEBUG, so its deliberate faults don't read as real ones. */
static oct_log_level level_for(const oct_link *l, oct_log_level level)
{
    return l->rehearsal ? OCT_LOG_DEBUG : level;
}

static void set_state_locked(oct_link *l, oct_transport_state to)
{
    if (l->status.state == to) return;
    l->status.state = to;
    oct_logf(level_for(l, to == OCT_TRANSPORT_ERROR ? OCT_LOG_ERROR : OCT_LOG_INFO), l->component, "state", "%s",
             oct_transport_state_name(to));
}

/* Records the last meaningful failure and logs it. */
static void failure_locked(oct_link *l, oct_log_level level, const char *event, const char *format, ...)
    OCT_LOG_PRINTF(4, 5);
static void failure_locked(oct_link *l, oct_log_level level, const char *event, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(l->status.last_failure, sizeof l->status.last_failure, format, args);
    va_end(args);
    oct_logf(level_for(l, level), l->component, event, "%s", l->status.last_failure);
}

static void reset_input_locked(oct_link *l)
{
    oct_buf_clear(&l->pending);
    l->discard = 0;
    l->cancelled = false;
    l->awaiting_data = false;
    l->warned_awaiting = false;
}

static void connected_state_locked(oct_link *l)
{
    if (l->status.host_attached) {
        set_state_locked(l, l->status.session_open ? OCT_TRANSPORT_SESSION_ACTIVE : OCT_TRANSPORT_HOST_CONNECTED);
    }
}

/* Drops a container that stopped arriving, and warns once about a data phase that never came. */
static void expire_locked(oct_link *l, int64_t now)
{
    if ((l->pending.length > 0 || l->discard > 0) && now - l->last_input_ms >= (int64_t)l->timeout_ms) {
        l->status.timeouts++;
        failure_locked(l, OCT_LOG_WARN, "timeout", "Incomplete container dropped after %lld ms: %zu bytes received%s",
                       (long long)(now - l->last_input_ms), l->pending.length,
                       l->discard ? " of an oversized container" : "");
        oct_buf_clear(&l->pending);
        l->discard = 0;
    }
    if (l->awaiting_data && !l->warned_awaiting && now - l->command_started_ms >= (int64_t)l->timeout_ms) {
        l->warned_awaiting = true;
        failure_locked(l, OCT_LOG_WARN, "no_data", "%s (tid %u) has waited %lld ms for its data phase",
                       operation_name(l->command), l->command_tid, (long long)(now - l->command_started_ms));
    }
}

/* ---- The transport the service sees ---------------------------------------------- */

static oct_status link_start(void *context, oct_service *service)
{
    oct_link *l = context;
    pthread_mutex_lock(&l->lock);
    l->service = service;
    reset_input_locked(l);
    l->status.host_attached = false;
    l->status.session_open = false;
    set_state_locked(l, OCT_TRANSPORT_INITIALIZING);
    pthread_mutex_unlock(&l->lock);
    oct_status status = l->driver.ops->start ? l->driver.ops->start(l->driver.context, l) : OCT_OK;
    pthread_mutex_lock(&l->lock);
    l->running = status == OCT_OK;
    if (status != OCT_OK) {
        failure_locked(l, OCT_LOG_ERROR, "start_failed", "The %s driver didn't start: %s", l->status.driver,
                       oct_status_name(status));
        set_state_locked(l, OCT_TRANSPORT_ERROR);
    } else {
        set_state_locked(l, OCT_TRANSPORT_WAITING);
    }
    pthread_mutex_unlock(&l->lock);
    return status;
}

static void link_stop(void *context)
{
    oct_link *l = context;
    if (l->driver.ops->stop) l->driver.ops->stop(l->driver.context);
    pthread_mutex_lock(&l->lock);
    l->running = false;
    l->status.host_attached = false;
    l->status.session_open = false;
    reset_input_locked(l);
    set_state_locked(l, OCT_TRANSPORT_UNAVAILABLE);
    pthread_mutex_unlock(&l->lock);
}

static void link_status(void *context, oct_transport_status *out)
{
    oct_link *l = context;
    pthread_mutex_lock(&l->lock);
    out->state = l->status.state;
    out->transactions = l->status.transactions;
    oct_copy_utf8(out->failure, sizeof out->failure, l->status.last_failure);
    pthread_mutex_unlock(&l->lock);
}

oct_status oct_link_create(const oct_link_driver *driver, const oct_link_config *config, oct_link **out)
{
    if (!out) return OCT_E_INVALID_ARGUMENT;
    *out = NULL;
    if (!driver || !driver->ops || !driver->ops->send) return OCT_E_INVALID_ARGUMENT;
    oct_link *l = calloc(1, sizeof *l);
    if (!l) return OCT_E_NO_MEMORY;
    if (pthread_mutex_init(&l->lock, NULL) != 0) {
        free(l);
        return OCT_E_NO_MEMORY;
    }
    l->driver = *driver;
    l->max_packet = config && config->max_packet ? config->max_packet : OCT_LINK_DEFAULT_PACKET;
    l->timeout_ms = config && config->timeout_ms ? config->timeout_ms : OCT_LINK_DEFAULT_TIMEOUT;
    l->rehearsal = config && config->rehearsal;
    oct_copy_utf8(l->component, sizeof l->component, config && config->log_component ? config->log_component : "link");
    oct_copy_utf8(l->status.driver, sizeof l->status.driver, driver->ops->name ? driver->ops->name : "driver");
    oct_buf_init(&l->pending, (size_t)MAX_CONTAINER);
    l->transport_ops.name = l->status.driver;
    l->transport_ops.start = link_start;
    l->transport_ops.stop = link_stop;
    l->transport_ops.status = link_status;
    l->status.state = OCT_TRANSPORT_UNAVAILABLE;
    l->status.device_status = OCT_RC_OK;
    *out = l;
    return OCT_OK;
}

void oct_link_destroy(oct_link *l)
{
    if (!l) return;
    oct_buf_free(&l->pending);
    pthread_mutex_destroy(&l->lock);
    free(l);
}

void oct_link_transport(oct_link *l, oct_transport *out)
{
    if (!out) return;
    out->ops = l ? &l->transport_ops : NULL;
    out->context = l;
}

void oct_link_get_status(oct_link *l, oct_link_status *out)
{
    if (!l || !out) return;
    pthread_mutex_lock(&l->lock);
    *out = l->status;
    out->device_status = l->busy ? OCT_RC_DEVICE_BUSY : l->cancelled ? OCT_RC_TRANSACTION_CANCELLED : OCT_RC_OK;
    pthread_mutex_unlock(&l->lock);
}

/* ---- Host connection and class requests ------------------------------------------- */

oct_status oct_link_host_attached(oct_link *l)
{
    if (!l) return OCT_E_INVALID_ARGUMENT;
    pthread_mutex_lock(&l->lock);
    if (!l->running) {
        pthread_mutex_unlock(&l->lock);
        return OCT_E_STATE;
    }
    if (l->status.host_attached) {
        pthread_mutex_unlock(&l->lock);
        return OCT_OK;
    }
    oct_service *service = l->service;
    reset_input_locked(l);
    pthread_mutex_unlock(&l->lock);
    oct_status status = oct_service_host_attached(service);
    pthread_mutex_lock(&l->lock);
    if (status == OCT_OK) {
        l->status.host_attached = true;
        l->status.session_open = false;
        l->status.connects++;
        oct_logf(level_for(l, OCT_LOG_INFO), l->component, "attach", "Host attached (connection %llu)",
                 (unsigned long long)l->status.connects);
        connected_state_locked(l);
    } else {
        failure_locked(l, OCT_LOG_ERROR, "attach_failed", "The service refused the host: %s", oct_status_name(status));
    }
    pthread_mutex_unlock(&l->lock);
    return status;
}

oct_status oct_link_host_detached(oct_link *l, const char *reason)
{
    if (!l) return OCT_E_INVALID_ARGUMENT;
    pthread_mutex_lock(&l->lock);
    if (!l->status.host_attached) {
        pthread_mutex_unlock(&l->lock);
        return OCT_E_STATE;
    }
    bool interrupted = l->pending.length > 0 || l->discard > 0 || l->awaiting_data;
    size_t partial = l->pending.length;
    l->status.host_attached = false;
    l->status.session_open = false;
    l->status.disconnects++;
    reset_input_locked(l);
    oct_service *service = l->service;
    pthread_mutex_unlock(&l->lock);
    oct_service_host_detached(service);
    pthread_mutex_lock(&l->lock);
    if (interrupted) {
        failure_locked(l, OCT_LOG_WARN, "interrupted", "Host detached mid-transfer (%s); %zu buffered bytes dropped",
                       reason ? reason : "no reason given", partial);
    } else {
        oct_logf(level_for(l, OCT_LOG_INFO), l->component, "detach", "Host detached: %s",
                 reason ? reason : "no reason given");
    }
    if (l->running) set_state_locked(l, OCT_TRANSPORT_DISCONNECTED);
    pthread_mutex_unlock(&l->lock);
    return OCT_OK;
}

oct_status oct_link_cancel(oct_link *l, uint32_t transaction_id)
{
    if (!l) return OCT_E_INVALID_ARGUMENT;
    pthread_mutex_lock(&l->lock);
    if (!l->status.host_attached) {
        pthread_mutex_unlock(&l->lock);
        return OCT_E_STATE;
    }
    size_t partial = l->pending.length;
    oct_buf_clear(&l->pending);
    l->discard = 0;
    l->awaiting_data = false;
    l->cancelled = true;
    l->status.cancels++;
    oct_logf(level_for(l, OCT_LOG_INFO), l->component, "cancel", "Host cancelled transaction %u; %zu buffered bytes dropped",
             transaction_id, partial);
    pthread_mutex_unlock(&l->lock);
    return OCT_OK;
}

uint16_t oct_link_device_status(oct_link *l)
{
    if (!l) return OCT_RC_GENERAL_ERROR;
    pthread_mutex_lock(&l->lock);
    uint16_t code = l->busy ? OCT_RC_DEVICE_BUSY : l->cancelled ? OCT_RC_TRANSACTION_CANCELLED : OCT_RC_OK;
    l->status.device_status = code;
    pthread_mutex_unlock(&l->lock);
    return code;
}

oct_status oct_link_device_reset(oct_link *l)
{
    if (!l) return OCT_E_INVALID_ARGUMENT;
    pthread_mutex_lock(&l->lock);
    if (!l->status.host_attached) {
        pthread_mutex_unlock(&l->lock);
        return OCT_E_STATE;
    }
    reset_input_locked(l);
    l->status.session_open = false;
    l->status.resets++;
    oct_service *service = l->service;
    pthread_mutex_unlock(&l->lock);
    oct_status status = oct_service_reset_session(service);
    pthread_mutex_lock(&l->lock);
    oct_logf(level_for(l, OCT_LOG_INFO), l->component, "reset", "Device reset by the host; session closed");
    connected_state_locked(l);
    pthread_mutex_unlock(&l->lock);
    return status;
}

void oct_link_fail(oct_link *l, const char *reason)
{
    if (!l) return;
    pthread_mutex_lock(&l->lock);
    failure_locked(l, OCT_LOG_ERROR, "driver_failed", "%s", reason ? reason : "The driver failed");
    bool attached = l->status.host_attached;
    l->status.host_attached = false;
    l->status.session_open = false;
    reset_input_locked(l);
    set_state_locked(l, OCT_TRANSPORT_ERROR);
    oct_service *service = l->service;
    pthread_mutex_unlock(&l->lock);
    if (attached) oct_service_host_detached(service);
}

oct_status oct_link_tick(oct_link *l, int64_t now_ms)
{
    if (!l) return OCT_E_INVALID_ARGUMENT;
    pthread_mutex_lock(&l->lock);
    if (l->running) expire_locked(l, now_ms);
    pthread_mutex_unlock(&l->lock);
    return OCT_OK;
}

/* ---- Containers in and out -------------------------------------------------------- */

/* Hands each container the responder produced to the driver as its own transfer. */
static void send_output(oct_link *l, const oct_buf *out)
{
    size_t offset = 0;
    while (offset + OCT_PTP_HEADER_SIZE <= out->length) {
        uint32_t size = le32(out->data + offset);
        if (size < OCT_PTP_HEADER_SIZE || size > out->length - offset) break;
        oct_status status = l->driver.ops->send(l->driver.context, OCT_LINK_BULK_IN, out->data + offset, size,
                                                size % l->max_packet == 0);
        pthread_mutex_lock(&l->lock);
        if (status == OCT_OK) {
            l->status.bytes_out += size;
        } else {
            l->status.send_failures++;
            failure_locked(l, OCT_LOG_ERROR, "send_failed", "Sending a %u-byte container failed: %s", size,
                           oct_status_name(status));
        }
        pthread_mutex_unlock(&l->lock);
        offset += size;
    }
}

static void process_container(oct_link *l, const uint8_t *bytes, uint32_t size, int64_t now)
{
    uint16_t type = le16(bytes + 4), code = le16(bytes + 6);
    uint32_t tid = le32(bytes + 8);
    pthread_mutex_lock(&l->lock);
    oct_service *service = l->service;
    l->status.containers_in++;
    l->busy = true;
    if (type == OCT_CONTAINER_COMMAND) {
        if (l->awaiting_data) {
            failure_locked(l, OCT_LOG_WARN, "abandoned", "%s (tid %u) never got its data phase",
                           operation_name(l->command), l->command_tid);
        }
        l->cancelled = false;
        l->awaiting_data = false;
        l->warned_awaiting = false;
        l->command = code;
        l->command_tid = tid;
        l->command_param = size >= OCT_PTP_HEADER_SIZE + 4u ? le32(bytes + OCT_PTP_HEADER_SIZE) : 0;
        l->command_started_ms = now;
        l->command_started_us = monotonic_us();
        l->command_bytes_in = 0;
    }
    l->command_bytes_in += size;
    pthread_mutex_unlock(&l->lock);

    oct_buf out;
    oct_buf_init(&out, 0);
    oct_status status = service ? oct_service_receive(service, bytes, size, &out) : OCT_E_STATE;
    bool responded = false;
    uint16_t response = 0;
    for (size_t offset = 0; offset + OCT_PTP_HEADER_SIZE <= out.length;) {
        uint32_t length = le32(out.data + offset);
        if (length < OCT_PTP_HEADER_SIZE) break;
        if (le16(out.data + offset + 4) == OCT_CONTAINER_RESPONSE) {
            responded = true;
            response = le16(out.data + offset + 6);
        }
        offset += length;
    }

    pthread_mutex_lock(&l->lock);
    l->busy = false;
    if (status == OCT_E_MALFORMED) {
        l->status.malformed++;
        failure_locked(l, OCT_LOG_WARN, "malformed", "The responder rejected a %u-byte container (type %u, code 0x%04X)%s",
                       size, type, code, responded ? "; answered GeneralError" : "");
    } else if (status == OCT_E_STATE) {
        l->status.malformed++;
        failure_locked(l, OCT_LOG_WARN, "unexpected",
                       type == OCT_CONTAINER_DATA ? "A data container (tid %u) arrived with no command waiting for it"
                                                  : "The service isn't accepting containers (tid %u)", tid);
    } else if (status != OCT_OK) {
        failure_locked(l, OCT_LOG_ERROR, "responder_failed", "The responder failed: %s", oct_status_name(status));
    }
    if (type == OCT_CONTAINER_COMMAND && status == OCT_OK && !responded) l->awaiting_data = true;
    uint16_t operation = l->command;
    uint32_t transaction = l->command_tid;
    uint64_t bytes_in = l->command_bytes_in;
    if (responded) {
        l->awaiting_data = false;
        l->status.transactions++;
        l->status.last_operation = operation;
        l->status.last_transaction = transaction;
        l->status.last_response = response;
        if (operation == OCT_OP_OPEN_SESSION && response == OCT_RC_OK) {
            l->status.session_open = true;
            l->status.session_id = l->command_param;
        } else if (operation == OCT_OP_CLOSE_SESSION && response == OCT_RC_OK) {
            l->status.session_open = false;
        } else if (response == OCT_RC_SESSION_NOT_OPEN) {
            l->status.session_open = false;
        }
        connected_state_locked(l);
    }
    pthread_mutex_unlock(&l->lock);

    send_output(l, &out);
    if (responded) {
        int64_t elapsed = monotonic_us();
        pthread_mutex_lock(&l->lock);
        elapsed -= l->command_started_us;
        l->status.last_duration_us = elapsed > 0 ? (uint64_t)elapsed : 0;
        pthread_mutex_unlock(&l->lock);
        /* Successful exchanges are DEBUG; refusals are INFO, so what a host asks for and doesn't get shows by
         * default. Names and contents of objects are never logged. */
        oct_logf(level_for(l, response == OCT_RC_OK ? OCT_LOG_DEBUG : OCT_LOG_INFO), l->component, "transaction",
                 "op=0x%04X %s tid=%u rc=0x%04X %s in=%llu out=%zu time=%.2fms", operation, operation_name(operation),
                 transaction, response, response_name(response), (unsigned long long)bytes_in, out.length,
                 (double)(elapsed > 0 ? elapsed : 0) / 1000.0);
    }
    oct_buf_free(&out);
}

oct_status oct_link_input(oct_link *l, const uint8_t *bytes, size_t length, int64_t now_ms)
{
    if (!l || (!bytes && length)) return OCT_E_INVALID_ARGUMENT;
    oct_buf ready; /* complete containers, back to back, in arrival order */
    oct_buf_init(&ready, 0);
    pthread_mutex_lock(&l->lock);
    if (!l->running || !l->status.host_attached) {
        failure_locked(l, OCT_LOG_WARN, "input_ignored", "%zu bytes arrived with no host attached", length);
        pthread_mutex_unlock(&l->lock);
        oct_buf_free(&ready);
        return OCT_E_STATE;
    }
    expire_locked(l, now_ms);
    l->last_input_ms = now_ms;
    l->status.bytes_in += length;
    oct_status result = OCT_OK;
    while (length > 0) {
        if (l->discard > 0) {
            size_t drop = (uint64_t)length < l->discard ? length : (size_t)l->discard;
            l->discard -= drop;
            bytes += drop;
            length -= drop;
            continue;
        }
        size_t have = l->pending.length;
        size_t want = have < OCT_PTP_HEADER_SIZE ? OCT_PTP_HEADER_SIZE - have : le32(l->pending.data) - have;
        size_t take = length < want ? length : want;
        oct_buf_bytes(&l->pending, bytes, take);
        if (l->pending.failed) {
            failure_locked(l, OCT_LOG_ERROR, "no_memory", "No memory for a %zu-byte container", have + take);
            reset_input_locked(l);
            result = OCT_E_NO_MEMORY;
            break;
        }
        bytes += take;
        length -= take;
        if (have < OCT_PTP_HEADER_SIZE && l->pending.length == OCT_PTP_HEADER_SIZE) {
            uint32_t declared = le32(l->pending.data);
            if (declared < OCT_PTP_HEADER_SIZE) {
                l->status.malformed++;
                failure_locked(l, OCT_LOG_WARN, "malformed",
                               "A container declares %u bytes, less than its header; %zu bytes of input dropped",
                               declared, OCT_PTP_HEADER_SIZE + length);
                oct_buf_clear(&l->pending);
                length = 0; /* framing is lost for the rest of this input */
                result = OCT_E_MALFORMED;
                break;
            }
            if (declared > MAX_CONTAINER) {
                l->status.malformed++;
                failure_locked(l, OCT_LOG_WARN, "oversized",
                               "A %u-byte container is larger than the %u-byte limit and is dropped", declared,
                               (unsigned)MAX_CONTAINER);
                l->discard = declared - OCT_PTP_HEADER_SIZE;
                oct_buf_clear(&l->pending);
                result = OCT_E_TOO_LARGE;
                continue;
            }
        }
        if (l->pending.length >= OCT_PTP_HEADER_SIZE && l->pending.length == le32(l->pending.data)) {
            if (ready.length == 0) {
                oct_buf swap = ready; /* hand over the buffer rather than copy a large data phase */
                ready = l->pending;
                l->pending = swap;
                l->pending.limit = (size_t)MAX_CONTAINER;
                ready.limit = OCT_BUF_DEFAULT_LIMIT; /* room for more containers behind a large one */
            } else {
                oct_buf_bytes(&ready, l->pending.data, l->pending.length);
                oct_buf_clear(&l->pending);
            }
        }
    }
    pthread_mutex_unlock(&l->lock);
    if (ready.failed) {
        pthread_mutex_lock(&l->lock);
        failure_locked(l, OCT_LOG_ERROR, "no_memory", "No memory to queue received containers; they were dropped");
        pthread_mutex_unlock(&l->lock);
        result = OCT_E_NO_MEMORY;
    }
    for (size_t offset = 0; !ready.failed && offset + OCT_PTP_HEADER_SIZE <= ready.length;) {
        uint32_t size = le32(ready.data + offset);
        process_container(l, ready.data + offset, size, now_ms);
        offset += size;
    }
    oct_buf_free(&ready);
    return result;
}

/* ---- Loopback driver -------------------------------------------------------------- */

typedef struct loopback_transfer {
    oct_link_pipe pipe;
    bool          zero_length_packet;
    uint8_t      *bytes;
    size_t        length;
} loopback_transfer;

struct oct_loopback {
    pthread_mutex_t    lock;
    loopback_transfer *queue;
    size_t             head, count, capacity;
    bool               fail_start, fail_send;
};

static oct_status loopback_start(void *context, oct_link *link)
{
    (void)link;
    oct_loopback *lb = context;
    pthread_mutex_lock(&lb->lock);
    bool fail = lb->fail_start;
    pthread_mutex_unlock(&lb->lock);
    return fail ? OCT_E_IO : OCT_OK;
}

static void loopback_stop(void *context) { (void)context; }

static oct_status loopback_send(void *context, oct_link_pipe pipe, const uint8_t *bytes, size_t length, bool zlp)
{
    oct_loopback *lb = context;
    pthread_mutex_lock(&lb->lock);
    oct_status status = OCT_OK;
    if (lb->fail_send) {
        status = OCT_E_IO;
    } else {
        if (lb->head + lb->count == lb->capacity) {
            if (lb->head > 0) {
                memmove(lb->queue, lb->queue + lb->head, lb->count * sizeof *lb->queue);
                lb->head = 0;
            } else {
                size_t capacity = lb->capacity ? lb->capacity * 2 : 16;
                loopback_transfer *grown = realloc(lb->queue, capacity * sizeof *grown);
                if (grown) {
                    lb->queue = grown;
                    lb->capacity = capacity;
                }
            }
        }
        uint8_t *copy = length ? malloc(length) : NULL;
        if (lb->head + lb->count == lb->capacity || (length && !copy)) {
            free(copy);
            status = OCT_E_NO_MEMORY;
        } else {
            if (length) memcpy(copy, bytes, length);
            lb->queue[lb->head + lb->count++] = (loopback_transfer){ pipe, zlp, copy, length };
        }
    }
    pthread_mutex_unlock(&lb->lock);
    return status;
}

static const oct_link_driver_ops loopback_ops = { "Loopback", loopback_start, loopback_stop, loopback_send };

oct_status oct_loopback_create(oct_loopback **out)
{
    if (!out) return OCT_E_INVALID_ARGUMENT;
    *out = calloc(1, sizeof **out);
    if (!*out) return OCT_E_NO_MEMORY;
    if (pthread_mutex_init(&(*out)->lock, NULL) != 0) {
        free(*out);
        *out = NULL;
        return OCT_E_NO_MEMORY;
    }
    return OCT_OK;
}

void oct_loopback_destroy(oct_loopback *lb)
{
    if (!lb) return;
    for (size_t i = 0; i < lb->count; i++) free(lb->queue[lb->head + i].bytes);
    free(lb->queue);
    pthread_mutex_destroy(&lb->lock);
    free(lb);
}

void oct_loopback_driver(oct_loopback *lb, oct_link_driver *out)
{
    if (!out) return;
    out->ops = &loopback_ops;
    out->context = lb;
}

void oct_loopback_set_failures(oct_loopback *lb, bool fail_start, bool fail_send)
{
    if (!lb) return;
    pthread_mutex_lock(&lb->lock);
    lb->fail_start = fail_start;
    lb->fail_send = fail_send;
    pthread_mutex_unlock(&lb->lock);
}

size_t oct_loopback_pending(oct_loopback *lb)
{
    if (!lb) return 0;
    pthread_mutex_lock(&lb->lock);
    size_t count = lb->count;
    pthread_mutex_unlock(&lb->lock);
    return count;
}

bool oct_loopback_next(oct_loopback *lb, oct_link_pipe *pipe, oct_buf *out, bool *zlp)
{
    if (!lb || !out) return false;
    pthread_mutex_lock(&lb->lock);
    bool found = lb->count > 0;
    if (found) {
        loopback_transfer transfer = lb->queue[lb->head++];
        if (--lb->count == 0) lb->head = 0;
        oct_buf_clear(out);
        oct_buf_bytes(out, transfer.bytes, transfer.length);
        if (pipe) *pipe = transfer.pipe;
        if (zlp) *zlp = transfer.zero_length_packet;
        free(transfer.bytes);
    }
    pthread_mutex_unlock(&lb->lock);
    return found;
}

/* ---- Test host ----------------------------------------------------------------------- */

static void feed(oct_link *link, const uint8_t *bytes, size_t length, size_t chunk, int64_t now)
{
    if (chunk == 0) chunk = length ? length : 1;
    for (size_t offset = 0; offset < length; offset += chunk) {
        oct_link_input(link, bytes + offset, length - offset < chunk ? length - offset : chunk, now);
    }
}

oct_status oct_link_transact(oct_link *link, oct_loopback *loopback, uint16_t operation, uint32_t transaction_id,
                             const uint32_t *params, uint32_t param_count, const void *data, size_t data_length,
                             size_t chunk, int64_t now_ms, oct_link_reply *reply)
{
    if (!link || !loopback || !reply) return OCT_E_INVALID_ARGUMENT;
    if (reply->data.limit == 0) oct_buf_init(&reply->data, 0);
    oct_buf_clear(&reply->data);
    reply->response = 0;
    reply->param_count = 0;
    reply->has_data = false;
    reply->transfers = 0;
    uint32_t packet = link->max_packet; /* fixed when the link is created */

    oct_buf message;
    oct_buf_init(&message, 0);
    oct_container_command(&message, operation, transaction_id, params, param_count);
    feed(link, message.data, message.length, chunk, now_ms);
    if (data) {
        oct_buf_clear(&message);
        size_t start = oct_container_data_begin(&message, operation, transaction_id);
        oct_buf_bytes(&message, data, data_length);
        oct_container_data_end(&message, start);
        feed(link, message.data, message.length, chunk, now_ms);
    }

    oct_status result = OCT_E_STATE; /* no response yet */
    oct_link_pipe pipe;
    bool zlp = false;
    while (oct_loopback_next(loopback, &pipe, &message, &zlp)) {
        reply->transfers++;
        if (result != OCT_E_STATE || pipe != OCT_LINK_BULK_IN || message.length < OCT_PTP_HEADER_SIZE ||
            le32(message.data) != message.length || zlp != (message.length % packet == 0) ||
            le32(message.data + 8) != transaction_id) {
            result = OCT_E_MALFORMED; /* more than one response, or a transfer that isn't one whole container */
            continue;
        }
        uint16_t type = le16(message.data + 4);
        if (type == OCT_CONTAINER_DATA && !reply->has_data && le16(message.data + 6) == operation) {
            reply->has_data = true;
            oct_buf_bytes(&reply->data, message.data + OCT_PTP_HEADER_SIZE, message.length - OCT_PTP_HEADER_SIZE);
        } else if (type == OCT_CONTAINER_RESPONSE && (message.length - OCT_PTP_HEADER_SIZE) % 4 == 0 &&
                   message.length <= OCT_PTP_MAX_COMMAND) {
            reply->response = le16(message.data + 6);
            reply->param_count = (uint32_t)((message.length - OCT_PTP_HEADER_SIZE) / 4);
            for (uint32_t i = 0; i < reply->param_count; i++) {
                reply->params[i] = le32(message.data + OCT_PTP_HEADER_SIZE + 4 * i);
            }
            result = OCT_OK;
        } else {
            result = OCT_E_MALFORMED;
        }
    }
    oct_buf_free(&message);
    return result;
}

/* ---- Self-test ----------------------------------------------------------------------- */

typedef struct link_test {
    oct_link      *link;
    oct_loopback  *loopback;
    oct_link_reply reply;
    uint32_t       checks;
    uint32_t       tid;
    int64_t        now;
    char          *failure;
    size_t         capacity;
} link_test;

#define EXPECT(t, condition, ...)                                            \
    do {                                                                     \
        (t)->checks++;                                                       \
        if (!(condition)) {                                                  \
            snprintf((t)->failure, (t)->capacity, __VA_ARGS__);              \
            return false;                                                    \
        }                                                                    \
    } while (0)

static oct_transport_state link_state(link_test *t)
{
    oct_link_status status;
    oct_link_get_status(t->link, &status);
    return status.state;
}

static bool exchange(link_test *t, uint16_t operation, const uint32_t *params, uint32_t count, size_t chunk,
                     uint16_t expected)
{
    uint32_t tid = operation == OCT_OP_OPEN_SESSION ? 0 : t->tid;
    oct_status status = oct_link_transact(t->link, t->loopback, operation, tid, params, count, NULL, 0, chunk, t->now,
                                          &t->reply);
    EXPECT(t, status == OCT_OK, "%s through the adapter: %s", operation_name(operation), oct_status_name(status));
    EXPECT(t, t->reply.response == expected, "%s through the adapter answered 0x%04X, expected 0x%04X",
           operation_name(operation), t->reply.response, expected);
    t->tid = operation == OCT_OP_OPEN_SESSION ? 1 : t->tid + 1;
    return true;
}

static bool self_test_script(link_test *t)
{
    static const uint8_t short_header[] = { 8, 0, 0, 0, 1, 0, 0x04, 0x10 };
    uint32_t session = 1, storage = 0, file = 0, file_size = 0;
    EXPECT(t, link_state(t) == OCT_TRANSPORT_WAITING, "The adapter isn't waiting for a host");
    EXPECT(t, oct_link_host_attached(t->link) == OCT_OK, "The host couldn't attach");
    EXPECT(t, link_state(t) == OCT_TRANSPORT_HOST_CONNECTED, "The adapter didn't report the host");

    /* Fragmented input: one byte at a time, then odd sizes. */
    if (!exchange(t, OCT_OP_GET_DEVICE_INFO, NULL, 0, 1, OCT_RC_OK)) return false;
    EXPECT(t, t->reply.has_data && t->reply.data.length > 40, "GetDeviceInfo sent no dataset");
    if (!exchange(t, OCT_OP_OPEN_SESSION, &session, 1, 5, OCT_RC_OK)) return false;
    EXPECT(t, link_state(t) == OCT_TRANSPORT_SESSION_ACTIVE, "The session isn't reported as active");
    if (!exchange(t, OCT_OP_GET_STORAGE_IDS, NULL, 0, 7, OCT_RC_OK)) return false;
    EXPECT(t, t->reply.data.length >= 8 && le32(t->reply.data.data) >= 1, "No storage was listed");
    storage = le32(t->reply.data.data + 4);
    uint32_t query[3] = { storage, 0, 0xFFFFFFFFu };
    if (!exchange(t, OCT_OP_GET_OBJECT_HANDLES, query, 3, 3, OCT_RC_OK)) return false;
    uint32_t count = t->reply.data.length >= 4 ? le32(t->reply.data.data) : 0;
    EXPECT(t, count >= 3 && t->reply.data.length >= 4u + 4u * count, "The root lists %u objects", count);
    uint32_t handles[16];
    uint32_t listed = count < 16 ? count : 16;
    for (uint32_t i = 0; i < listed; i++) handles[i] = le32(t->reply.data.data + 4 + 4 * i);
    for (uint32_t i = 0; i < listed; i++) {
        if (!exchange(t, OCT_OP_GET_OBJECT_INFO, &handles[i], 1, 13, OCT_RC_OK)) return false;
        EXPECT(t, t->reply.data.length >= 12, "ObjectInfo %u is too short", handles[i]);
        if (!file && le16(t->reply.data.data + 4) != 0x3001u) { /* not a folder */
            file = handles[i];
            file_size = le32(t->reply.data.data + 8);
        }
    }
    EXPECT(t, file != 0, "The root holds no file");
    if (!exchange(t, OCT_OP_GET_OBJECT, &file, 1, 64, OCT_RC_OK)) return false;
    EXPECT(t, t->reply.has_data && t->reply.data.length == file_size, "GetObject sent %zu bytes, expected %u",
           t->reply.data.length, file_size);

    /* A container shorter than its header, then recovery. */
    oct_link_status before, after;
    oct_link_get_status(t->link, &before);
    oct_link_input(t->link, short_header, sizeof short_header, t->now);
    oct_link_input(t->link, short_header, 4, t->now); /* completes a 12-byte header that declares 8 */
    oct_link_get_status(t->link, &after);
    EXPECT(t, after.malformed == before.malformed + 1 && oct_loopback_pending(t->loopback) == 0,
           "A malformed container wasn't dropped quietly");
    if (!exchange(t, OCT_OP_GET_STORAGE_IDS, NULL, 0, 0, OCT_RC_OK)) return false;

    /* A stalled container is dropped after the timeout. */
    oct_buf partial;
    oct_buf_init(&partial, 0);
    oct_container_command(&partial, OCT_OP_GET_STORAGE_IDS, t->tid, NULL, 0);
    oct_link_input(t->link, partial.data, 6, t->now);
    t->now += OCT_LINK_DEFAULT_TIMEOUT;
    oct_link_tick(t->link, t->now);
    oct_link_get_status(t->link, &after);
    EXPECT(t, after.timeouts == before.timeouts + 1, "A stalled container wasn't dropped");
    if (!exchange(t, OCT_OP_GET_STORAGE_IDS, NULL, 0, 2, OCT_RC_OK)) {
        oct_buf_free(&partial);
        return false;
    }

    /* Cancel drops a partial container; Get Device Status reports it until the next command. */
    oct_buf_clear(&partial);
    oct_container_command(&partial, OCT_OP_GET_STORAGE_IDS, t->tid, NULL, 0);
    oct_link_input(t->link, partial.data, 5, t->now);
    oct_buf_free(&partial);
    EXPECT(t, oct_link_cancel(t->link, t->tid) == OCT_OK, "Cancel was refused");
    EXPECT(t, oct_link_device_status(t->link) == OCT_RC_TRANSACTION_CANCELLED, "Device status ignores the cancel");
    if (!exchange(t, OCT_OP_GET_STORAGE_IDS, NULL, 0, 4, OCT_RC_OK)) return false;
    EXPECT(t, oct_link_device_status(t->link) == OCT_RC_OK, "Device status stayed cancelled");

    /* Device Reset closes the session; a new one opens, twice. */
    EXPECT(t, oct_link_device_reset(t->link) == OCT_OK, "Device reset failed");
    EXPECT(t, link_state(t) == OCT_TRANSPORT_HOST_CONNECTED, "The reset session is still reported");
    if (!exchange(t, OCT_OP_GET_STORAGE_IDS, NULL, 0, 0, OCT_RC_SESSION_NOT_OPEN)) return false;
    session = 2;
    if (!exchange(t, OCT_OP_OPEN_SESSION, &session, 1, 3, OCT_RC_OK)) return false;
    if (!exchange(t, OCT_OP_OPEN_SESSION, &session, 1, 3, OCT_RC_SESSION_ALREADY_OPEN)) return false;
    EXPECT(t, link_state(t) == OCT_TRANSPORT_SESSION_ACTIVE, "A repeated OpenSession lost the session");
    if (!exchange(t, OCT_OP_CLOSE_SESSION, NULL, 0, 1, OCT_RC_OK)) return false;
    EXPECT(t, link_state(t) == OCT_TRANSPORT_HOST_CONNECTED, "CloseSession left the session active");

    /* A disconnect mid-container, then a reconnect and a fresh session. */
    oct_link_input(t->link, short_header, 5, t->now);
    EXPECT(t, oct_link_host_detached(t->link, "self-test unplug") == OCT_OK, "The host couldn't detach");
    EXPECT(t, link_state(t) == OCT_TRANSPORT_DISCONNECTED, "The adapter didn't report the disconnect");
    EXPECT(t, oct_link_host_attached(t->link) == OCT_OK, "The host couldn't reconnect");
    session = 3;
    if (!exchange(t, OCT_OP_OPEN_SESSION, &session, 1, 2, OCT_RC_OK)) return false;
    if (!exchange(t, OCT_OP_CLOSE_SESSION, NULL, 0, 0, OCT_RC_OK)) return false;
    EXPECT(t, oct_loopback_pending(t->loopback) == 0, "The adapter sent transfers nobody asked for");
    return true;
}

oct_status oct_link_self_test(uint32_t *checks, char *failure, size_t capacity)
{
    char ignored[8];
    if (!failure || capacity == 0) {
        failure = ignored;
        capacity = sizeof ignored;
    }
    failure[0] = '\0';
    link_test t = { 0 };
    t.failure = failure;
    t.capacity = capacity;
    t.now = 1000;
    oct_service *service = NULL;
    oct_link_driver driver;
    oct_link_config config = { 64, OCT_LINK_DEFAULT_TIMEOUT, "selftest", true };
    oct_status status = oct_service_create(&service);
    if (status == OCT_OK) status = oct_loopback_create(&t.loopback);
    if (status == OCT_OK) {
        oct_loopback_driver(t.loopback, &driver);
        status = oct_link_create(&driver, &config, &t.link);
    }
    if (status == OCT_OK) {
        oct_transport transport;
        oct_link_transport(t.link, &transport);
        oct_service_set_log_component(service, NULL); /* only the adapter's own lines, as "loopback" */
        status = oct_service_set_transport(service, &transport);
    }
    oct_service_config service_config = { 0 };
    if (status == OCT_OK) status = oct_service_start(service, &service_config);
    if (status == OCT_OK && !self_test_script(&t)) status = OCT_E_IO;
    if (status != OCT_OK && !failure[0]) snprintf(failure, capacity, "Adapter self-test: %s", oct_status_name(status));
    if (service) oct_service_stop(service);
    oct_service_destroy(service);
    oct_link_destroy(t.link);
    oct_loopback_destroy(t.loopback);
    oct_buf_free(&t.reply.data);
    if (checks) *checks = t.checks;
    return status;
}
