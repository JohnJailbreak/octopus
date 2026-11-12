/* oct_compat.c - compatibility checks for the transport adapter. See oct_compat.h. */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#elif !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "oct_compat.h"
#include "oct_link.h"
#include "oct_usb.h"
#include "oct_sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HS_PACKET 512u /* USB 2.0 high-speed bulk packets */

typedef struct rig {
    oct_service       *service;
    oct_loopback      *loopback;
    oct_link          *link;
    oct_link_reply     reply;
    oct_buf            scratch;
    uint32_t           tid;
    int64_t            now;
    oct_compat_report *report;
} rig;

#define REQUIRE(r, condition, ...)                                                              \
    do {                                                                                        \
        (r)->report->checks++;                                                                  \
        if (!(condition)) {                                                                     \
            snprintf((r)->report->failure, sizeof (r)->report->failure, __VA_ARGS__);           \
            return false;                                                                       \
        }                                                                                       \
    } while (0)

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static oct_status rig_open(rig *r, oct_compat_report *report, uint32_t event_packet)
{
    memset(r, 0, sizeof *r);
    memset(report, 0, sizeof *report);
    r->report = report;
    r->now = 1000;
    oct_buf_init(&r->scratch, 0);
    oct_link_driver driver;
    oct_link_config config = { HS_PACKET, 1000, "compat", true, event_packet };
    oct_status status = oct_service_create(&r->service);
    if (status == OCT_OK) status = oct_loopback_create(&r->loopback);
    if (status == OCT_OK) {
        oct_loopback_driver(r->loopback, &driver);
        status = oct_link_create(&driver, &config, &r->link);
    }
    if (status == OCT_OK) {
        oct_transport transport;
        oct_link_transport(r->link, &transport);
        oct_service_set_log_component(r->service, NULL);
        status = oct_service_set_transport(r->service, &transport);
    }
    oct_service_config service_config = { 0, false, false }; /* read-only: the interoperability target */
    if (status == OCT_OK) status = oct_service_start(r->service, &service_config);
    if (status != OCT_OK) {
        snprintf(report->failure, sizeof report->failure, "Preparing the simulated host failed: %s",
                 oct_status_name(status));
    }
    return status;
}

static void rig_close(rig *r)
{
    if (r->service) oct_service_stop(r->service);
    oct_service_destroy(r->service);
    oct_link_destroy(r->link);
    oct_loopback_destroy(r->loopback);
    oct_buf_free(&r->reply.data);
    oct_buf_free(&r->scratch);
}

static oct_status run(oct_compat_report *report, uint32_t event_packet, bool (*script)(rig *r))
{
    oct_compat_report ignored;
    if (!report) report = &ignored;
    rig r;
    oct_status status = rig_open(&r, report, event_packet);
    if (status == OCT_OK && !script(&r)) status = OCT_E_IO;
    rig_close(&r);
    return status;
}

/* One command through the adapter, its transfers checked by oct_link_transact; the response code in `code`. */
static bool send_command(rig *r, uint16_t operation, const uint32_t *params, uint32_t count, uint16_t *code)
{
    uint32_t tid = operation == OCT_OP_OPEN_SESSION ? 0 : r->tid;
    oct_status status = oct_link_transact(r->link, r->loopback, operation, tid, params, count, NULL, 0, 0, r->now,
                                          &r->reply);
    REQUIRE(r, status == OCT_OK, "Operation 0x%04X through the adapter: %s", operation, oct_status_name(status));
    r->tid = operation == OCT_OP_OPEN_SESSION ? 1 : r->tid + 1;
    *code = r->reply.response;
    return true;
}

static bool expect(rig *r, uint16_t operation, const uint32_t *params, uint32_t count, uint16_t expected)
{
    uint16_t code = 0;
    if (!send_command(r, operation, params, count, &code)) return false;
    REQUIRE(r, code == expected, "Operation 0x%04X answered 0x%04X, expected 0x%04X", operation, code, expected);
    return true;
}

static bool has(const uint16_t *values, uint32_t count, uint16_t wanted)
{
    for (uint32_t i = 0; i < count; i++) {
        if (values[i] == wanted) return true;
    }
    return false;
}

