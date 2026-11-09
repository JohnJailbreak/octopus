/*
 * oct_bridge_client.c - the client half of the bridge: what the ptpd integration links.
 *
 * Depends only on libc, oct_log and oct_bridge_io. It deliberately does not reference the
 * server, the responder or the storage engine, so the tweak injected into ptpd stays small
 * and carries none of the app's file-serving code.
 *
 * Not thread-safe: use one client from one thread (or serialise it yourself).
 */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#elif !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "oct_bridge.h"
#include "oct_bridge_io.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* ---- Client ------------------------------------------------------------------------------ */

struct oct_bridge_client {
    int  fd;
    char socket_path[SOCK_PATH_MAX];
};

oct_status oct_bridge_client_connect(const char *path, uint32_t timeout_ms, oct_bridge_client **out)
{
    if (!out) return OCT_E_INVALID_ARGUMENT;
    *out = NULL;
    if (!path) path = OCT_BRIDGE_DEFAULT_SOCKET;
    size_t length = strlen(path);
    if (length == 0 || length >= SOCK_PATH_MAX) return OCT_E_INVALID_ARGUMENT;
    if (!timeout_ms) timeout_ms = 5000;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, length + 1);
    int64_t deadline = oct_bridge_deadline_after(timeout_ms);
    int fd = -1;
    for (;;) {
        /* A fresh socket for every attempt: a socket whose connect failed isn't reusable everywhere. */
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return OCT_E_IO;
        oct_bridge_configure_socket(fd);
        if (connect(fd, (const struct sockaddr *)&addr, (socklen_t)sizeof addr) == 0) break;
        int error = errno;
        close(fd);
        fd = -1;
        if (error == EINTR) continue;
        if (error != ENOENT && error != ECONNREFUSED && error != EAGAIN) return OCT_E_IO;
        if (oct_bridge_monotonic_ms() >= deadline) return OCT_E_STATE; /* no server is listening (yet) */
        struct timespec pause = { 0, 50 * 1000000L };
        nanosleep(&pause, NULL);
    }
    oct_bridge_client *c = calloc(1, sizeof *c);
    if (!c) {
        close(fd);
        return OCT_E_NO_MEMORY;
    }
    c->fd = fd;
    memcpy(c->socket_path, path, length + 1);
    *out = c;
    return OCT_OK;
}

void oct_bridge_client_destroy(oct_bridge_client *c)
{
    if (!c) return;
    if (c->fd >= 0) close(c->fd);
    free(c);
}

oct_status oct_bridge_client_send_frame(oct_bridge_client *c, uint8_t type, const void *payload, size_t length)
{
    if (!c || c->fd < 0 || (length && !payload) || length > OCT_BRIDGE_MAX_PAYLOAD) return OCT_E_INVALID_ARGUMENT;
    return oct_bridge_send_frame(c->fd, type, payload, length) ? OCT_OK : OCT_E_IO;
}

oct_status oct_bridge_client_host_event(oct_bridge_client *c, bool attached, const char *reason)
{
    const char *payload = (!attached && reason) ? reason : NULL;
    return oct_bridge_client_send_frame(c, attached ? OCT_BRIDGE_MSG_HOST_ATTACHED : OCT_BRIDGE_MSG_HOST_DETACHED,
                                        payload, payload ? strlen(payload) : 0);
}

oct_status oct_bridge_client_send_bulk(oct_bridge_client *c, const uint8_t *bytes, size_t length)
{
    return oct_bridge_client_send_frame(c, OCT_BRIDGE_MSG_BULK_FROM_HOST, bytes, length);
}

oct_status oct_bridge_client_lifecycle(oct_bridge_client *c, uint8_t type)
{
    if (type != OCT_BRIDGE_MSG_BUS_RESET && type != OCT_BRIDGE_MSG_SUSPEND && type != OCT_BRIDGE_MSG_RESUME) {
        return OCT_E_INVALID_ARGUMENT;
    }
    return oct_bridge_client_send_frame(c, type, NULL, 0);
}

static oct_status client_receive(oct_bridge_client *c, uint32_t timeout_ms, uint8_t *type, uint8_t **out_data,
                                 size_t *out_length)
{
    switch (oct_bridge_recv_frame(c->fd, -1, oct_bridge_deadline_after(timeout_ms), type, out_data, out_length)) {
    case IO_OK: return OCT_OK;
    case IO_TIMEOUT: return OCT_E_STATE;
    case IO_CLOSED:
    case IO_ERROR:
    case IO_WOKEN: break;
    }
    return OCT_E_IO;
}

oct_status oct_bridge_client_receive(oct_bridge_client *c, uint8_t *type, uint8_t **out_data, size_t *out_length,
                                     uint32_t timeout_ms)
{
    if (!c || c->fd < 0 || !type || !out_data || !out_length) return OCT_E_INVALID_ARGUMENT;
    return client_receive(c, timeout_ms, type, out_data, out_length);
}

