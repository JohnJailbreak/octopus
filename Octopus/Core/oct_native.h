/* Native ptpd observation journal. No transport, hooks, filesystem access or
 * authorization UI. The caller serializes every call, supplies a fresh nonzero
 * process generation, and enables recording only after explicit user consent.
 * A paired response means Apple's sendResponse returned YES, NOT USB completion.
 * The receive scope must surround the actual handleBulkData:result: completion, not a
 * generic handleRequest call which may originate from PTP/IP or a simulation. */
#ifndef OCT_NATIVE_H
#define OCT_NATIVE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define OCT_NATIVE_EVENTS 64u
#define OCT_NATIVE_PENDING 16u
#define OCT_NATIVE_SCOPES 8u
#define OCT_NATIVE_TOMBSTONES 64u

typedef enum oct_native_event_kind {
    OCT_NATIVE_AUTHORIZATION = 1,
    OCT_NATIVE_CONNECTION,
    OCT_NATIVE_USB_READ,
    OCT_NATIVE_COMMAND,
    OCT_NATIVE_DATA_SUBMITTED,
    OCT_NATIVE_RESPONSE,
    OCT_NATIVE_REJECTED,
    OCT_NATIVE_OVERFLOW
} oct_native_event_kind;

typedef struct oct_native_event {
    uint64_t sequence;
    int64_t utc_ms; /* zero if the platform clock failed */
    uint64_t generation;
    uint64_t epoch;
    uint64_t transport;
    uint64_t scope;
    uint64_t bytes;
    uint32_t transaction;
    uint16_t operation;
    uint16_t response;
    int32_t result;
    oct_native_event_kind kind;
} oct_native_event;

/*
 * The write operations a host sent and what ptpd answered, across every command, not only the last
 * sixty-four the event ring holds. It answers the question the hook report cannot: whether a host
 * that was offered uploads ever tried one. If it never sent SendObjectInfo, it decided from what the
 * device reported not to offer adding a file; if it sent one and ptpd refused, the refusal happened
 * inside Apple's responder before Octopus was asked. Those need opposite fixes.
 */
enum {
    OCT_NATIVE_TRAFFIC_SEND_OBJECT_INFO = 1u << 0, /* 0x100C */
    OCT_NATIVE_TRAFFIC_SEND_OBJECT      = 1u << 1, /* 0x100D */
    OCT_NATIVE_TRAFFIC_DELETE_OBJECT    = 1u << 2, /* 0x100B */
    OCT_NATIVE_TRAFFIC_SET_PROP_VALUE   = 1u << 3, /* 0x9804, an MTP rename */
    OCT_NATIVE_TRAFFIC_SEND_PROP_LIST   = 1u << 4, /* 0x9808, an MTP upload */
    OCT_NATIVE_TRAFFIC_PROPS_SUPPORTED  = 1u << 5, /* 0x9801, a host probing for MTP */
    OCT_NATIVE_TRAFFIC_MOVE_OBJECT      = 1u << 6, /* 0x1019 */
    OCT_NATIVE_TRAFFIC_COPY_OBJECT      = 1u << 7, /* 0x101A */
};

/* How the companion told a connected host the device changed, so an export shows whether a phone
 * plugged in before USB access was enabled was ever told. Set by the observer, not the journal. */
enum {
    OCT_NATIVE_ANNOUNCE_CAMERA      = 1u << 0, /* through Apple's sendEventAddStorageWithID: */
    OCT_NATIVE_ANNOUNCE_EVENT       = 1u << 1, /* through a StoreAdded event on the transport */
    OCT_NATIVE_ANNOUNCE_DEVICE_INFO = 1u << 2, /* the transport confirmed it queued DeviceInfoChanged */
    OCT_NATIVE_ANNOUNCE_FAILED      = 1u << 3, /* an event could not be sent: no session or transport */
};

typedef struct oct_native_traffic {
    uint8_t  announced;            /* OCT_NATIVE_ANNOUNCE_* bits, four of them */
    uint8_t  seen;                 /* OCT_NATIVE_TRAFFIC_* bits */
    uint8_t  send_info_count;      /* SendObjectInfo commands, saturating */
    uint16_t send_info_response;   /* the last response to SendObjectInfo, zero if none */
    uint16_t send_object_response; /* the last response to SendObject */
    uint16_t delete_response;      /* the last response to DeleteObject */
} oct_native_traffic;

typedef struct oct_native_snapshot {
    oct_native_traffic traffic;
    uint64_t generation;
    uint64_t epoch;
    uint64_t transport;
    uint64_t sequence;
    uint64_t usb_reads;
    uint64_t failed_reads;
    uint64_t commands;
    uint64_t data_submissions;
    uint64_t response_calls;
    uint64_t paired_responses; /* successful native BOOL return, NOT bus completion */
    uint64_t failed_responses;
    uint64_t unmatched_responses;
    uint64_t rejected_commands;
    uint64_t dropped_pending;
    uint64_t overwritten_events;
    bool authorized;
    bool connected;
    bool pairing_disabled; /* ambiguity history overflow: needs a fresh generation */
    size_t event_count;
    oct_native_event events[OCT_NATIVE_EVENTS]; /* oldest to newest */
} oct_native_snapshot;

typedef struct oct_native_pending {
    uint64_t command_sequence;
    uint64_t epoch;
    uint32_t transaction;
    uint16_t operation;
    bool active;
} oct_native_pending;

typedef struct oct_native_scope {
    uint64_t id;
    uint64_t epoch;
    bool active;
} oct_native_scope;

typedef struct oct_native_tombstone {
    uint64_t transport;
    uint32_t transaction;
    bool active;
} oct_native_tombstone;

typedef struct oct_native_journal {
    oct_native_snapshot state;
    oct_native_pending pending[OCT_NATIVE_PENDING];
    oct_native_scope scopes[OCT_NATIVE_SCOPES];
    oct_native_tombstone tombstones[OCT_NATIVE_TOMBSTONES];
} oct_native_journal;

void oct_native_init(oct_native_journal *journal, uint64_t generation);
void oct_native_authorize(oct_native_journal *journal, bool authorized);
void oct_native_connect(oct_native_journal *journal, uint64_t transport, bool connected);
uint64_t oct_native_receive_begin(oct_native_journal *journal, uint64_t transport,
                                  int32_t io_result, uint32_t byte_count);
void oct_native_receive_end(oct_native_journal *journal, uint64_t scope);
bool oct_native_command(oct_native_journal *journal, uint64_t scope, uint16_t operation, uint32_t transaction);
/* The traffic bit for a write operation, or zero for one that is not. */
unsigned oct_native_traffic_bit(uint16_t operation);
bool oct_native_data_submitted(oct_native_journal *journal, uint64_t transport, uint32_t transaction, uint64_t bytes);
bool oct_native_response(oct_native_journal *journal, uint64_t transport, uint32_t transaction,
                         uint16_t response, bool accepted);
void oct_native_get_snapshot(const oct_native_journal *journal, oct_native_snapshot *out);
#ifdef __cplusplus
}
#endif
#endif