/* A PTP array of UINT16: up to `capacity` values kept, the rest read past. */
static uint32_t read_array(oct_reader *reader, uint16_t *out, uint32_t capacity)
{
    uint32_t count = oct_read_u32(reader), kept = 0;
    for (uint32_t i = 0; i < count && oct_reader_remaining(reader) >= 2; i++) {
        uint16_t value = oct_read_u16(reader);
        if (kept < capacity) out[kept++] = value;
    }
    return kept;
}

static bool name_of(rig *r, uint32_t handle, char *name, size_t capacity)
{
    uint32_t params[2] = { handle, OCT_PROP_OBJECT_FILE_NAME };
    if (!expect(r, OCT_OP_GET_OBJECT_PROP_VALUE, params, 2, OCT_RC_OK)) return false;
    oct_reader reader;
    oct_reader_init(&reader, r->reply.data.data, r->reply.data.length);
    REQUIRE(r, oct_read_string(&reader, name, capacity), "The name of object %u didn't decode", handle);
    return true;
}

/* The way a host finds an object: the folder's handles, then each one's ObjectFileName. */
static bool find_child(rig *r, uint32_t storage, uint32_t parent, const char *wanted, uint32_t *found)
{
    uint32_t query[3] = { storage, 0, parent };
    if (!expect(r, OCT_OP_GET_OBJECT_HANDLES, query, 3, OCT_RC_OK)) return false;
    uint32_t count = r->reply.data.length >= 4 ? rd32(r->reply.data.data) : 0, handles[64];
    REQUIRE(r, count <= 64 && r->reply.data.length == 4u + 4u * (size_t)count,
            "GetObjectHandles listed %u objects in %zu bytes", count, r->reply.data.length);
    for (uint32_t i = 0; i < count; i++) handles[i] = rd32(r->reply.data.data + 4 + 4 * i);
    *found = 0;
    for (uint32_t i = 0; i < count && *found == 0; i++) {
        char name[256];
        if (!name_of(r, handles[i], name, sizeof name)) return false;
        if (strcmp(name, wanted) == 0) *found = handles[i];
    }
    REQUIRE(r, *found != 0, "\"%s\" isn't listed", wanted);
    return true;
}

