/* oct_bridge.c - socket transport between Octopus and the ptpd integration. See oct_bridge.h.
 *
 * Threading. The server has one thread, started by the link's start and joined by its stop. That thread alone
 * reads from the connected client's socket and alone closes it. Every frame written to the client, by the
 * responder through the link or by the thread's own replies, is written whole under send_lock, so frames never
 * interleave; the thread takes send_lock before it closes the socket, so nothing writes to a closed (or reused)
 * descriptor. Stop never closes a descriptor the thread may be using: it shuts the client down, which fails any
 * blocked read or write, and wakes the thread through a pipe; it closes the listening socket and the pipe only
 * after joining the thread. Every other shared field is read and written under lock.
 *
 * Sockets never raise SIGPIPE (SO_NOSIGPIPE on Darwin, MSG_NOSIGNAL elsewhere): a peer that goes away is an error
 * to report, not a signal that ends the app, or ptpd. */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#elif !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "oct_bridge.h"
#include "oct_bridge_io.h"
#include "oct_log.h"
#include "../Protocol/oct_ptp.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define BRIDGE_COMPONENT   "bridge"
#define LISTEN_BACKLOG     1
#define SOCK_PATH_MAX      104   /* sun_path is 104 bytes on Darwin, 108 on Linux */
#define FRAME_TIMEOUT_MS   5000  /* for the rest of a frame once its header has arrived */
#define SEND_TIMEOUT_S     5     /* a peer that stops reading fails the write instead of blocking it */
#define REPLY_TIMEOUT_MS   2000

#if defined(MSG_NOSIGNAL) && !defined(__APPLE__)
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0 /* Darwin: SO_NOSIGPIPE, set on every socket by configure_socket */
#endif
#if defined(__APPLE__) && !defined(SO_NOSIGPIPE)
#error "SO_NOSIGPIPE is needed: without it a peer that leaves would end the process with SIGPIPE"
#endif

/* ---- Server (Octopus app side) ----------------------------------------------------------- */

struct oct_bridge {
    char            socket_path[SOCK_PATH_MAX]; /* immutable after create */
    pthread_mutex_t lock;                       /* guards every field below */
    pthread_mutex_t send_lock;                  /* held for each whole frame written to the client */
    oct_link       *link;                       /* set by start, cleared by stop */
    bool            running;                    /* between start and stop */
    bool            thread_started;             /* the thread exists and hasn't been joined */
    pthread_t       thread;
    int             listen_fd;                  /* created by start, closed by stop after the join */
    int             wake_read;                  /* the stop pipe: the thread watches this end */
    int             wake_write;
    int             client_fd;                  /* the connected client, or -1; closed only by the thread */
    bool            host_attached;              /* the client reported a host on the PTP interface */
};

static oct_link *current_link(oct_bridge *b)
{
    pthread_mutex_lock(&b->lock);
    oct_link *link = b->link;
    pthread_mutex_unlock(&b->lock);
    return link;
}

static bool still_running(oct_bridge *b)
{
    pthread_mutex_lock(&b->lock);
    bool running = b->running;
    pthread_mutex_unlock(&b->lock);
    return running;
}

/* Writes one frame to the connected client, whole. OCT_E_STATE when no client is connected. A failed write may
 * have left part of a frame behind, so the stream can't be trusted any more: the client is shut down, and the
 * thread drops it. */
static oct_status bridge_send(oct_bridge *b, uint8_t type, const void *payload, size_t length)
{
    pthread_mutex_lock(&b->send_lock);
    pthread_mutex_lock(&b->lock);
    int fd = b->client_fd;
    pthread_mutex_unlock(&b->lock);
    oct_status status = OCT_E_STATE;
    if (fd >= 0) status = oct_bridge_send_frame(fd, type, payload, length) ? OCT_OK : OCT_E_IO;
    int error = errno;
    if (status == OCT_E_IO) shutdown(fd, SHUT_RDWR); /* still open: closing needs send_lock, which we hold */
    pthread_mutex_unlock(&b->send_lock);
    if (status == OCT_E_IO) {
        oct_logf(OCT_LOG_WARN, BRIDGE_COMPONENT, "send_failed", "Sending a %s frame to the client failed: %s",
                 type == OCT_BRIDGE_MSG_INTERRUPT_EVENT ? "event" : type == OCT_BRIDGE_MSG_BULK_TO_HOST ? "bulk"
                                                                                                   : "reply",
                 strerror(error));
    }
    return status;
}

