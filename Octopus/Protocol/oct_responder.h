/*
 * Transport-independent PTP/MTP responder over an oct_store.
 *
 * Feed it one complete host-to-device container at a time; it appends the
 * device-to-host containers (an optional data container, then the response)
 * to `out`. Operations with a host data phase (SendObjectInfo, SendObject,
 * SetObjectPropValue) produce no output until their data container arrives.
 * Data phases are buffered and limited to OCT_PTP_MAX_TRANSFER bytes.
 */
#ifndef OCT_RESPONDER_H
#define OCT_RESPONDER_H

#include "oct_ptp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct oct_responder oct_responder;

typedef struct oct_responder_identity {
    const char *manufacturer;    /* NULL selects "Octopus" */
    const char *model;           /* NULL selects "Octopus File Provider" */
    const char *device_version;  /* NULL selects OCT_CORE_VERSION */
    const char *serial_number;   /* NULL selects "0" */
    const char *friendly_name;   /* NULL selects the model */
} oct_responder_identity;

typedef struct oct_responder_stats {
    uint64_t commands;        /* command containers dispatched */
    uint64_t failures;        /* responses other than OK */
    uint64_t malformed;       /* containers that could not be parsed */
    uint64_t abandoned;       /* expected data phases the host never sent */
    uint64_t bytes_in;
    uint64_t bytes_out;
    uint16_t last_operation;
    uint16_t last_response;
} oct_responder_stats;

/* The store is borrowed and must outlive the responder. */
oct_status oct_responder_create(oct_store *store, const oct_responder_identity *identity,
                                oct_responder **out);
void       oct_responder_destroy(oct_responder *responder);

/* Returns OCT_OK when the container was processed, OCT_E_MALFORMED when it
 * could not be parsed (a GeneralError response is still appended when the
 * command header was readable), OCT_E_STATE for a data container nobody
 * expected (nothing appended), or OCT_E_NO_MEMORY. */
/* The PTP response code for a store status, so a caller outside the responder answers a host with
 * the same code the responder would. Shared rather than copied: a second mapping would drift. */
uint16_t oct_response_for_status(oct_status status);

oct_status oct_responder_handle(oct_responder *responder, const uint8_t *bytes, size_t length,
                                oct_buf *out);
/* Ends the session as a transport reset or disconnect would. */
void       oct_responder_reset(oct_responder *responder);
bool       oct_responder_session(const oct_responder *responder, uint32_t *session_id);
bool       oct_responder_awaiting_data(const oct_responder *responder);
void       oct_responder_get_stats(const oct_responder *responder, oct_responder_stats *out);
const uint16_t *oct_responder_operations(size_t *count);

/* Loopback initiator: sends one transaction (and an optional host data phase)
 * and decodes the reply. Rejects replies whose framing is wrong, so it doubles
 * as a conformance check of the responder's output. */
typedef struct oct_exchange {
    uint16_t       response_code;
    uint32_t       params[OCT_PTP_MAX_PARAMS];
    uint32_t       param_count;
    bool           has_data;
    const uint8_t *data;         /* points into `scratch` */
    size_t         data_length;
} oct_exchange;

oct_status oct_exchange_run(oct_responder *responder, uint16_t operation, uint32_t transaction_id,
                            const uint32_t *params, uint32_t param_count, bool send_data,
                            const void *data, size_t data_length, oct_buf *scratch,
                            oct_exchange *result);

#ifdef __cplusplus
}
#endif

#endif /* OCT_RESPONDER_H */