static bool mtp_script(rig *r)
{
    static const uint16_t operations_needed[] = {
        OCT_OP_GET_DEVICE_INFO,    OCT_OP_OPEN_SESSION,       OCT_OP_CLOSE_SESSION,
        OCT_OP_GET_STORAGE_IDS,    OCT_OP_GET_STORAGE_INFO,   OCT_OP_GET_OBJECT_HANDLES,
        OCT_OP_GET_OBJECT_INFO,    OCT_OP_GET_OBJECT,         OCT_OP_GET_PARTIAL_OBJECT,
        OCT_OP_GET_DEVICE_PROP_DESC, OCT_OP_GET_OBJECT_PROPS_SUPPORTED, OCT_OP_GET_OBJECT_PROP_VALUE,
    };
    static const uint16_t properties_needed[] = { OCT_PROP_STORAGE_ID, OCT_PROP_OBJECT_FORMAT, OCT_PROP_OBJECT_SIZE,
                                                  OCT_PROP_OBJECT_FILE_NAME, OCT_PROP_PARENT_OBJECT };
    REQUIRE(r, oct_link_host_attached(r->link) == OCT_OK, "The simulated host couldn't attach");

    /* DeviceInfo, before any session. */
    if (!expect(r, OCT_OP_GET_DEVICE_INFO, NULL, 0, OCT_RC_OK)) return false;
    REQUIRE(r, r->reply.has_data, "GetDeviceInfo sent no dataset");
    oct_reader reader;
    oct_reader_init(&reader, r->reply.data.data, r->reply.data.length);
    uint16_t standard = oct_read_u16(&reader);
    uint32_t vendor = oct_read_u32(&reader);
    (void)oct_read_u16(&reader); /* VendorExtensionVersion */
    char text[256];
    REQUIRE(r, oct_read_string(&reader, text, sizeof text), "The vendor extension description didn't decode");
    (void)oct_read_u16(&reader); /* FunctionalMode */
    uint16_t operations[128], events[32], device_properties[32];
    uint32_t operation_count = read_array(&reader, operations, 128);
    uint32_t event_count = read_array(&reader, events, 32);
    uint32_t device_property_count = read_array(&reader, device_properties, 32);
    REQUIRE(r, standard == 100, "StandardVersion is %u, expected 100 (PTP 1.0)", standard);
    REQUIRE(r, vendor == 6 && strstr(text, "microsoft.com") != NULL, "Vendor extension %u (\"%.64s\") isn't MTP", vendor,
            text);
    for (size_t i = 0; i < sizeof operations_needed / sizeof operations_needed[0]; i++) {
        REQUIRE(r, has(operations, operation_count, operations_needed[i]),
                "GetDeviceInfo doesn't list operation 0x%04X", operations_needed[i]);
    }
    REQUIRE(r, has(device_properties, device_property_count, OCT_DEVICE_PROP_FRIENDLY_NAME),
            "GetDeviceInfo doesn't list DeviceFriendlyName");

    /* A session, the device's name, and the storage. */
    uint32_t session = 1, property = OCT_DEVICE_PROP_FRIENDLY_NAME;
    if (!expect(r, OCT_OP_OPEN_SESSION, &session, 1, OCT_RC_OK)) return false;
    if (!expect(r, OCT_OP_GET_DEVICE_PROP_DESC, &property, 1, OCT_RC_OK)) return false;
    if (!expect(r, OCT_OP_GET_STORAGE_IDS, NULL, 0, OCT_RC_OK)) return false;
    REQUIRE(r, r->reply.data.length >= 8 && rd32(r->reply.data.data) >= 1, "No storage is listed");
    uint32_t storage = rd32(r->reply.data.data + 4);
    if (!expect(r, OCT_OP_GET_STORAGE_INFO, &storage, 1, OCT_RC_OK)) return false;
    REQUIRE(r, r->reply.data.length >= 6, "StorageInfo is too short");
    uint16_t access = rd16(r->reply.data.data + 4);
    REQUIRE(r, access == 0x0001, "The storage's AccessCapability is 0x%04X, expected 0x0001 (read-only)", access);
    uint32_t format = 0x3000; /* Undefined: any file */
    if (!expect(r, OCT_OP_GET_OBJECT_PROPS_SUPPORTED, &format, 1, OCT_RC_OK)) return false;
    oct_reader_init(&reader, r->reply.data.data, r->reply.data.length);
    uint16_t object_properties[64];
    uint32_t object_property_count = read_array(&reader, object_properties, 64);
    for (size_t i = 0; i < sizeof properties_needed / sizeof properties_needed[0]; i++) {
        REQUIRE(r, has(object_properties, object_property_count, properties_needed[i]),
                "Object property 0x%04X isn't supported for files", properties_needed[i]);
    }

    /* The root, then the nested folder, by name. */
    uint32_t readme = 0, documents = 0, example = 0;
    if (!find_child(r, storage, 0xFFFFFFFFu, "README.txt", &readme)) return false;
    if (!find_child(r, storage, 0xFFFFFFFFu, "Documents", &documents)) return false;
    if (!find_child(r, storage, documents, "example.txt", &example)) return false;

    /* The file: its size, then whole, then its first bytes, compared with a copy read without the responder. */
    uint32_t size_query[2] = { example, OCT_PROP_OBJECT_SIZE };
    if (!expect(r, OCT_OP_GET_OBJECT_PROP_VALUE, size_query, 2, OCT_RC_OK)) return false;
    REQUIRE(r, r->reply.data.length == 8, "ObjectSize came as %zu bytes, not a UINT64", r->reply.data.length);
    uint64_t size = (uint64_t)rd32(r->reply.data.data) | ((uint64_t)rd32(r->reply.data.data + 4) << 32);
    if (!expect(r, OCT_OP_GET_OBJECT, &example, 1, OCT_RC_OK)) return false;
    REQUIRE(r, r->reply.has_data && r->reply.data.length == size, "GetObject sent %zu bytes; ObjectSize says %llu",
            r->reply.data.length, (unsigned long long)size);
    char received[65], reference[65];
    uint64_t reference_size = 0;
    oct_sha256_hex(r->reply.data.data, r->reply.data.length, received);
    REQUIRE(r, oct_compat_reference_digest("Documents", "example.txt", &reference_size, reference) == OCT_OK,
            "The reference copy of example.txt couldn't be read");
    REQUIRE(r, reference_size == size && strcmp(received, reference) == 0,
            "example.txt arrived with SHA-256 %.16s..., expected %.16s...", received, reference);
    uint8_t first[16];
    size_t first_length = r->reply.data.length < sizeof first ? r->reply.data.length : sizeof first;
    memcpy(first, r->reply.data.data, first_length);
    uint32_t partial[3] = { example, 0, (uint32_t)first_length };
    if (!expect(r, OCT_OP_GET_PARTIAL_OBJECT, partial, 3, OCT_RC_OK)) return false;
    REQUIRE(r, r->reply.data.length == first_length && memcmp(r->reply.data.data, first, first_length) == 0,
            "GetPartialObject sent different bytes");

    /* The storage refuses changes. */
    uint16_t code = 0;
    uint32_t doomed[2] = { example, 0 };
    if (!send_command(r, OCT_OP_DELETE_OBJECT, doomed, 2, &code)) return false;
    REQUIRE(r, code == OCT_RC_STORE_READ_ONLY || code == OCT_RC_OBJECT_WRITE_PROTECTED || code == OCT_RC_ACCESS_DENIED,
            "Deleting from the read-only storage answered 0x%04X", code);
    if (!expect(r, OCT_OP_CLOSE_SESSION, NULL, 0, OCT_RC_OK)) return false;
    snprintf(r->report->summary, sizeof r->report->summary,
             "Documents/example.txt: %llu bytes, SHA-256 %s; %u operations and %u events advertised",
             (unsigned long long)size, received, operation_count, event_count);
    return true;
}