/* oct_link_driver_ops.send: the responder's data and responses, and events. */
static oct_status bridge_driver_send(void *ctx, oct_link_pipe pipe, const uint8_t *bytes, size_t length,
                                     bool zero_length_packet)
{
    (void)zero_length_packet;
    uint8_t type = pipe == OCT_LINK_INTERRUPT_IN ? OCT_BRIDGE_MSG_INTERRUPT_EVENT : OCT_BRIDGE_MSG_BULK_TO_HOST;
    return bridge_send(ctx, type, bytes, length);
}

static void handle_class_request(oct_bridge *b, const uint8_t *payload, size_t length)
{
    /* Request: byte 0 is bRequest, the rest the OUT data stage. Reply: a status byte, then any IN data. */
    uint8_t reply[1 + 8] = { OCT_BRIDGE_CLASS_STALL };
    size_t reply_length = 0;
    oct_link *link = current_link(b);
    if (length && link) {
        oct_status status = oct_link_class_request(link, payload[0], payload + 1, length - 1, reply + 1,
                                                   sizeof reply - 1, &reply_length);
        if (status == OCT_OK) reply[0] = OCT_BRIDGE_CLASS_COMPLETED;
        else reply_length = 0;
    }
    bridge_send(b, OCT_BRIDGE_MSG_CLASS_RESPONSE, reply, 1 + reply_length);
}

static void handle_frame(oct_bridge *b, uint8_t type, const uint8_t *payload, size_t length)
{
    oct_link *link = current_link(b);
    switch (type) {
    case OCT_BRIDGE_MSG_HOST_ATTACHED:
        pthread_mutex_lock(&b->lock);
        b->host_attached = true;
        pthread_mutex_unlock(&b->lock);
        if (link) oct_link_host_attached(link);
        oct_logf(OCT_LOG_INFO, BRIDGE_COMPONENT, "host_attached", "Host attached through the bridge");
        break;
    case OCT_BRIDGE_MSG_HOST_DETACHED: {
        char reason[OCT_EVENT_TEXT_MAX] = "the bridge client reported a detach";
        if (length) {
            size_t copy = length < sizeof reason - 1 ? length : sizeof reason - 1;
            memcpy(reason, payload, copy);
            reason[copy] = '\0';
        }
        pthread_mutex_lock(&b->lock);
        b->host_attached = false;
        pthread_mutex_unlock(&b->lock);
        if (link) oct_link_host_detached(link, reason);
        oct_logf(OCT_LOG_INFO, BRIDGE_COMPONENT, "host_detached", "Host detached: %.160s", reason);
        break;
    }
    case OCT_BRIDGE_MSG_BULK_FROM_HOST:
        if (link && length) {
            oct_status status = oct_link_input(link, payload, length, oct_bridge_monotonic_ms());
            if (status != OCT_OK && status != OCT_E_STATE) {
                oct_logf(OCT_LOG_WARN, BRIDGE_COMPONENT, "input", "The link refused %zu bytes: %s", length,
                         oct_status_name(status));
            }
        }
        break;
    case OCT_BRIDGE_MSG_BUS_RESET:
        if (link) oct_link_bus_reset(link);
        break;
    case OCT_BRIDGE_MSG_SUSPEND:
        if (link) oct_link_set_suspended(link, true);
        break;
    case OCT_BRIDGE_MSG_RESUME:
        if (link) oct_link_set_suspended(link, false);
        break;
    case OCT_BRIDGE_MSG_CLASS_REQUEST:
        handle_class_request(b, payload, length);
        break;
    case OCT_BRIDGE_MSG_PING:
        bridge_send(b, OCT_BRIDGE_MSG_PONG, b->socket_path, strlen(b->socket_path));
        break;
    default:
        oct_logf(OCT_LOG_WARN, BRIDGE_COMPONENT, "unknown_frame",
                 "Ignored a frame of unknown type 0x%02X (%zu bytes) from the client", type, length);
        break;
    }
}

