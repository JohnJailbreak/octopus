/*
 * Octopus service: owns the storage and responder and runs their lifecycle as
 * an explicit state machine. The app talks only to this interface. A live USB
 * transport plugs in through oct_transport without changing it; this build
 * has none, so the service stops at Ready and never claims a connection.
 *
 * All functions are thread-safe. Transport callbacks must not call back into
 * the service synchronously from start().
 */
#ifndef OCT_SERVICE_H
#define OCT_SERVICE_H

#include "../Protocol/oct_responder.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum oct_service_state {
    OCT_SERVICE_DISABLED = 0,
    OCT_SERVICE_STARTING = 1,
    OCT_SERVICE_READY = 2,      /* provider and responder running, no transport */
    OCT_SERVICE_WAITING = 3,    /* transport listening for a host */
    OCT_SERVICE_CONNECTED = 4,  /* a host is attached to the transport */
    OCT_SERVICE_ERROR = 5
} oct_service_state;

typedef enum oct_event_kind {
    OCT_EVENT_STATE = 1,
    OCT_EVENT_OPERATION_BEGIN = 2, /* text: activity-sheet title */
    OCT_EVENT_STEP_BEGIN = 3,      /* text: what is running */
    OCT_EVENT_STEP_END = 4,        /* text: the result */
    OCT_EVENT_OPERATION_END = 5,   /* text: final summary line */
    OCT_EVENT_LOG = 6              /* text: activity-log line */
} oct_event_kind;

#define OCT_EVENT_TEXT_MAX 160

typedef struct oct_event {
    uint32_t          sequence;
    oct_event_kind    kind;
    oct_service_state state;      /* state when the event was recorded */
    uint32_t          step;       /* step or operation identifier */
    bool              failed;
    int64_t           time_ms;    /* milliseconds since 1970 */
    char              text[OCT_EVENT_TEXT_MAX];
} oct_event;

typedef struct oct_service_config {
    int64_t seed_time;  /* timestamp of the synthetic objects; 0 means now */
    bool    scratch;    /* add the writable Scratch folder */
    bool    self_test;  /* run the protocol self-test while starting */
} oct_service_config;

typedef struct oct_service oct_service;

typedef struct oct_transport_ops {
    const char *name;
    oct_status (*start)(void *context, oct_service *service);
    void       (*stop)(void *context);
} oct_transport_ops;

typedef struct oct_transport {
    const oct_transport_ops *ops;
    void                    *context;
} oct_transport;

typedef struct oct_service_status {
    oct_service_state state;
    bool              transport_available;
    bool              session_open;
    bool              scratch;
    uint32_t          session_id;
    uint32_t          storage_id;
    uint32_t          object_count;
    uint32_t          operation_count;
    uint64_t          transactions;
    uint32_t          starts;
    uint32_t          stops;
    uint32_t          dropped_events;
    char              transport_name[32];
} oct_service_status;

const char *oct_service_state_name(oct_service_state state);
const char *oct_event_text(const oct_event *event);
const char *oct_object_info_name(const oct_object_info *info);

oct_status oct_service_create(oct_service **out);
void       oct_service_destroy(oct_service *service);
oct_status oct_service_set_identity(oct_service *service, const char *device_name,
                                    const char *serial_number, const char *system_version);
/* Allowed only while disabled or in error. NULL removes the transport. */
oct_status oct_service_set_transport(oct_service *service, const oct_transport *transport);

/* start/stop are idempotent: starting a running service or stopping a
 * disabled one succeeds without side effects. */
oct_status oct_service_start(oct_service *service, const oct_service_config *config);
oct_status oct_service_stop(oct_service *service);
/* Runs the recorded PTP/MTP self-test with one activity step per check
 * group: against the live storage when running, otherwise against a
 * temporary synthetic storage with a Scratch folder. */
oct_status oct_service_self_test(oct_service *service);

oct_service_state oct_service_get_state(const oct_service *service);
oct_status oct_service_get_status(oct_service *service, oct_service_status *out);
bool       oct_service_last_error(oct_service *service, char *buffer, size_t capacity);
size_t     oct_service_drain_events(oct_service *service, oct_event *events, size_t capacity);

/* Storage inspection; OCT_E_STATE unless the service is running. `count`
 * receives the total number of children even when it exceeds `capacity`. */
oct_status oct_service_storage_info(oct_service *service, oct_storage_info *out);
oct_status oct_service_list(oct_service *service, uint32_t parent, oct_object_info *items,
                            size_t capacity, size_t *count);
oct_status oct_service_read(oct_service *service, uint32_t handle, uint64_t offset, void *buffer,
                            size_t length, size_t *read);

/* Transport side. */
oct_status oct_service_host_attached(oct_service *service);
oct_status oct_service_host_detached(oct_service *service);
oct_status oct_service_receive(oct_service *service, const uint8_t *bytes, size_t length, oct_buf *out);

#ifdef __cplusplus
}
#endif

#endif /* OCT_SERVICE_H */