static oct_status class_request(rig *r, uint8_t request, const uint8_t *data, size_t length, uint16_t *status_code)
{
    uint8_t reply[8] = { 0 };
    size_t reply_length = 0;
    oct_status status = oct_link_class_request(r->link, request, data, length, reply, sizeof reply, &reply_length);
    if (status_code) *status_code = reply_length == 4 && rd16(reply) == 4 ? rd16(reply + 2) : 0;
    return status;
}

static bool class_script(rig *r)
{
    uint16_t code = 0;
    REQUIRE(r, oct_link_host_attached(r->link) == OCT_OK, "The simulated host couldn't attach");
    uint32_t session = 1;
    if (!expect(r, OCT_OP_OPEN_SESSION, &session, 1, OCT_RC_OK)) return false;
    REQUIRE(r, class_request(r, OCT_USB_GET_DEVICE_STATUS, NULL, 0, &code) == OCT_OK && code == OCT_RC_OK,
            "Get Device Status reported 0x%04X, not OK", code);
    static const uint8_t short_stage[5] = { 0x01, 0x40, 0, 0, 0 };
    static const uint8_t wrong_code[6] = { 0x02, 0x40, 0, 0, 0, 0 };
    REQUIRE(r, class_request(r, OCT_USB_CANCEL_REQUEST, short_stage, sizeof short_stage, NULL) == OCT_E_MALFORMED,
            "A 5-byte Cancel data stage was accepted");
    REQUIRE(r, class_request(r, OCT_USB_CANCEL_REQUEST, wrong_code, sizeof wrong_code, NULL) == OCT_E_MALFORMED,
            "A Cancel with cancellation code 0x4002 was accepted");

    /* Cancel a command that is half received; Get Device Status says so until the next command. */
    oct_buf_clear(&r->scratch);
    oct_container_command(&r->scratch, OCT_OP_GET_STORAGE_IDS, r->tid, NULL, 0);
    oct_link_input(r->link, r->scratch.data, 5, r->now);
    uint8_t cancel[6] = { 0x01, 0x40, (uint8_t)r->tid, (uint8_t)(r->tid >> 8), (uint8_t)(r->tid >> 16),
                          (uint8_t)(r->tid >> 24) };
    REQUIRE(r, class_request(r, OCT_USB_CANCEL_REQUEST, cancel, sizeof cancel, NULL) == OCT_OK, "Cancel was refused");
    REQUIRE(r, class_request(r, OCT_USB_GET_DEVICE_STATUS, NULL, 0, &code) == OCT_OK &&
                   code == OCT_RC_TRANSACTION_CANCELLED,
            "After Cancel, Get Device Status reported 0x%04X", code);
    if (!expect(r, OCT_OP_GET_STORAGE_IDS, NULL, 0, OCT_RC_OK)) return false;
    REQUIRE(r, class_request(r, OCT_USB_GET_DEVICE_STATUS, NULL, 0, &code) == OCT_OK && code == OCT_RC_OK,
            "Get Device Status stayed at 0x%04X after the next command", code);

    /* Device Reset ends the session; a new one opens. */
    REQUIRE(r, class_request(r, OCT_USB_DEVICE_RESET_REQUEST, NULL, 0, NULL) == OCT_OK, "Device Reset was refused");
    if (!expect(r, OCT_OP_GET_STORAGE_IDS, NULL, 0, OCT_RC_SESSION_NOT_OPEN)) return false;
    session = 2;
    if (!expect(r, OCT_OP_OPEN_SESSION, &session, 1, OCT_RC_OK)) return false;

    /* What the driver must stall. */
    REQUIRE(r, class_request(r, OCT_USB_GET_EXTENDED_EVENT_DATA, NULL, 0, NULL) == OCT_E_UNSUPPORTED,
            "Get Extended Event Data wasn't reported as unsupported");
    REQUIRE(r, class_request(r, 0x70, NULL, 0, NULL) == OCT_E_UNSUPPORTED, "An unknown request wasn't refused");
    uint8_t tiny[2];
    REQUIRE(r, oct_link_class_request(r->link, OCT_USB_GET_DEVICE_STATUS, NULL, 0, tiny, sizeof tiny, NULL) ==
                   OCT_E_INVALID_ARGUMENT,
            "Get Device Status wrote into a 2-byte reply");
    if (!expect(r, OCT_OP_CLOSE_SESSION, NULL, 0, OCT_RC_OK)) return false;
    snprintf(r->report->summary, sizeof r->report->summary,
             "Cancel, Get Device Status and Device Reset decoded; Get Extended Event Data and unknown requests stall");
    return true;
}