/* Serves one client until it leaves, sends a malformed frame, or the server stops. Returns why it ended. */
static const char *serve_client(oct_bridge *b, int fd, int wake_fd)
{
    for (;;) {
        uint8_t type = 0;
        uint8_t *payload = NULL;
        size_t length = 0;
        switch (oct_bridge_recv_frame(fd, wake_fd, -1, &type, &payload, &length)) {
        case IO_OK: break;
        case IO_WOKEN: return "the bridge stopped";
        case IO_CLOSED: return "the bridge client disconnected";
        case IO_TIMEOUT:
        case IO_ERROR: return "the bridge client's connection failed or sent a malformed frame";
        }
        handle_frame(b, type, payload, length);
        free(payload);
    }
}

/* Takes the client away from writers, then closes it: the only place the client socket is closed. */
static void drop_client(oct_bridge *b, int fd, const char *reason)
{
    pthread_mutex_lock(&b->lock);
    b->client_fd = -1;
    bool attached = b->host_attached;
    b->host_attached = false;
    oct_link *link = b->link;
    pthread_mutex_unlock(&b->lock);
    shutdown(fd, SHUT_RDWR);           /* fails a write another thread may be blocked in */
    pthread_mutex_lock(&b->send_lock); /* waits for that write to return */
    close(fd);
    pthread_mutex_unlock(&b->send_lock);
    if (attached && link) oct_link_host_detached(link, reason);
    oct_logf(OCT_LOG_INFO, BRIDGE_COMPONENT, "disconnected", "Bridge client gone: %s", reason);
}

static void *bridge_thread(void *ctx)
{
    oct_bridge *b = ctx;
    pthread_mutex_lock(&b->lock);
    int listener = b->listen_fd, wake_fd = b->wake_read;
    pthread_mutex_unlock(&b->lock);
    while (still_running(b)) {
        io_result ready = oct_bridge_wait_readable(listener, wake_fd, -1);
        if (ready == IO_WOKEN) break;
        if (ready != IO_OK) {
            (void)oct_bridge_wait_readable(wake_fd, -1, oct_bridge_deadline_after(100)); /* don't spin on a failing poll */
            continue;
        }
        int fd = accept(listener, NULL, NULL);
        if (fd < 0) {
            if (errno != EINTR && errno != ECONNABORTED && errno != EAGAIN && errno != EWOULDBLOCK) {
                oct_logf(OCT_LOG_WARN, BRIDGE_COMPONENT, "accept_failed", "Accepting a bridge client failed: %s",
                         strerror(errno));
                (void)oct_bridge_wait_readable(wake_fd, -1, oct_bridge_deadline_after(100)); /* e.g. out of descriptors */
            }
            continue;
        }
        oct_bridge_configure_socket(fd);
        pthread_mutex_lock(&b->lock);
        bool keep = b->running;
        if (keep) b->client_fd = fd;
        pthread_mutex_unlock(&b->lock);
        if (!keep) {
            close(fd);
            break;
        }
        oct_logf(OCT_LOG_INFO, BRIDGE_COMPONENT, "connected", "A bridge client connected");
        drop_client(b, fd, serve_client(b, fd, wake_fd));
    }
    return NULL;
}

/* Creates the listening socket at `path`. A stale socket left there is replaced; anything else is kept. */
static int open_listener(const char *path, int *out_error)
{
    struct stat existing;
    if (lstat(path, &existing) == 0) {
        if (!S_ISSOCK(existing.st_mode)) {
            *out_error = EEXIST;
            return -1;
        }
        unlink(path);
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, strlen(path) + 1); /* fits: checked against SOCK_PATH_MAX */
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        *out_error = errno;
        return -1;
    }
    oct_bridge_configure_socket(fd);
    if (bind(fd, (const struct sockaddr *)&addr, (socklen_t)sizeof addr) != 0 || listen(fd, LISTEN_BACKLOG) != 0) {
        *out_error = errno;
        close(fd);
        unlink(path);
        return -1;
    }
    return fd;
}