oct_status oct_bridge_client_class_request(oct_bridge_client *c, uint8_t request, const uint8_t *data,
                                           size_t data_length, uint8_t *out_reply, size_t capacity,
                                           size_t *out_reply_length, bool *out_stall)
{
    if (out_reply_length) *out_reply_length = 0;
    if (out_stall) *out_stall = false;
    if (!c || c->fd < 0 || (data_length && !data) || data_length >= OCT_BRIDGE_MAX_PAYLOAD) {
        return OCT_E_INVALID_ARGUMENT;
    }
    uint8_t *payload = malloc(1 + data_length);
    if (!payload) return OCT_E_NO_MEMORY;
    payload[0] = request;
    if (data_length) memcpy(payload + 1, data, data_length);
    oct_status status = oct_bridge_client_send_frame(c, OCT_BRIDGE_MSG_CLASS_REQUEST, payload, 1 + data_length);
    free(payload);
    if (status != OCT_OK) return status;
    uint8_t type = 0;
    uint8_t *reply = NULL;
    size_t reply_length = 0;
    status = client_receive(c, REPLY_TIMEOUT_MS, &type, &reply, &reply_length);
    if (status != OCT_OK) return status;
    if (type != OCT_BRIDGE_MSG_CLASS_RESPONSE || reply_length < 1) {
        free(reply);
        return OCT_E_IO;
    }
    if (reply[0] != OCT_BRIDGE_CLASS_COMPLETED) {
        if (out_stall) *out_stall = true;
    } else if (reply_length > 1) {
        if (!out_reply || capacity < reply_length - 1) {
            free(reply);
            return OCT_E_TOO_LARGE;
        }
        memcpy(out_reply, reply + 1, reply_length - 1);
        if (out_reply_length) *out_reply_length = reply_length - 1;
    }
    free(reply);
    return OCT_OK;
}

oct_status oct_bridge_client_ping(oct_bridge_client *c, char *info, size_t capacity, uint32_t timeout_ms)
{
    if (info && capacity) info[0] = '\0';
    oct_status status = oct_bridge_client_send_frame(c, OCT_BRIDGE_MSG_PING, NULL, 0);
    if (status != OCT_OK) return status;
    uint8_t type = 0;
    uint8_t *reply = NULL;
    size_t length = 0;
    status = client_receive(c, timeout_ms ? timeout_ms : REPLY_TIMEOUT_MS, &type, &reply, &length);
    if (status != OCT_OK) return status;
    if (type != OCT_BRIDGE_MSG_PONG) {
        free(reply);
        return OCT_E_IO;
    }
    if (info && capacity && length) {
        size_t copy = length < capacity - 1 ? length : capacity - 1;
        memcpy(info, reply, copy);
        info[copy] = '\0';
    }
    free(reply);
    return OCT_OK;
}

/* ---- PTP containers ---------------------------------------------------------------------- */

#define PTP_CONTAINER_HEADER 12u
#define PTP_MAX_PARAMS       5u

static void put_u16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value & 0xFFu);
    out[1] = (uint8_t)((value >> 8) & 0xFFu);
}

static void put_u32(uint8_t *out, uint32_t value)
{
    put_u16(out, (uint16_t)(value & 0xFFFFu));
    put_u16(out + 2, (uint16_t)((value >> 16) & 0xFFFFu));
}

static uint16_t get_u16(const uint8_t *in)
{
    return (uint16_t)(in[0] | (in[1] << 8));
}

static uint32_t get_u32(const uint8_t *in)
{
    return (uint32_t)get_u16(in) | ((uint32_t)get_u16(in + 2) << 16);
}

oct_status oct_bridge_client_send_command(oct_bridge_client *c, uint16_t operation,
                                           uint32_t transaction_id, const uint32_t *params,
                                           uint32_t count)
{
    if (!c || c->fd < 0 || count > PTP_MAX_PARAMS || (count && !params)) return OCT_E_INVALID_ARGUMENT;
    uint8_t container[PTP_CONTAINER_HEADER + 4u * PTP_MAX_PARAMS];
    uint32_t length = PTP_CONTAINER_HEADER + 4u * count;
    put_u32(container, length);
    put_u16(container + 4, 1); /* a command container */
    put_u16(container + 6, operation);
    put_u32(container + 8, transaction_id);
    for (uint32_t i = 0; i < count; i++) put_u32(container + PTP_CONTAINER_HEADER + 4u * i, params[i]);
    return oct_bridge_client_send_bulk(c, container, length);
}

oct_status oct_bridge_client_receive_container(oct_bridge_client *c, uint16_t *out_code,
                                                uint16_t *out_container_type, uint8_t **out_payload,
                                                size_t *out_length, uint32_t timeout_ms)
{
    if (out_code) *out_code = 0;
    if (out_container_type) *out_container_type = 0;
    if (out_payload) *out_payload = NULL;
    if (out_length) *out_length = 0;
    if (!c || c->fd < 0) return OCT_E_INVALID_ARGUMENT;
    uint8_t type = 0;
    uint8_t *frame = NULL;
    size_t frame_length = 0;
    oct_status status = client_receive(c, timeout_ms, &type, &frame, &frame_length);
    if (status != OCT_OK) return status;
    if (type != OCT_BRIDGE_MSG_BULK_TO_HOST || frame_length < PTP_CONTAINER_HEADER ||
        get_u32(frame) != frame_length) {
        free(frame);
        return OCT_E_MALFORMED;
    }
    if (out_container_type) *out_container_type = get_u16(frame + 4);
    if (out_code) *out_code = get_u16(frame + 6);
    size_t payload = frame_length - PTP_CONTAINER_HEADER;
    if (out_payload && payload) {
        uint8_t *copy = malloc(payload);
        if (!copy) {
            free(frame);
            return OCT_E_NO_MEMORY;
        }
        memcpy(copy, frame + PTP_CONTAINER_HEADER, payload);
        *out_payload = copy;
        if (out_length) *out_length = payload;
    }
    free(frame);
    return OCT_OK;
}