static bool event_script(rig *r)
{
    uint32_t handle = 7, three[3] = { 1, 2, 3 }, four[4] = { 1, 2, 3, 4 };
    oct_link_pipe pipe = OCT_LINK_BULK_IN;
    bool zlp = false;
    REQUIRE(r, oct_link_send_event(r->link, 0x4002, 0, &handle, 1) == OCT_E_STATE, "An event went out with no host");
    REQUIRE(r, oct_link_host_attached(r->link) == OCT_OK, "The simulated host couldn't attach");
    uint32_t session = 1;
    if (!expect(r, OCT_OP_OPEN_SESSION, &session, 1, OCT_RC_OK)) return false;
    REQUIRE(r, oct_link_send_event(r->link, 0x4002, 0, &handle, 1) == OCT_OK, "ObjectAdded couldn't be sent");
    REQUIRE(r, oct_loopback_next(r->loopback, &pipe, &r->scratch, &zlp), "No event transfer was queued");
    REQUIRE(r, pipe == OCT_LINK_INTERRUPT_IN && r->scratch.length == 16 && rd32(r->scratch.data) == 16 &&
                   rd16(r->scratch.data + 4) == OCT_CONTAINER_EVENT && rd16(r->scratch.data + 6) == 0x4002 &&
                   rd32(r->scratch.data + 12) == handle && !zlp,
            "The ObjectAdded event container is wrong (%zu bytes)", r->scratch.length);
    REQUIRE(r, oct_link_send_event(r->link, 0x4003, 0, three, 3) == OCT_OK, "A 3-parameter event couldn't be sent");
    REQUIRE(r, oct_loopback_next(r->loopback, &pipe, &r->scratch, &zlp) && r->scratch.length == 24 && zlp,
            "A 24-byte event on 24-byte packets must end with a zero-length packet");
    REQUIRE(r, oct_link_send_event(r->link, 0x4003, 0, four, 4) == OCT_E_INVALID_ARGUMENT,
            "A 4-parameter event was accepted");
    REQUIRE(r, oct_link_set_suspended(r->link, true) == OCT_OK, "Suspend was refused");
    REQUIRE(r, oct_link_send_event(r->link, 0x4002, 0, &handle, 1) == OCT_E_STATE, "An event went out while suspended");
    REQUIRE(r, oct_link_set_suspended(r->link, false) == OCT_OK, "Resume was refused");
    REQUIRE(r, oct_loopback_pending(r->loopback) == 0, "Refused events still produced transfers");
    if (!expect(r, OCT_OP_GET_STORAGE_IDS, NULL, 0, OCT_RC_OK)) return false; /* bulk traffic carries on */
    oct_link_status status;
    oct_link_get_status(r->link, &status);
    REQUIRE(r, status.events == 2, "%llu events counted, expected 2", (unsigned long long)status.events);
    if (!expect(r, OCT_OP_CLOSE_SESSION, NULL, 0, OCT_RC_OK)) return false;
    snprintf(r->report->summary, sizeof r->report->summary,
             "Event containers on the interrupt pipe: layout, zero-length packets, refusals without a host or awake bus");
    return true;
}