static bool open_wake_pipe(int *out_read, int *out_write)
{
    int fds[2];
    if (pipe(fds) != 0) return false;
    for (int i = 0; i < 2; i++) {
        int flags = fcntl(fds[i], F_GETFD);
        if (flags >= 0) (void)fcntl(fds[i], F_SETFD, flags | FD_CLOEXEC);
    }
    int flags = fcntl(fds[1], F_GETFL);
    if (flags >= 0) (void)fcntl(fds[1], F_SETFL, flags | O_NONBLOCK); /* a wake never blocks stop */
    *out_read = fds[0];
    *out_write = fds[1];
    return true;
}

/* oct_link_driver_ops.start: opens the socket and starts the thread. */
static oct_status bridge_driver_start(void *ctx, oct_link *link)
{
    oct_bridge *b = ctx;
    pthread_mutex_lock(&b->lock);
    bool busy = b->running || b->thread_started;
    pthread_mutex_unlock(&b->lock);
    if (busy) return OCT_E_STATE;
    int error = 0;
    int listener = open_listener(b->socket_path, &error);
    if (listener < 0) {
        oct_logf(OCT_LOG_ERROR, BRIDGE_COMPONENT, "listen_failed", "Couldn't listen on %s: %s", b->socket_path,
                 strerror(error));
        return OCT_E_IO;
    }
    int wake_read = -1, wake_write = -1;
    if (!open_wake_pipe(&wake_read, &wake_write)) {
        oct_logf(OCT_LOG_ERROR, BRIDGE_COMPONENT, "pipe_failed", "Couldn't create the bridge's stop pipe: %s",
                 strerror(errno));
        close(listener);
        unlink(b->socket_path);
        return OCT_E_IO;
    }
    pthread_mutex_lock(&b->lock);
    b->link = link;
    b->listen_fd = listener;
    b->wake_read = wake_read;
    b->wake_write = wake_write;
    b->client_fd = -1;
    b->host_attached = false;
    b->running = true;
    b->thread_started = pthread_create(&b->thread, NULL, bridge_thread, b) == 0;
    bool started = b->thread_started;
    if (!started) {
        b->running = false;
        b->link = NULL;
        b->listen_fd = b->wake_read = b->wake_write = -1;
    }
    pthread_mutex_unlock(&b->lock);
    if (!started) {
        close(listener);
        close(wake_read);
        close(wake_write);
        unlink(b->socket_path);
        return OCT_E_IO;
    }
    oct_logf(OCT_LOG_INFO, BRIDGE_COMPONENT, "transport_start", "Bridge transport listening on %s", b->socket_path);
    return OCT_OK;
}

/* oct_link_driver_ops.stop, and oct_bridge_destroy's. Returns once the thread has exited; does nothing when
 * the bridge isn't running. */
static void bridge_driver_stop(void *ctx)
{
    oct_bridge *b = ctx;
    pthread_mutex_lock(&b->lock);
    bool was_running = b->running || b->thread_started;
    b->running = false;
    b->link = NULL;
    if (b->client_fd >= 0) shutdown(b->client_fd, SHUT_RDWR); /* still open: only the thread closes it */
    if (b->wake_write >= 0) {
        uint8_t byte = 1;
        ssize_t ignored = write(b->wake_write, &byte, 1);
        (void)ignored;
    }
    bool join = b->thread_started;
    pthread_t thread = b->thread;
    pthread_mutex_unlock(&b->lock);
    if (join) pthread_join(thread, NULL);
    pthread_mutex_lock(&b->lock);
    b->thread_started = false;
    int listener = b->listen_fd, wake_read = b->wake_read, wake_write = b->wake_write;
    b->listen_fd = b->wake_read = b->wake_write = -1;
    pthread_mutex_unlock(&b->lock);
    if (listener >= 0) {
        close(listener);
        unlink(b->socket_path);
    }
    if (wake_read >= 0) close(wake_read);
    if (wake_write >= 0) close(wake_write);
    if (was_running) oct_logf(OCT_LOG_INFO, BRIDGE_COMPONENT, "stopped", "Bridge transport stopped");
}

static const oct_link_driver_ops k_bridge_link_ops = {
    "bridge-socket",
    bridge_driver_start,
    bridge_driver_stop,
    bridge_driver_send,
};

