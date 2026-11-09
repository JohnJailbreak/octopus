/*
 * oct_bridge: a Unix-domain-socket transport that connects the Octopus responder to an external
 * integration component, without USB I/O code in the app process.
 *
 * The Octopus app is the server. The integration client is the ptpd bridge tweak (or the
 * diagnostic probe). The wire protocol is a simple framing layer:
 *
 *     [1 byte type][4 bytes payload length, LE][payload bytes]
 *
 * The client represents the "USB side" (it forwards bytes from Windows over the PTP bulk
 * endpoint and sends back the device's bulk responses). The server represents the responder.
 *
 * Message types
 * ─────────────
 *   0x01  HOST_ATTACHED    client → server: a host has opened the PTP interface
 *   0x02  HOST_DETACHED    client → server: host disconnected; payload is the reason string
 *   0x03  BULK_FROM_HOST   client → server: bytes from the host (a PTP command or data phase)
 *   0x04  BULK_TO_HOST     server → client: bytes to send to the host (a PTP data or response)
 *   0x05  INTERRUPT_EVENT  server → client: an event container for the interrupt IN pipe
 *   0x06  BUS_RESET        client → server: a USB bus reset
 *   0x07  SUSPEND          client → server: USB bus suspended
 *   0x08  RESUME           client → server: USB bus resumed
 *   0x09  CLASS_REQUEST    client → server: a class request; byte 0 is the bRequest code,
 *                            bytes 1–N are the OUT data stage (empty for device→host requests)
 *   0x0A  CLASS_RESPONSE   server → client: class-request reply (4 bytes for GetDeviceStatus,
 *                            empty to signal "stall this request")
 *   0x0B  PING             client → server: liveness probe; server replies with PONG
 *   0x0C  PONG             server → client: reply to PING; payload is the socket path
 *
 * Both sides drop the connection on framing errors. The server closes the socket when the
 * transport stops; a new client can reconnect.
 *
 * Not an IOKit driver. No code here runs in ptpd; that is OctopusBridge.h.
 */
#ifndef OCT_BRIDGE_H
#define OCT_BRIDGE_H

#include "oct_link.h"
#include "oct_service.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OCT_BRIDGE_MSG_HOST_ATTACHED    0x01u
#define OCT_BRIDGE_MSG_HOST_DETACHED    0x02u
#define OCT_BRIDGE_MSG_BULK_FROM_HOST   0x03u
#define OCT_BRIDGE_MSG_BULK_TO_HOST     0x04u
#define OCT_BRIDGE_MSG_INTERRUPT_EVENT  0x05u
#define OCT_BRIDGE_MSG_BUS_RESET        0x06u
#define OCT_BRIDGE_MSG_SUSPEND          0x07u
#define OCT_BRIDGE_MSG_RESUME           0x08u
#define OCT_BRIDGE_MSG_CLASS_REQUEST    0x09u
#define OCT_BRIDGE_MSG_CLASS_RESPONSE   0x0Au
#define OCT_BRIDGE_MSG_PING             0x0Bu
#define OCT_BRIDGE_MSG_PONG             0x0Cu
#define OCT_BRIDGE_HEADER_SIZE          5u /* 1 type + 4 length */
#define OCT_BRIDGE_MAX_PAYLOAD          (8u * 1024u * 1024u)
/* The socket path the Octopus app creates. The ptpd integration client connects to it. */
#define OCT_BRIDGE_DEFAULT_SOCKET       "/var/mobile/Library/Caches/Octopus/octopus-bridge.sock"

/* ---- Server (Octopus app) ----------------------------------------------------------------- */

typedef struct oct_bridge oct_bridge;

/* Creates the socket listener at `path` (NULL → OCT_BRIDGE_DEFAULT_SOCKET). The server is
 * idle until oct_bridge_transport fills `out_transport`, which the app passes to the service. */
oct_status  oct_bridge_create(const char *path, oct_bridge **out);
void        oct_bridge_destroy(oct_bridge *bridge);
/* Creates an oct_link backed by the bridge driver and fills `out_transport` with the
 * oct_transport the app passes to oct_service_set_transport. The bridge driver starts
 * accepting one client when the service starts, and closes on stop. `out_link` may be NULL. */
oct_status  oct_bridge_link_create(oct_bridge *bridge, oct_link **out_link, oct_transport *out_transport);
bool        oct_bridge_client_connected(const oct_bridge *bridge);
/* The socket path, for the diagnostic report. */
const char *oct_bridge_socket_path(const oct_bridge *bridge);

/* ---- Client (ptpd integration / diagnostic probe) ----------------------------------------- */

typedef struct oct_bridge_client oct_bridge_client;

/* Connects to the socket at `path` (NULL → default). Times out after `timeout_ms` (0 → 5000). */
oct_status  oct_bridge_client_connect(const char *path, uint32_t timeout_ms,
                                       oct_bridge_client **out);
void        oct_bridge_client_destroy(oct_bridge_client *client);
/* Sends a HOST_ATTACHED or HOST_DETACHED message. reason is for HOST_DETACHED only (may be NULL). */
oct_status  oct_bridge_client_host_event(oct_bridge_client *client, bool attached,
                                          const char *reason);
/* Sends a BULK_FROM_HOST message (bytes from Windows on the bulk OUT endpoint). */
oct_status  oct_bridge_client_send_bulk(oct_bridge_client *client, const uint8_t *bytes,
                                         size_t length);
/* Sends a CLASS_REQUEST. `out_reply` and `capacity` are for class requests that have a
 * device→host data stage (Get Device Status). `out_stall` is set when the server says stall. */
oct_status  oct_bridge_client_class_request(oct_bridge_client *client, uint8_t request,
                                             const uint8_t *data, size_t length,
                                             uint8_t *out_reply, size_t capacity,
                                             size_t *out_reply_length, bool *out_stall);
/* Sends a BUS_RESET, SUSPEND or RESUME message. */
oct_status  oct_bridge_client_lifecycle(oct_bridge_client *client, uint8_t type);
/* Sends a PING and waits for PONG. The socket path in the PONG payload goes into `info` (may be
 * NULL). `timeout_ms` is for the reply (0 → 2000). Returns OCT_E_STATE when no server answers. */
oct_status  oct_bridge_client_ping(oct_bridge_client *client, char *info, size_t capacity,
                                   uint32_t timeout_ms);
/* Reads one BULK_TO_HOST or INTERRUPT_EVENT from the server. `type` gets the message type,
 * `out_data` and `out_length` the payload (caller-owned, freeable with free()). */
oct_status  oct_bridge_client_receive(oct_bridge_client *client, uint8_t *type,
                                       uint8_t **out_data, size_t *out_length,
                                       uint32_t timeout_ms);

/* ---- Wire encoding helpers ---------------------------------------------------------------- */

/* Encode a bridge message into `buf`. Returns the number of bytes written, or 0 when `capacity`
 * is too small. `payload` may be NULL when `length` is 0. */
size_t oct_bridge_encode(uint8_t type, const void *payload, size_t length,
                          uint8_t *buf, size_t capacity);
/* Encode a bridge message in a freshly allocated buffer. Caller frees it. */
uint8_t *oct_bridge_alloc(uint8_t type, const void *payload, size_t length, size_t *out_length);

#ifdef __cplusplus
}
#endif

#endif /* OCT_BRIDGE_H */