static oct_transport_state state_of(rig *r, oct_link_status *status)
{
    oct_link_get_status(r->link, status);
    return status->state;
}

static bool lifecycle_script(rig *r)
{
    oct_link_status status;
    oct_service_config config = { 0, false, false };
    REQUIRE(r, oct_link_host_attached(r->link) == OCT_OK, "The simulated host couldn't attach");
    uint32_t session = 1;
    if (!expect(r, OCT_OP_OPEN_SESSION, &session, 1, OCT_RC_OK)) return false;

    /* Suspend keeps the session but refuses input. */
    REQUIRE(r, oct_link_set_suspended(r->link, true) == OCT_OK, "Suspend was refused");
    oct_buf_clear(&r->scratch);
    oct_container_command(&r->scratch, OCT_OP_GET_STORAGE_IDS, r->tid, NULL, 0);
    REQUIRE(r, oct_link_input(r->link, r->scratch.data, r->scratch.length, r->now) == OCT_E_STATE,
            "Input was accepted while suspended");
    REQUIRE(r, oct_link_set_suspended(r->link, false) == OCT_OK, "Resume was refused");
    if (!expect(r, OCT_OP_GET_STORAGE_IDS, NULL, 0, OCT_RC_OK)) return false;

    /* A bus reset ends the session; the host stays. */
    REQUIRE(r, oct_link_bus_reset(r->link) == OCT_OK, "The bus reset failed");
    state_of(r, &status);
    REQUIRE(r, status.bus_resets == 1 && status.host_attached && !status.session_open,
            "After a bus reset: %llu resets, host %d, session %d", (unsigned long long)status.bus_resets,
            status.host_attached, status.session_open);
    if (!expect(r, OCT_OP_GET_STORAGE_IDS, NULL, 0, OCT_RC_SESSION_NOT_OPEN)) return false;
    session = 2;
    if (!expect(r, OCT_OP_OPEN_SESSION, &session, 1, OCT_RC_OK)) return false;

    /* Unplugged mid-container, then plugged back in. */
    oct_link_input(r->link, r->scratch.data, 5, r->now);
    REQUIRE(r, oct_link_host_detached(r->link, "compatibility unplug") == OCT_OK, "The host couldn't detach");
    REQUIRE(r, state_of(r, &status) == OCT_TRANSPORT_DISCONNECTED, "The unplug wasn't reported");
    REQUIRE(r, oct_link_host_attached(r->link) == OCT_OK, "The host couldn't reattach");
    session = 3;
    if (!expect(r, OCT_OP_OPEN_SESSION, &session, 1, OCT_RC_OK)) return false;
    if (!expect(r, OCT_OP_CLOSE_SESSION, NULL, 0, OCT_RC_OK)) return false;

    /* Stopping and starting the service three times. */
    for (int cycle = 0; cycle < 3; cycle++) {
        REQUIRE(r, oct_service_stop(r->service) == OCT_OK, "Stop %d failed", cycle + 1);
        REQUIRE(r, state_of(r, &status) == OCT_TRANSPORT_UNAVAILABLE, "Stop %d left the transport running", cycle + 1);
        REQUIRE(r, oct_link_bus_reset(r->link) == OCT_E_STATE, "A bus reset was accepted while stopped");
        REQUIRE(r, oct_service_start(r->service, &config) == OCT_OK, "Start %d failed", cycle + 1);
        REQUIRE(r, state_of(r, &status) == OCT_TRANSPORT_WAITING, "Start %d isn't waiting for a host", cycle + 1);
        REQUIRE(r, oct_link_host_attached(r->link) == OCT_OK, "The host couldn't attach after start %d", cycle + 1);
        if (!expect(r, OCT_OP_GET_DEVICE_INFO, NULL, 0, OCT_RC_OK)) return false;
    }
    snprintf(r->report->summary, sizeof r->report->summary,
             "Suspend and resume, a bus reset, an unplug mid-container, and three stop and start cycles");
    return true;
}

