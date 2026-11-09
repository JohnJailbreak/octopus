/* oct_bridge.c - socket transport between Octopus and the ptpd integration. See oct_bridge.h. */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#elif !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "oct_bridge.h"
#include "oct_log.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define BRIDGE_COMPONENT "bridge"
#define LISTEN_BACKLOG   1
#define SOCK_PATH_MAX    104 /* sun_path on Darwin and Linux */

/* ---- Wire protocol ----------------------------------------------------------------------- */

size_t oct_bridge_encode(uint8_t type, const void *payload, size_t length, uint8_t *buf,
                          size_t capacity)
{
    if (length > OCT_BRIDGE_MAX_PAYLOAD || capacity < OCT_BRIDGE_HEADER_SIZE + length) return 0;
    buf[0] = type;
    uint32_t l = (uint32_t)length;
    buf[1] = (uint8_t)(l & 0xFFu);
    buf[2] = (uint8_t)((l >> 8) & 0xFFu);
    buf[3] = (uint8_t)((l >> 16) & 0xFFu);
    buf[4] = (uint8_t)((l >> 24) & 0xFFu);
    if (length && payload) memcpy(buf + OCT_BRIDGE_HEADER_SIZE, payload, length);
    return OCT_BRIDGE_HEADER_SIZE + length;
}

uint8_t *oct_bridge_alloc(uint8_t type, const void *payload, size_t length, size_t *out_length)
{
    size_t total = OCT_BRIDGE_HEADER_SIZE + length;
    uint8_t *buf = malloc(total ? total : 1);
    if (!buf) return NULL;
    size_t written = oct_bridge_encode(type, payload, length, buf, total);
    if (!written) {
        free(buf);
        return NULL;
    }
    if (out_length) *out_length = written;
    return buf;
}

/* ---- I/O primitives ---------------------------------------------------------------------- */

static bool write_all(int fd, const uint8_t *bytes, size_t length)
{
    while (length > 0) {
        ssize_t written = write(fd, bytes, length);
        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        bytes += written;
        length -= (size_t)written;
    }
    return true;
}

static bool read_all_fd(int fd, uint8_t *out, size_t length, uint32_t timeout_ms)
{
    struct timeval timeout = { (long)(timeout_ms / 1000u), (int)((timeout_ms % 1000u) * 1000) };
    size_t got = 0;
    while (got < length) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(fd, &fds);
        int ready = select(fd + 1, &fds, NULL, NULL, timeout_ms ? &timeout : NULL);
        if (ready <= 0) return false;
        ssize_t n = read(fd, out + got, length - got);
        if (n <= 0) return false;
        got += (size_t)n;
    }
    return true;
}

static bool send_msg(int fd, uint8_t type, const void *payload, size_t length)
{
    uint8_t header[OCT_BRIDGE_HEADER_SIZE];
    oct_bridge_encode(type, NULL, 0, header, sizeof header); /* header only: type + 0 length */
    header[0] = type;
    uint32_t l = (uint32_t)length;
    header[1] = (uint8_t)(l & 0xFFu);
    header[2] = (uint8_t)((l >> 8) & 0xFFu);
    header[3] = (uint8_t)((l >> 16) & 0xFFu);
    header[4] = (uint8_t)((l >> 24) & 0xFFu);
    return write_all(fd, header, OCT_BRIDGE_HEADER_SIZE) &&
           (!length || write_all(fd, payload, length));
}

static bool recv_msg(int fd, uint8_t *out_type, uint8_t **out_payload, size_t *out_length,
                     uint32_t timeout_ms)
{
    uint8_t header[OCT_BRIDGE_HEADER_SIZE];
    if (!read_all_fd(fd, header, sizeof header, timeout_ms)) return false;
    *out_type = header[0];
    uint32_t length = (uint32_t)header[1] | ((uint32_t)header[2] << 8) |
                      ((uint32_t)header[3] << 16) | ((uint32_t)header[4] << 24);
    if (length > OCT_BRIDGE_MAX_PAYLOAD) return false;
    uint8_t *payload = length ? malloc(length) : NULL;
    if (length && !payload) return false;
    if (length && !read_all_fd(fd, payload, length, timeout_ms)) {
        free(payload);
        return false;
    }
    if (out_payload) *out_payload = payload; else free(payload);
    if (out_length) *out_length = length;
    return true;
}

