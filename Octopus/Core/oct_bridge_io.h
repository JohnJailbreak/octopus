/*
 * oct_bridge_io.h — the bridge's framing and socket primitives, shared by its two halves.
 *
 * Internal to the bridge: oct_bridge.c (the server, which the app runs) and oct_bridge_client.c
 * (the client, which the ptpd integration links). Nothing outside the bridge includes this.
 *
 * The split exists so the ptpd integration can link the client without the app's storage engine:
 * oct_bridge_io.c and oct_bridge_client.c depend on libc and oct_log only.
 */
#ifndef OCT_BRIDGE_IO_H
#define OCT_BRIDGE_IO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SOCK_PATH_MAX      104   /* sun_path is 104 bytes on Darwin, 108 on Linux */
#define FRAME_TIMEOUT_MS   5000  /* for the rest of a frame once its header has arrived */
#define SEND_TIMEOUT_S     5     /* a peer that stops reading fails the write instead of blocking it */
#define REPLY_TIMEOUT_MS   2000

/* How a read ended. */
typedef enum io_result {
    IO_OK = 0,
    IO_TIMEOUT,  /* nothing arrived before the deadline */
    IO_CLOSED,   /* the peer closed or reset the connection */
    IO_ERROR,    /* a failed call or a malformed frame */
    IO_WOKEN,    /* the server is stopping */
} io_result;

int64_t    oct_bridge_monotonic_ms(void);
int64_t    oct_bridge_deadline_after(uint32_t timeout_ms);
/* Close-on-exec, no SIGPIPE, and a send timeout. */
void       oct_bridge_configure_socket(int fd);
/* Waits until `fd` is readable or hung up, `wake_fd` is readable, or the deadline passes.
 * A negative deadline waits indefinitely; a negative wake_fd isn't watched. */
io_result  oct_bridge_wait_readable(int fd, int wake_fd, int64_t deadline);
/* Writes a whole frame. Small frames go in one call. */
bool       oct_bridge_send_frame(int fd, uint8_t type, const void *payload, size_t length);
/* Reads one frame. The header may take until `deadline`; the payload must then follow within
 * FRAME_TIMEOUT_MS, or the frame is malformed. The payload (NULL when empty) is the caller's to free. */
io_result  oct_bridge_recv_frame(int fd, int wake_fd, int64_t deadline, uint8_t *out_type,
                                 uint8_t **out_payload, size_t *out_length);

#endif /* OCT_BRIDGE_IO_H */
