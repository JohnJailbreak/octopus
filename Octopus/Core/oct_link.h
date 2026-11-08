/*
 * oct_link: the transport adapter between a byte-level driver and the service.
 *
 * A driver moves bytes: whatever the host sends on its bulk OUT pipe arrives through
 * oct_link_input in pieces of any size, and the link hands back whole transfers for the
 * bulk IN and interrupt IN pipes. The link does the rest, as the USB Still Image Capture
 * Device Definition 1.0 lays out for PTP over USB:
 *
 *   - reassembles containers from fragmented input, by their declared length;
 *   - passes each complete container to the service, which runs the real responder;
 *   - sends the data container and the response container as separate transfers, and
 *     says when a transfer is a multiple of the packet size so the driver ends it with a
 *     zero-length packet;
 *   - tracks the session and the transaction in progress;
 *   - handles disconnects, stalled input (a timeout), and the class requests Cancel,
 *     Device Reset and Get Device Status;
 *   - reports its state and the last meaningful failure, and logs every transaction.
 *
 * It plugs into the service as an oct_transport (oct_link_transport), so the service
 * starts and stops it. Lock order is service, then link: the link never calls into the
 * service while holding its own lock.
 *
 * Driver side threading: input, tick and attach/detach come from one thread (the
 * driver's reader); cancel, device status and reset may come from another (a control
 * pipe). Driver start is called with the service locked, so it must not call any link
 * function before returning.
 */
#ifndef OCT_LINK_H
#define OCT_LINK_H

#include "oct_service.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OCT_LINK_DEFAULT_PACKET  512u   /* USB 2.0 high-speed bulk */
#define OCT_LINK_DEFAULT_TIMEOUT 5000u  /* ms without input before a partial container is dropped */

/* PTP response codes used by Get Device Status (PIMA 15740 table 27). */
#define OCT_RC_DEVICE_BUSY           0x2019u
#define OCT_RC_TRANSACTION_CANCELLED 0x201Fu

typedef struct oct_link oct_link;

typedef enum oct_link_pipe {
    OCT_LINK_BULK_IN = 1,
    OCT_LINK_INTERRUPT_IN = 2
} oct_link_pipe;

typedef struct oct_link_driver_ops {
    const char *name;
    oct_status (*start)(void *context, oct_link *link);
    void       (*stop)(void *context);
    /* One whole transfer. `zero_length_packet` is true when `length` is a multiple of the
     * packet size, so a USB driver must end the transfer with a zero-length packet. */
    oct_status (*send)(void *context, oct_link_pipe pipe, const uint8_t *bytes, size_t length,
                       bool zero_length_packet);
} oct_link_driver_ops;

typedef struct oct_link_driver {
    const oct_link_driver_ops *ops;
    void                      *context;
} oct_link_driver;

typedef struct oct_link_config {
    uint32_t    max_packet;     /* 0 selects OCT_LINK_DEFAULT_PACKET */
    uint32_t    timeout_ms;     /* 0 selects OCT_LINK_DEFAULT_TIMEOUT */
    const char *log_component;  /* NULL selects "link" */
    bool        rehearsal;      /* the self-test provokes faults on purpose: log everything as DEBUG */
} oct_link_config;

typedef struct oct_link_status {
    oct_transport_state state;
    bool     host_attached;
    bool     session_open;
    uint32_t session_id;
    uint32_t last_transaction;
    uint16_t last_operation;
    uint16_t last_response;
    uint16_t device_status;     /* what Get Device Status reports now */
    uint64_t transactions;      /* commands answered */
    uint64_t containers_in;
    uint64_t bytes_in;
    uint64_t bytes_out;
    uint64_t malformed;         /* unframeable or rejected input */
    uint64_t timeouts;          /* partial containers dropped */
    uint64_t cancels;
    uint64_t resets;
    uint64_t connects;
    uint64_t disconnects;
    uint64_t send_failures;
    uint64_t last_duration_us;  /* the last transaction, command to response */
    char     driver[32];
    char     last_failure[OCT_EVENT_TEXT_MAX];
} oct_link_status;

oct_status  oct_link_create(const oct_link_driver *driver, const oct_link_config *config, oct_link **out);
void        oct_link_destroy(oct_link *link);
/* The transport to give oct_service_set_transport. */
void        oct_link_transport(oct_link *link, oct_transport *out);
void        oct_link_get_status(oct_link *link, oct_link_status *out);
int64_t     oct_link_now_ms(void); /* a monotonic clock for drivers */

/* Driver side. */
oct_status  oct_link_host_attached(oct_link *link);
oct_status  oct_link_host_detached(oct_link *link, const char *reason);
oct_status  oct_link_input(oct_link *link, const uint8_t *bytes, size_t length, int64_t now_ms);
oct_status  oct_link_tick(oct_link *link, int64_t now_ms);
oct_status  oct_link_cancel(oct_link *link, uint32_t transaction_id); /* class request 0x64 */
oct_status  oct_link_device_reset(oct_link *link);                    /* class request 0x66 */
uint16_t    oct_link_device_status(oct_link *link);                   /* class request 0x67 */
void        oct_link_fail(oct_link *link, const char *reason);

/* A loopback driver: transfers the device sends are queued in memory for a test host. */
typedef struct oct_loopback oct_loopback;

oct_status  oct_loopback_create(oct_loopback **out);
void        oct_loopback_destroy(oct_loopback *loopback);
void        oct_loopback_driver(oct_loopback *loopback, oct_link_driver *out);
void        oct_loopback_set_failures(oct_loopback *loopback, bool fail_start, bool fail_send);
size_t      oct_loopback_pending(oct_loopback *loopback);
/* Takes the oldest queued transfer; false when there is none. */
bool        oct_loopback_next(oct_loopback *loopback, oct_link_pipe *pipe, oct_buf *out, bool *zero_length_packet);

/* A test host on a loopback link: sends one command (and an optional data phase), each
 * cut into `chunk`-byte pieces, then reads and checks the reply transfers: one container
 * per transfer, the data container before the response, matching transaction IDs, and
 * the zero-length-packet flag. */
typedef struct oct_link_reply {
    uint16_t response;
    uint32_t params[OCT_PTP_MAX_PARAMS];
    uint32_t param_count;
    bool     has_data;
    oct_buf  data;          /* the data container's payload; the caller frees it */
    size_t   transfers;
} oct_link_reply;

oct_status  oct_link_transact(oct_link *link, oct_loopback *loopback, uint16_t operation, uint32_t transaction_id,
                              const uint32_t *params, uint32_t param_count, const void *data, size_t data_length,
                              size_t chunk, int64_t now_ms, oct_link_reply *reply);

/* The adapter's end-to-end check, run by the app's self-test: a temporary service with a
 * synthetic storage, driven through a loopback link with fragmented input, a malformed
 * container, a stall, a cancel, a reset, repeated sessions and a reconnect. */
oct_status  oct_link_self_test(uint32_t *checks, char *failure, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif /* OCT_LINK_H */