/* ---- Server (Octopus app side) ----------------------------------------------------------- */

typedef enum bridge_server_state {
    BRIDGE_IDLE = 0,
    BRIDGE_RUNNING,
    BRIDGE_STOPPED,
} bridge_server_state;

struct oct_bridge {
    char                 socket_path[SOCK_PATH_MAX];
    int                  listen_fd;      /* the listening socket */
    int                  client_fd;      /* the connected client, or -1 */
    oct_link            *link;
    pthread_t            thread;
    pthread_mutex_t      lock;
    bridge_server_state  state;
    bool                 host_connected;
    char                 last_error[256];
};

static void bridge_error(oct_bridge *b, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(b->last_error, sizeof b->last_error, fmt, args);
    va_end(args);
    oct_logf(OCT_LOG_WARN, BRIDGE_COMPONENT, "bridge_error", "%s", b->last_error);
}

/* Called by the server thread when the client sends BULK_FROM_HOST. */
static void handle_bulk_from_host(oct_bridge *b, const uint8_t *bytes, size_t length)
{
    pthread_mutex_lock(&b->lock);
    oct_link *link = b->link;
    pthread_mutex_unlock(&b->lock);
    if (!link) return;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t now_ms = (int64_t)now.tv_sec * 1000 + (int64_t)(now.tv_nsec / 1000000);
    oct_status status = oct_link_input(link, bytes, length, now_ms);
    if (status != OCT_OK && status != OCT_E_STATE) {
        oct_logf(OCT_LOG_WARN, BRIDGE_COMPONENT, "input", "oct_link_input: %s",
                 oct_status_name(status));
    }
}

/* oct_link_driver_ops.send: called by the responder to send a response to the host. */
static oct_status bridge_driver_send(void *ctx, oct_link_pipe pipe, const uint8_t *bytes,
                                     size_t length, bool zero_length_packet)
{
    oct_bridge *b = ctx;
    (void)zero_length_packet;
    uint8_t type = pipe == OCT_LINK_INTERRUPT_IN ? OCT_BRIDGE_MSG_INTERRUPT_EVENT
                                                  : OCT_BRIDGE_MSG_BULK_TO_HOST;
    pthread_mutex_lock(&b->lock);
    int fd = b->client_fd;
    pthread_mutex_unlock(&b->lock);
    if (fd < 0) return OCT_E_STATE;
    if (!send_msg(fd, type, bytes, length)) {
        bridge_error(b, "Sending %s to the client failed: %s",
                     type == OCT_BRIDGE_MSG_INTERRUPT_EVENT ? "event" : "bulk", strerror(errno));
        return OCT_E_IO;
    }
    return OCT_OK;
}

static void *bridge_accept_thread(void *ctx); /* forward */

/* oct_link_driver_ops.start: also starts the socket listener. */
static oct_status bridge_start_socket(oct_bridge *b)
{
    unlink(b->socket_path);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, b->socket_path, sizeof addr.sun_path - 1);
    b->listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (b->listen_fd < 0) {
        oct_logf(OCT_LOG_ERROR, BRIDGE_COMPONENT, "socket_failed",
                 "Creating the bridge socket failed: %s", strerror(errno));
        return OCT_E_IO;
    }
    if (bind(b->listen_fd, (const struct sockaddr *)&addr, sizeof addr) != 0 ||
        listen(b->listen_fd, LISTEN_BACKLOG) != 0) {
        oct_logf(OCT_LOG_ERROR, BRIDGE_COMPONENT, "listen_failed",
                 "Binding/listening on %s failed: %s", b->socket_path, strerror(errno));
        close(b->listen_fd);
        b->listen_fd = -1;
        return OCT_E_IO;
    }
    pthread_mutex_lock(&b->lock);
    b->state = BRIDGE_RUNNING;
    pthread_mutex_unlock(&b->lock);
    if (pthread_create(&b->thread, NULL, bridge_accept_thread, b) != 0) {
        close(b->listen_fd);
        b->listen_fd = -1;
        b->state = BRIDGE_IDLE;
        return OCT_E_IO;
    }
    oct_logf(OCT_LOG_INFO, BRIDGE_COMPONENT, "transport_start",
             "Bridge transport started, socket %s", b->socket_path);
    return OCT_OK;
}