oct_status oct_bridge_create(const char *path, oct_bridge **out)
{
    if (!out) return OCT_E_INVALID_ARGUMENT;
    *out = NULL;
    if (!path) path = OCT_BRIDGE_DEFAULT_SOCKET;
    size_t length = strlen(path);
    if (length == 0 || length >= SOCK_PATH_MAX) return OCT_E_INVALID_ARGUMENT;
    oct_bridge *b = calloc(1, sizeof *b);
    if (!b) return OCT_E_NO_MEMORY;
    memcpy(b->socket_path, path, length + 1);
    b->listen_fd = b->wake_read = b->wake_write = b->client_fd = -1;
    if (pthread_mutex_init(&b->lock, NULL) != 0) {
        free(b);
        return OCT_E_NO_MEMORY;
    }
    if (pthread_mutex_init(&b->send_lock, NULL) != 0) {
        pthread_mutex_destroy(&b->lock);
        free(b);
        return OCT_E_NO_MEMORY;
    }
    *out = b;
    return OCT_OK;
}

void oct_bridge_destroy(oct_bridge *b)
{
    if (!b) return;
    bridge_driver_stop(b); /* nothing to do when the service already stopped the transport */
    pthread_mutex_destroy(&b->send_lock);
    pthread_mutex_destroy(&b->lock);
    free(b);
}

