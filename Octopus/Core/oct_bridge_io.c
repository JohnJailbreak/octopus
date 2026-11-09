/* oct_bridge_io.c - the bridge's framing and socket primitives. See oct_bridge_io.h. */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#elif !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "oct_bridge_io.h"
#include "oct_bridge.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#if defined(MSG_NOSIGNAL) && !defined(__APPLE__)
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0 /* Darwin: SO_NOSIGPIPE, set on every socket by oct_bridge_configure_socket */
#endif
#if defined(__APPLE__) && !defined(SO_NOSIGPIPE)
#error "SO_NOSIGPIPE is needed: without it a peer that leaves would end the process with SIGPIPE"
#endif

/* ---- Wire protocol ----------------------------------------------------------------------- */

static void encode_header(uint8_t *header, uint8_t type, size_t length)
{
    uint32_t l = (uint32_t)length;
    header[0] = type;
    header[1] = (uint8_t)(l & 0xFFu);
    header[2] = (uint8_t)((l >> 8) & 0xFFu);
    header[3] = (uint8_t)((l >> 16) & 0xFFu);
    header[4] = (uint8_t)((l >> 24) & 0xFFu);
}

size_t oct_bridge_encode(uint8_t type, const void *payload, size_t length, uint8_t *buf, size_t capacity)
{
    if (!buf || length > OCT_BRIDGE_MAX_PAYLOAD || (length && !payload)) return 0;
    if (capacity < OCT_BRIDGE_HEADER_SIZE + length) return 0;
    encode_header(buf, type, length);
    if (length) memcpy(buf + OCT_BRIDGE_HEADER_SIZE, payload, length);
    return OCT_BRIDGE_HEADER_SIZE + length;
}

uint8_t *oct_bridge_alloc(uint8_t type, const void *payload, size_t length, size_t *out_length)
{
    if (length > OCT_BRIDGE_MAX_PAYLOAD) return NULL;
    size_t total = OCT_BRIDGE_HEADER_SIZE + length;
    uint8_t *buf = malloc(total);
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

int64_t oct_bridge_monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + (int64_t)(now.tv_nsec / 1000000);
}

int64_t oct_bridge_deadline_after(uint32_t timeout_ms)
{
    return timeout_ms ? oct_bridge_monotonic_ms() + timeout_ms : -1;
}

/* Close-on-exec (the update helper is spawned), no SIGPIPE, and a send timeout. */
void oct_bridge_configure_socket(int fd)
{
    int flags = fcntl(fd, F_GETFD);
    if (flags >= 0) (void)fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
#if defined(SO_NOSIGPIPE)
    int one = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, (socklen_t)sizeof one);
#endif
    struct timeval timeout = { SEND_TIMEOUT_S, 0 };
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, (socklen_t)sizeof timeout);
}

/* Waits until `fd` is readable (or hung up), `wake_fd` is readable, or the deadline passes. A negative deadline
 * waits indefinitely; a negative wake_fd isn't watched. */
io_result oct_bridge_wait_readable(int fd, int wake_fd, int64_t deadline)
{
    for (;;) {
        int timeout = -1;
        if (deadline >= 0) {
            int64_t left = deadline - oct_bridge_monotonic_ms();
            if (left <= 0) return IO_TIMEOUT;
            timeout = left > INT_MAX ? INT_MAX : (int)left;
        }
        struct pollfd fds[2] = { { fd, POLLIN, 0 }, { wake_fd, POLLIN, 0 } };
        nfds_t count = (nfds_t)(wake_fd >= 0 ? 2 : 1);
        int ready = poll(fds, count, timeout);
        if (ready < 0) {
            if (errno == EINTR) continue;
            return IO_ERROR;
        }
        if (ready == 0) continue; /* the deadline check above ends the wait */
        if (count == 2 && fds[1].revents) return IO_WOKEN;
        if (fds[0].revents & POLLNVAL) return IO_ERROR;
        if (fds[0].revents) return IO_OK; /* readable, or hung up: the read says which */
    }
}

static io_result read_exact(int fd, int wake_fd, uint8_t *out, size_t length, int64_t deadline)
{
    size_t got = 0;
    while (got < length) {
        io_result ready = oct_bridge_wait_readable(fd, wake_fd, deadline);
        if (ready != IO_OK) return ready;
        ssize_t n = recv(fd, out + got, length - got, 0);
        if (n == 0) return IO_CLOSED;
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return errno == ECONNRESET ? IO_CLOSED : IO_ERROR;
        }
        got += (size_t)n;
    }
    return IO_OK;
}

static bool write_exact(int fd, const uint8_t *bytes, size_t length)
{
    while (length > 0) {
        ssize_t n = send(fd, bytes, length, SEND_FLAGS);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false; /* EPIPE, ECONNRESET, or EAGAIN after the send timeout */
        }
        bytes += (size_t)n;
        length -= (size_t)n;
    }
    return true;
}

/* Writes a frame with one call where the socket takes it all (small frames always), finishing any rest. */
bool oct_bridge_send_frame(int fd, uint8_t type, const void *payload, size_t length)
{
    if (length > OCT_BRIDGE_MAX_PAYLOAD || (length && !payload)) return false;
    uint8_t header[OCT_BRIDGE_HEADER_SIZE];
    encode_header(header, type, length);
    struct iovec parts[2] = { { header, sizeof header }, { (void *)payload, length } };
    struct msghdr message;
    memset(&message, 0, sizeof message);
    message.msg_iov = parts;
    if (length) message.msg_iovlen = 2;
    else message.msg_iovlen = 1;
    ssize_t n;
    do {
        n = sendmsg(fd, &message, SEND_FLAGS);
    } while (n < 0 && errno == EINTR);
    if (n < 0) return false;
    size_t sent = (size_t)n;
    if (sent < sizeof header) {
        return write_exact(fd, header + sent, sizeof header - sent) && (!length || write_exact(fd, payload, length));
    }
    sent -= sizeof header;
    return sent >= length || write_exact(fd, (const uint8_t *)payload + sent, length - sent);
}

/* Reads one frame. The header may take until `deadline`; once it has arrived the payload must follow within
 * FRAME_TIMEOUT_MS, or the frame is malformed. The payload (NULL when empty) is the caller's to free. */
io_result oct_bridge_recv_frame(int fd, int wake_fd, int64_t deadline, uint8_t *out_type,
                                uint8_t **out_payload, size_t *out_length)
{
    *out_payload = NULL;
    *out_length = 0;
    uint8_t header[OCT_BRIDGE_HEADER_SIZE];
    io_result result = read_exact(fd, wake_fd, header, sizeof header, deadline);
    if (result != IO_OK) return result;
    uint32_t length = (uint32_t)header[1] | ((uint32_t)header[2] << 8) | ((uint32_t)header[3] << 16) |
                      ((uint32_t)header[4] << 24);
    if (length > OCT_BRIDGE_MAX_PAYLOAD) return IO_ERROR;
    uint8_t *payload = NULL;
    if (length) {
        payload = malloc(length);
        if (!payload) return IO_ERROR;
        result = read_exact(fd, wake_fd, payload, length, oct_bridge_deadline_after(FRAME_TIMEOUT_MS));
        if (result != IO_OK) {
            free(payload);
            return result == IO_TIMEOUT ? IO_ERROR : result; /* a frame cut short is malformed */
        }
    }
    *out_type = header[0];
    *out_payload = payload;
    *out_length = length;
    return IO_OK;
}