/* oct_link_driver_ops.start: stores the link reference and starts the socket listener. */
static oct_status bridge_driver_start(void *ctx, oct_link *link)
{
    oct_bridge *b = ctx;
    pthread_mutex_lock(&b->lock);
    b->link = link;
    pthread_mutex_unlock(&b->lock);
    return bridge_start_socket(b);
}

/* oct_link_driver_ops.stop: the link calls this on stop. */
static void bridge_driver_stop(void *ctx)
{
    oct_bridge *b = ctx;
    pthread_mutex_lock(&b->lock);
    b->link = NULL;
    b->state = BRIDGE_STOPPED;
    int client = b->client_fd;
    b->client_fd = -1;
    b->host_connected = false;
    int listen = b->listen_fd;
    b->listen_fd = -1;
    pthread_mutex_unlock(&b->lock);
    if (client >= 0) close(client);
    if (listen >= 0) close(listen); /* wakes the accept thread's select */
    unlink(b->socket_path);
    if (b->thread) {
        pthread_join(b->thread, NULL);
        b->thread = 0;
    }
    oct_logf(OCT_LOG_INFO, BRIDGE_COMPONENT, "stopped", "Bridge transport stopped");
}

/* bridge_driver_stop signals the accept thread to stop and closes the client connection. */