oct_status oct_compat_mtp_enumeration(oct_compat_report *report) { return run(report, 0, mtp_script); }
oct_status oct_compat_class_requests(oct_compat_report *report) { return run(report, 0, class_script); }
oct_status oct_compat_events(oct_compat_report *report) { return run(report, 24, event_script); }
oct_status oct_compat_lifecycle(oct_compat_report *report) { return run(report, 0, lifecycle_script); }

oct_status oct_compat_reference_digest(const char *folder, const char *name, uint64_t *size, char hex[65])
{
    if (!name || !hex) return OCT_E_INVALID_ARGUMENT;
    oct_synthetic_options options = { .time = 0 }; /* designated: every other field defaults */
    oct_backend backend;
    oct_store *store = NULL;
    oct_status status = oct_synthetic_backend_create(&options, &backend);
    if (status == OCT_OK) status = oct_store_open(NULL, backend, &store);
    if (status == OCT_OK) status = oct_store_index_all(store);
    uint32_t parent = OCT_ROOT, handle = 0;
    if (status == OCT_OK && folder) status = oct_store_find(store, OCT_ROOT, folder, &parent);
    if (status == OCT_OK) status = oct_store_find(store, parent, name, &handle);
    oct_object_info info;
    if (status == OCT_OK) status = oct_store_get(store, handle, &info);
    if (status == OCT_OK && info.is_directory) status = OCT_E_IS_DIRECTORY;
    if (status == OCT_OK && info.size > 64u * 1024u * 1024u) status = OCT_E_TOO_LARGE;
    uint8_t *bytes = status == OCT_OK ? malloc(info.size ? (size_t)info.size : 1u) : NULL;
    if (status == OCT_OK && !bytes) status = OCT_E_NO_MEMORY;
    size_t got = 0;
    if (status == OCT_OK) status = oct_store_read(store, handle, 0, bytes, (size_t)info.size, &got);
    if (status == OCT_OK && got != info.size) status = OCT_E_IO;
    if (status == OCT_OK) {
        oct_sha256_hex(bytes, got, hex);
        if (size) *size = info.size;
    }
    free(bytes);
    oct_store_close(store);
    return status;
}

/* The setup packets and descriptors here are synthetic USB 2.0 fixtures, not
 * captured Windows traffic. This exercises the same boundary a native owner uses. */