oct_status oct_bridge_link_create(oct_bridge *b, oct_link **out_link, oct_transport *out_transport)
{
    if (!b || !out_transport) return OCT_E_INVALID_ARGUMENT;
    static const oct_link_config link_config = {
        512,   /* bulk_packet: USB 2.0 high speed */
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

bool oct_bridge_client_connected(const oct_bridge *bridge)
{
    if (!bridge) return false;
    oct_bridge *b = (oct_bridge *)bridge; /* the lock isn't part of the bridge's value */
    pthread_mutex_lock(&b->lock);
    bool connected = b->client_fd >= 0;
    pthread_mutex_unlock(&b->lock);
    return connected;
}

const char *oct_bridge_socket_path(const oct_bridge *b) { return b ? b->socket_path : ""; }

/* ---- Self-test --------------------------------------------------------------------------- */

typedef struct bridge_test {
    oct_compat_report *report;
    oct_bridge_client *client;
    uint32_t           transaction;
} bridge_test;

OCT_LOG_PRINTF(2, 3)
static bool test_fail(bridge_test *t, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(t->report->failure, sizeof t->report->failure, format, args);
    va_end(args);
    return false;
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)le16(p) | ((uint32_t)le16(p + 2) << 16); }

static bool test_command(bridge_test *t, uint16_t operation, const uint32_t *params, uint32_t count)
{
    oct_buf command;
    oct_buf_init(&command, 0);
    oct_container_command(&command, operation, t->transaction, params, count);
    oct_status status = command.failed ? OCT_E_NO_MEMORY
                                       : oct_bridge_client_send_bulk(t->client, command.data, command.length);
    oct_buf_free(&command);
    if (status != OCT_OK) return test_fail(t, "Sending operation 0x%04X failed: %s", operation, oct_status_name(status));
    return true;
}

/* Reads the next container, which must be `type` (data or response) with `code`, in this transaction. The
 * payload after the 12-byte header goes to `out` (freed by the caller) when it isn't NULL. */
static bool test_container(bridge_test *t, uint16_t type, uint16_t code, uint8_t **out, size_t *out_length)
{
    uint8_t frame = 0;
    uint8_t *data = NULL;
    size_t length = 0;
    oct_status status = oct_bridge_client_receive(t->client, &frame, &data, &length, REPLY_TIMEOUT_MS);
    if (status != OCT_OK) {
        return test_fail(t, "Waiting for a container with code 0x%04X: %s", code, oct_status_name(status));
    }
    bool ok = frame == OCT_BRIDGE_MSG_BULK_TO_HOST && length >= 12 && le32(data) == length &&
              le16(data + 4) == type && le16(data + 6) == code && le32(data + 8) == t->transaction;
    if (!ok) {
        test_fail(t, "Expected a %s container with code 0x%04X in transaction %u; got frame 0x%02X, %zu bytes, "
                  "container type %u code 0x%04X", type == OCT_CONTAINER_DATA ? "data" : "response", code,
                  t->transaction, frame, length, length >= 6 ? le16(data + 4) : 0u, length >= 8 ? le16(data + 6) : 0u);
        free(data);
        return false;
    }
    if (out) {
        *out = data;
        *out_length = length;
    } else {
        free(data);
    }
    return true;
}

static bool test_response(bridge_test *t, uint16_t code)
{
    bool ok = test_container(t, OCT_CONTAINER_RESPONSE, code, NULL, NULL);
    t->transaction++;
    return ok;
}

static bool test_session(bridge_test *t)
{
    uint32_t session = 1;
    oct_status status = oct_bridge_client_host_event(t->client, true, NULL);
    if (status != OCT_OK) return test_fail(t, "Reporting a host failed: %s", oct_status_name(status));
    if (!test_command(t, OCT_OP_OPEN_SESSION, &session, 1) || !test_response(t, OCT_RC_OK)) return false;
    t->report->checks++;
    if (!test_command(t, OCT_OP_GET_STORAGE_IDS, NULL, 0)) return false;
    uint8_t *data = NULL;
    size_t length = 0;
    if (!test_container(t, OCT_CONTAINER_DATA, OCT_OP_GET_STORAGE_IDS, &data, &length)) return false;
    uint32_t storages = length >= 16 ? le32(data + 12) : 0;
    free(data);
    if (storages < 1 || length < 16 + 4u * storages) {
        return test_fail(t, "GetStorageIDs listed %u storages in %zu bytes", storages, length);
    }
    t->report->checks++;
    if (!test_response(t, OCT_RC_OK)) return false;
    t->report->checks++;
    return true;
}

static bool test_class_requests(bridge_test *t)
{
    uint8_t reply[8] = { 0 };
    size_t reply_length = 0;
    bool stall = true;
    oct_status status = oct_bridge_client_class_request(t->client, OCT_USB_GET_DEVICE_STATUS, NULL, 0, reply,
                                                        sizeof reply, &reply_length, &stall);
    if (status != OCT_OK || stall || reply_length != 4 || le16(reply + 2) != OCT_RC_OK) {
        return test_fail(t, "Get Device Status: %s, %s, %zu bytes", oct_status_name(status),
                         stall ? "stalled" : "completed", reply_length);
    }
    t->report->checks++;
    stall = false;
    status = oct_bridge_client_class_request(t->client, OCT_USB_GET_EXTENDED_EVENT_DATA, NULL, 0, reply,
                                             sizeof reply, &reply_length, &stall);
    if (status != OCT_OK || !stall) {
        return test_fail(t, "Get Extended Event Data wasn't stalled: %s", oct_status_name(status));
    }
    t->report->checks++;
    return true;
}

static bool test_ping(bridge_test *t, const char *path)
{
    char info[SOCK_PATH_MAX] = "";
    oct_status status = oct_bridge_client_ping(t->client, info, sizeof info, REPLY_TIMEOUT_MS);
    if (status != OCT_OK || strcmp(info, path) != 0) {
        return test_fail(t, "Ping: %s, answered with \"%s\"", oct_status_name(status), info);
    }
    t->report->checks++;
    return true;
}

static bool test_script(bridge_test *t, oct_bridge *bridge, const char *path)
{
    oct_status status = oct_bridge_client_connect(path, REPLY_TIMEOUT_MS, &t->client);
    if (status != OCT_OK) return test_fail(t, "Connecting to %s failed: %s", path, oct_status_name(status));
    t->report->checks++;
    if (!test_ping(t, path) || !test_session(t) || !test_class_requests(t)) return false;
    status = oct_bridge_client_send_frame(t->client, 0xEF, "?", 1);
    if (status != OCT_OK || !test_ping(t, path)) {
        return t->report->failure[0] ? false : test_fail(t, "An unknown frame: %s", oct_status_name(status));
    }
    if (!test_command(t, OCT_OP_CLOSE_SESSION, NULL, 0) || !test_response(t, OCT_RC_OK)) return false;
    t->report->checks++;
    oct_bridge_client_destroy(t->client); /* leaves without a detach */
    t->client = NULL;
    int64_t deadline = oct_bridge_deadline_after(REPLY_TIMEOUT_MS);
    while (oct_bridge_client_connected(bridge) && oct_bridge_monotonic_ms() < deadline) {
        struct timespec pause = { 0, 5 * 1000000L };
        nanosleep(&pause, NULL);
    }
    if (oct_bridge_client_connected(bridge)) return test_fail(t, "The server didn't notice the client leave");
    t->report->checks++;
    status = oct_bridge_client_connect(path, REPLY_TIMEOUT_MS, &t->client);
    if (status != OCT_OK) return test_fail(t, "Reconnecting failed: %s", oct_status_name(status));
    return test_ping(t, path);
}

oct_status oct_bridge_self_test(const char *directory, oct_compat_report *report)
{
    if (!report) return OCT_E_INVALID_ARGUMENT;
    memset(report, 0, sizeof *report);
    static const char *const k_directories[] = { "/tmp", "/var/tmp", "/var/mobile/Library/Caches/Octopus" };
    static atomic_uint sequence;
    const char *const *candidates = directory ? &directory : k_directories;
    size_t candidate_count = directory ? 1 : sizeof k_directories / sizeof k_directories[0];
    char path[SOCK_PATH_MAX] = "";
    char refused[OCT_EVENT_TEXT_MAX - 40] = ""; /* leaves room for the sentence around it */
    size_t used = 0;
    const char *folder = NULL;
    for (size_t i = 0; i < candidate_count && !folder; i++) {
        unsigned number = atomic_fetch_add(&sequence, 1u);
        int written = snprintf(path, sizeof path, "%s/octopus-bridge-test-%ld-%u.sock", candidates[i],
                               (long)getpid(), number);
        int error = ENAMETOOLONG;
        if (written > 0 && (size_t)written < sizeof path) {
            int fd = open_listener(path, &error);
            if (fd >= 0) {
                close(fd);
                unlink(path);
                folder = candidates[i];
                break;
            }
        }
        int added = snprintf(refused + used, sizeof refused - used, "%s%s: %s", used ? "; " : "", candidates[i],
                             strerror(error));
        if (added > 0) used = used + (size_t)added < sizeof refused ? used + (size_t)added : sizeof refused - 1;
    }
    if (!folder) {
        snprintf(report->failure, sizeof report->failure, "No folder takes a bridge socket (%s)", refused);
        return OCT_E_IO;
    }
    bridge_test t = { report, NULL, 0 };
    oct_service *service = NULL;
    oct_bridge *bridge = NULL;
    oct_link *link = NULL;
    oct_transport transport;
    oct_status status = oct_service_create(&service);
    if (status == OCT_OK) status = oct_bridge_create(path, &bridge);
    if (status == OCT_OK) status = oct_bridge_link_create(bridge, &link, &transport);
    if (status == OCT_OK) {
        oct_service_set_log_component(service, NULL); /* only the bridge's own lines */
        status = oct_service_set_transport(service, &transport);
    }
    oct_service_config config = { 0, false, false };
    if (status == OCT_OK) status = oct_service_start(service, &config);
    if (status == OCT_OK) {
        report->checks++;
        if (!test_script(&t, bridge, path)) status = OCT_E_IO;
    } else {
        snprintf(report->failure, sizeof report->failure, "Starting the bridge transport on %s failed: %s", path,
                 oct_status_name(status));
    }
    oct_bridge_client_destroy(t.client);
    if (service) oct_service_stop(service);
    struct stat left;
    if (status == OCT_OK) {
        if (lstat(path, &left) == 0) {
            status = OCT_E_IO;
            test_fail(&t, "The socket %s was still there after the stop", path);
        } else {
            report->checks++;
        }
    }
    oct_service_destroy(service);
    oct_bridge_destroy(bridge);
    oct_link_destroy(link);
    if (status == OCT_OK) {
        snprintf(report->summary, sizeof report->summary,
                 "%u checks on a socket in %s: ping, session, storage IDs, class requests, an unknown frame, "
                 "a reconnect, the stop", report->checks, folder);
    } else if (!report->failure[0]) {
        snprintf(report->failure, sizeof report->failure, "Bridge self-test: %s", oct_status_name(status));
    }
    return status;
}