/* Background thread: accepts one client and reads messages from it. */
static void *bridge_accept_thread(void *ctx)
{
    oct_bridge *b = ctx;
    for (;;) {
        pthread_mutex_lock(&b->lock);
        bool running = b->state == BRIDGE_RUNNING;
        int listen_fd = b->listen_fd;
        pthread_mutex_unlock(&b->lock);
        if (!running) break;
        struct timeval tv = { 1, 0 };
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(listen_fd, &fds);
        int ready = select(listen_fd + 1, &fds, NULL, NULL, &tv);
        if (ready <= 0) continue;
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) continue;
        oct_logf(OCT_LOG_INFO, BRIDGE_COMPONENT, "connected",
                 "ptpd integration connected to the bridge");
        pthread_mutex_lock(&b->lock);
        b->client_fd = fd;
        pthread_mutex_unlock(&b->lock);
        /* Read messages until the client disconnects or we stop. */
        for (;;) {
            uint8_t type;
            uint8_t *payload = NULL;
            size_t length = 0;
            if (!recv_msg(fd, &type, &payload, &length, 1000)) {
                free(payload);
                if (errno != EAGAIN) break;
                /* timeout: check if still running */
                pthread_mutex_lock(&b->lock);
                bool still = b->state == BRIDGE_RUNNING;
                pthread_mutex_unlock(&b->lock);
                if (!still) break;
                continue;
            }
            switch (type) {
            case OCT_BRIDGE_MSG_HOST_ATTACHED: {
                pthread_mutex_lock(&b->lock);
                b->host_connected = true;
                b->last_error[0] = '\0';
                oct_link *link = b->link;
                pthread_mutex_unlock(&b->lock);
                if (link) oct_link_host_attached(link);
                oct_logf(OCT_LOG_INFO, BRIDGE_COMPONENT, "host_attached",
                         "Host attached via the bridge");
                break;
            }
            case OCT_BRIDGE_MSG_HOST_DETACHED: {
                /* Copy the reason into a local buffer before payload is freed below. */
                char reason[OCT_EVENT_TEXT_MAX] = "bridge disconnect";
                if (payload && length) {
                    size_t copy = length < sizeof reason - 1 ? length : sizeof reason - 1;
                    memcpy(reason, payload, copy);
                    reason[copy] = '\0';
                }
                pthread_mutex_lock(&b->lock);
                b->host_connected = false;
                oct_link *detach_link = b->link;
                pthread_mutex_unlock(&b->lock);
                if (detach_link) oct_link_host_detached(detach_link, reason);
                oct_logf(OCT_LOG_INFO, BRIDGE_COMPONENT, "host_detached",
                         "Host detached: %.160s", reason);
                break;
            }
            case OCT_BRIDGE_MSG_BULK_FROM_HOST:
                if (payload && length) handle_bulk_from_host(b, payload, length);
                break;
            case OCT_BRIDGE_MSG_BUS_RESET: {
                pthread_mutex_lock(&b->lock);
                oct_link *reset_link = b->link;
                pthread_mutex_unlock(&b->lock);
                if (reset_link) oct_link_bus_reset(reset_link);
                break;
            }
            case OCT_BRIDGE_MSG_SUSPEND: {
                pthread_mutex_lock(&b->lock);
                oct_link *susp_link = b->link;
                pthread_mutex_unlock(&b->lock);
                if (susp_link) oct_link_set_suspended(susp_link, true);
                break;
            }
            case OCT_BRIDGE_MSG_RESUME: {
                pthread_mutex_lock(&b->lock);
                oct_link *resum_link = b->link;
                pthread_mutex_unlock(&b->lock);
                if (resum_link) oct_link_set_suspended(resum_link, false);
                break;
            }
            case OCT_BRIDGE_MSG_CLASS_REQUEST: {
                /* byte 0 is bRequest, bytes 1-N are out_data */
                if (!length) { free(payload); continue; }
                uint8_t request = payload[0];
                uint8_t reply[8] = { 0 };
                size_t reply_length = 0;
                pthread_mutex_lock(&b->lock);
                oct_link *class_link = b->link;
                pthread_mutex_unlock(&b->lock);
                oct_status status = OCT_E_STATE;
                if (class_link) {
                    status = oct_link_class_request(class_link, request, payload + 1, length - 1,
                                                    reply, sizeof reply, &reply_length);
                }
                /* Reply with CLASS_RESPONSE: empty payload = stall, otherwise the reply bytes. */
                bool stall = (status == OCT_E_UNSUPPORTED || status == OCT_E_MALFORMED);
                send_msg(fd, OCT_BRIDGE_MSG_CLASS_RESPONSE, stall ? NULL : reply,
                         stall ? 0 : reply_length);
                break;
            }
            case OCT_BRIDGE_MSG_PING: {
                const char *path = b->socket_path;
                send_msg(fd, OCT_BRIDGE_MSG_PONG, path, strlen(path));
                break;
            }
            default:
                oct_logf(OCT_LOG_WARN, BRIDGE_COMPONENT, "unknown_msg",
                         "Unknown message type 0x%02X from the client, length %zu", type, length);
                break;
            }
            free(payload);
        }
        pthread_mutex_lock(&b->lock);
        b->client_fd = -1;
        b->host_connected = false;
        pthread_mutex_unlock(&b->lock);
        close(fd);
        if (b->link) oct_link_host_detached(b->link, "bridge client disconnected");
        oct_logf(OCT_LOG_INFO, BRIDGE_COMPONENT, "disconnected", "ptpd integration disconnected");
    }
    return NULL;
}

/* oct_transport_ops.start: open the socket and start the accept thread. */

static const oct_link_driver_ops k_bridge_link_ops = {
    "bridge-socket",
    bridge_driver_start,
    bridge_driver_stop,
    bridge_driver_send,
};

oct_status oct_bridge_create(const char *path, oct_bridge **out)
{
    if (!out) return OCT_E_INVALID_ARGUMENT;
    if (!path) path = OCT_BRIDGE_DEFAULT_SOCKET;
    if (strlen(path) >= SOCK_PATH_MAX) return OCT_E_INVALID_ARGUMENT;
    oct_bridge *b = calloc(1, sizeof *b);
    if (!b) return OCT_E_NO_MEMORY;
    strncpy(b->socket_path, path, sizeof b->socket_path - 1);
    b->listen_fd = -1;
    b->client_fd = -1;
    b->state = BRIDGE_IDLE;
    pthread_mutex_init(&b->lock, NULL);
    *out = b;
    return OCT_OK;
}