static bool usb_boundary_script(rig *r)
{
    static const uint8_t descriptors[] = {
        9,2,39,0,1,1,0,0x80,50,
        9,4,0,0,3,6,1,1,0,
        7,5,0x81,2,0,2,0,
        7,5,0x02,2,0,2,0,
        7,5,0x83,3,64,0,4
    };
    oct_usb_interface interface;
    REQUIRE(r, oct_usb_parse_interface(descriptors, sizeof descriptors, 0, 0, &interface) == OCT_OK,
            "Synthetic PTP descriptors were rejected");
    REQUIRE(r, interface.bulk_in == 0x81 && interface.bulk_out == 2 && interface.interrupt_in == 0x83 &&
            interface.bulk_in_packet == 512 && interface.bulk_out_packet == 512 && interface.interrupt_packet == 64,
            "PTP endpoint selection was incorrect");
    for (size_t i = 0; i < sizeof descriptors; i++) {
        REQUIRE(r, oct_usb_parse_interface(descriptors, i, 0, 0, &interface) == OCT_E_MALFORMED,
                "Truncated descriptors (%zu bytes) were accepted", i);
    }
    uint8_t setup[8] = { 0xA1, OCT_USB_GET_DEVICE_STATUS, 0,0,0,0,4,0 };
    uint8_t reply[8] = { 0 };
    size_t length = 99;
    REQUIRE(r, oct_usb_control(r->link, 0, setup, sizeof setup, NULL, 0, reply, sizeof reply, &length) == OCT_E_STATE &&
            length == 0, "Unattached interface answered a control request");
    REQUIRE(r, oct_link_host_attached(r->link) == OCT_OK, "Simulated host could not attach");
    uint32_t session = 1;
    uint16_t code;
    if (!send_command(r, OCT_OP_OPEN_SESSION, &session, 1, &code)) return false;
    REQUIRE(r, code == OCT_RC_OK, "Could not open the synthetic USB session");
    for (uint8_t n = 0; n <= 8; n++) {
        setup[6] = n;
        memset(reply, 0xCC, sizeof reply);
        REQUIRE(r, oct_usb_control(r->link, 0, setup, sizeof setup, NULL, 0, reply, sizeof reply, &length) == OCT_OK,
                "Device status rejected host buffer length %u", n);
        REQUIRE(r, length == (n < 4 ? n : 4) && reply[length] == 0xCC,
                "Device status overran host buffer length %u", n);
    }
    setup[0] = 0x21; setup[1] = OCT_USB_DEVICE_RESET_REQUEST; setup[6] = 0;
    for (size_t field = 0; field < sizeof setup; field++) {
        uint8_t bad[8];
        memcpy(bad, setup, sizeof bad);
        bad[field] ^= 0x80;
        REQUIRE(r, oct_usb_control(r->link, 0, bad, sizeof bad, NULL, 0, reply, sizeof reply, &length) != OCT_OK &&
                length == 0, "Invalid reset setup field %zu was accepted", field);
        oct_link_status status;
        oct_link_get_status(r->link, &status);
        REQUIRE(r, status.session_open && status.resets == 0, "Invalid reset changed the session");
    }
    REQUIRE(r, oct_usb_control(r->link, 0, setup, sizeof setup, reply, 1, NULL, 0, &length) == OCT_E_MALFORMED,
            "Reset accepted an unexpected OUT data stage");
    REQUIRE(r, oct_usb_control(r->link, 0, setup, sizeof setup, NULL, 0, NULL, 0, &length) == OCT_OK,
            "Valid reset failed");
    oct_link_status status;
    oct_link_get_status(r->link, &status);
    REQUIRE(r, !status.session_open && status.resets == 1, "Valid reset did not close the session");
    setup[1] = OCT_USB_CANCEL_REQUEST; setup[6] = 6;
    uint8_t cancel[6] = { 1,0x40,1,0,0,0 };
    REQUIRE(r, oct_usb_control(r->link, 0, setup, sizeof setup, cancel, sizeof cancel, NULL, 0, &length) == OCT_OK,
            "Valid cancel failed");
    REQUIRE(r, oct_link_device_status(r->link) == OCT_RC_TRANSACTION_CANCELLED,
            "Cancellation status was lost at the control boundary");
    REQUIRE(r, oct_link_set_suspended(r->link, true) == OCT_OK, "Could not suspend");
    REQUIRE(r, oct_usb_control(r->link, 0, setup, sizeof setup, cancel, sizeof cancel, NULL, 0, &length) == OCT_E_STATE,
            "Suspended interface accepted control traffic");
    oct_copy_utf8(r->report->summary, sizeof r->report->summary,
                 "Synthetic USB descriptors and full setup packets: endpoint selection, interface routing, malformed reset isolation, bounded status, cancel and suspend");
    return true;
}

oct_status oct_compat_usb_boundary(oct_compat_report *report)
{
    return run(report, 64, usb_boundary_script);
}