void oct_bridge_destroy(oct_bridge *b)
{
    if (!b) return;
    /* If the service already stopped the transport, bridge_driver_stop was called; the thread was
     * joined and the sockets were closed. Otherwise, do it now (destroy without a service stop). */
    if (b->state == BRIDGE_RUNNING) {
        bridge_driver_stop(b);
    } else if (b->thread) {
        /* state is STOPPED but the thread hasn't been joined (unusual path). */
        pthread_join(b->thread, NULL);
        b->thread = 0;
    }
    pthread_mutex_destroy(&b->lock);
    free(b);
}

oct_status oct_bridge_link_create(oct_bridge *b, oct_link **out_link, oct_transport *out_transport)
{
    if (!b || !out_transport) return OCT_E_INVALID_ARGUMENT;
    static const oct_link_config link_config = {
        512,   /* bulk_packet: USB 2.0 high-speed */
        5000,  /* timeout_ms */
        "bridge",
        false, /* rehearsal */
        64,    /* event_packet */
    };
    oct_link_driver driver = { &k_bridge_link_ops, b };
    oct_link *link = NULL;
    oct_status status = oct_link_create(&driver, &link_config, &link);
    if (status != OCT_OK) return status;
    oct_link_transport(link, out_transport);
    if (out_link) *out_link = link;
    return OCT_OK;
}

bool oct_bridge_client_connected(const oct_bridge *b) { return b && b->client_fd >= 0; }
const char *oct_bridge_socket_path(const oct_bridge *b) { return b ? b->socket_path : ""; }

/* ---- Client ---------------------------------------------------------------------- */

struct oct_bridge_client {
    int  fd;
    char socket_path[SOCK_PATH_MAX];
};

oct_status oct_bridge_client_connect(const char *path, uint32_t timeout_ms,
                                      oct_bridge_client **out)
{
    if (!out) return OCT_E_INVALID_ARGUMENT;
    if (!path) path = OCT_BRIDGE_DEFAULT_SOCKET;
    if (strlen(path) >= SOCK_PATH_MAX) return OCT_E_INVALID_ARGUMENT;
    if (!timeout_ms) timeout_ms = 5000;
    *out = NULL;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return OCT_E_IO;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof addr.sun_path - 1);
    uint32_t elapsed = 0, interval = 100;
    while (connect(fd, (const struct sockaddr *)&addr, sizeof addr) != 0) {
        if (errno != ENOENT && errno != ECONNREFUSED) {
            close(fd);
            return OCT_E_IO;
        }
        elapsed += interval;
        if (elapsed >= timeout_ms) {
            close(fd);
            return OCT_E_STATE; /* server not up yet */
        }
        struct timespec sleep = { 0, (long)interval * 1000000 };
        nanosleep(&sleep, NULL);
    }
    oct_bridge_client *c = calloc(1, sizeof *c);
    if (!c) {
        close(fd);
        return OCT_E_NO_MEMORY;
    }
    c->fd = fd;
    strncpy(c->socket_path, path, sizeof c->socket_path - 1);
    *out = c;
    return OCT_OK;
}

void oct_bridge_client_destroy(oct_bridge_client *c)
{
    if (!c) return;
    if (c->fd >= 0) close(c->fd);
    free(c);
}

oct_status oct_bridge_client_host_event(oct_bridge_client *c, bool attached, const char *reason)
{
    if (!c || c->fd < 0) return OCT_E_INVALID_ARGUMENT;
    uint8_t type = attached ? OCT_BRIDGE_MSG_HOST_ATTACHED : OCT_BRIDGE_MSG_HOST_DETACHED;
    const char *payload = (!attached && reason) ? reason : NULL;
    size_t length = payload ? strlen(payload) : 0;
    return send_msg(c->fd, type, payload, length) ? OCT_OK : OCT_E_IO;
}

oct_status oct_bridge_client_send_bulk(oct_bridge_client *c, const uint8_t *bytes, size_t length)
{
    if (!c || c->fd < 0 || (!bytes && length)) return OCT_E_INVALID_ARGUMENT;
    return send_msg(c->fd, OCT_BRIDGE_MSG_BULK_FROM_HOST, bytes, length) ? OCT_OK : OCT_E_IO;
}

oct_status oct_bridge_client_class_request(oct_bridge_client *c, uint8_t request,
                                            const uint8_t *data, size_t data_length,
                                            uint8_t *out_reply, size_t capacity,
                                            size_t *out_reply_length, bool *out_stall)
{
    if (!c || c->fd < 0) return OCT_E_INVALID_ARGUMENT;
    if (out_reply_length) *out_reply_length = 0;
    if (out_stall) *out_stall = false;
    /* Header: request code first, then out data. */
    size_t payload_length = 1 + data_length;
    uint8_t *payload = malloc(payload_length ? payload_length : 1);
    if (!payload) return OCT_E_NO_MEMORY;
    payload[0] = request;
    if (data && data_length) memcpy(payload + 1, data, data_length);
    bool sent = send_msg(c->fd, OCT_BRIDGE_MSG_CLASS_REQUEST, payload, payload_length);
    free(payload);
    if (!sent) return OCT_E_IO;
    uint8_t type;
    uint8_t *reply = NULL;
    size_t reply_length = 0;
    if (!recv_msg(c->fd, &type, &reply, &reply_length, 2000)) return OCT_E_IO;
    if (type != OCT_BRIDGE_MSG_CLASS_RESPONSE) {
        free(reply);
        return OCT_E_IO;
    }
    if (!reply_length) {
        if (out_stall) *out_stall = true;
    } else if (out_reply && capacity >= reply_length) {
        memcpy(out_reply, reply, reply_length);
        if (out_reply_length) *out_reply_length = reply_length;
    }
    free(reply);
    return OCT_OK;
}

oct_status oct_bridge_client_lifecycle(oct_bridge_client *c, uint8_t type)
{
    if (!c || c->fd < 0) return OCT_E_INVALID_ARGUMENT;
    if (type != OCT_BRIDGE_MSG_BUS_RESET && type != OCT_BRIDGE_MSG_SUSPEND &&
        type != OCT_BRIDGE_MSG_RESUME) {
        return OCT_E_INVALID_ARGUMENT;
    }
    return send_msg(c->fd, type, NULL, 0) ? OCT_OK : OCT_E_IO;
}

oct_status oct_bridge_client_ping(oct_bridge_client *c, char *info, size_t capacity,
                                  uint32_t timeout_ms)
{
    if (!c || c->fd < 0) return OCT_E_INVALID_ARGUMENT;
    if (!timeout_ms) timeout_ms = 2000;
    if (!send_msg(c->fd, OCT_BRIDGE_MSG_PING, NULL, 0)) return OCT_E_IO;
    uint8_t type;
    uint8_t *reply = NULL;
    size_t length = 0;
    if (!recv_msg(c->fd, &type, &reply, &length, timeout_ms)) return OCT_E_STATE;
    if (type != OCT_BRIDGE_MSG_PONG) {
        free(reply);
        return OCT_E_IO;
    }
    if (info && capacity && reply && length) {
        size_t copy = length < capacity - 1 ? length : capacity - 1;
        memcpy(info, reply, copy);
        info[copy] = '\0';
    } else if (info && capacity) {
        info[0] = '\0';
    }
    free(reply);
    return OCT_OK;
}

oct_status oct_bridge_client_receive(oct_bridge_client *c, uint8_t *type, uint8_t **out_data,
                                      size_t *out_length, uint32_t timeout_ms)
{
    if (!c || c->fd < 0 || !type || !out_data || !out_length) return OCT_E_INVALID_ARGUMENT;
    if (!recv_msg(c->fd, type, out_data, out_length, timeout_ms)) return OCT_E_IO;
    return OCT_OK;
}
