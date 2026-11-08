#include "test_support.h"

#include "../../Core/oct_link.h"
#include "../../Core/oct_log.h"

#include <pthread.h>
#include <time.h>
#include <unistd.h>

/* A started service on a loopback link with a host attached: the real responder and storage behind it. */
typedef struct rig {
    oct_service   *service;
    oct_loopback  *loopback;
    oct_link      *link;
    oct_link_reply reply;
    uint32_t       tid;
    int64_t        now;
} rig;

static void rig_create(rig *r, uint32_t max_packet, bool scratch, bool start)
{
    memset(r, 0, sizeof *r);
    r->now = 1000;
    CHECK_OK(oct_service_create(&r->service));
    CHECK_OK(oct_loopback_create(&r->loopback));
    oct_link_driver driver;
    oct_loopback_driver(r->loopback, &driver);
    oct_link_config config = { max_packet, 1000, "linktest", false, 0 };
    CHECK_OK(oct_link_create(&driver, &config, &r->link));
    oct_transport transport;
    oct_link_transport(r->link, &transport);
    CHECK_OK(oct_service_set_transport(r->service, &transport));
    if (start) {
        oct_service_config service_config = { SEED_TIME, scratch, false };
        CHECK_OK(oct_service_start(r->service, &service_config));
        CHECK_OK(oct_link_host_attached(r->link));
    }
}

static void rig_destroy(rig *r)
{
    oct_service_stop(r->service);
    oct_service_destroy(r->service);
    oct_link_destroy(r->link);
    oct_loopback_destroy(r->loopback);
    oct_buf_free(&r->reply.data);
}

static uint16_t call(rig *r, uint16_t operation, const uint32_t *params, uint32_t count, const void *data,
                     size_t length, size_t chunk)
{
    uint32_t tid = operation == OCT_OP_OPEN_SESSION ? 0 : r->tid;
    oct_status status = oct_link_transact(r->link, r->loopback, operation, tid, params, count, data, length, chunk,
                                          r->now, &r->reply);
    CHECK_OK(status);
    r->tid = operation == OCT_OP_OPEN_SESSION ? 1 : r->tid + 1;
    return status == OCT_OK ? r->reply.response : 0;
}

static void open_session(rig *r, uint32_t id)
{
    CHECK_EQ(call(r, OCT_OP_OPEN_SESSION, &id, 1, NULL, 0, 0), OCT_RC_OK);
}

static oct_link_status link_status_of(rig *r)
{
    oct_link_status status;
    oct_link_get_status(r->link, &status);
    return status;
}

static void drain(rig *r)
{
    oct_buf transfer;
    oct_buf_init(&transfer, 0);
    while (oct_loopback_next(r->loopback, NULL, &transfer, NULL)) {
    }
    oct_buf_free(&transfer);
}

static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* The handle of a root object by name, found the way a host would: handles, then each ObjectInfo. */
static uint32_t find_root_object(rig *r, const char *name)
{
    uint32_t query[3] = { OCT_DEFAULT_STORAGE_ID, 0, 0xFFFFFFFFu };
    if (call(r, OCT_OP_GET_OBJECT_HANDLES, query, 3, NULL, 0, 0) != OCT_RC_OK || r->reply.data.length < 4) return 0;
    uint32_t count = read_u32(r->reply.data.data), handles[32];
    for (uint32_t i = 0; i < count && i < 32; i++) handles[i] = read_u32(r->reply.data.data + 4 + 4 * i);
    for (uint32_t i = 0; i < count && i < 32; i++) {
        if (call(r, OCT_OP_GET_OBJECT_INFO, &handles[i], 1, NULL, 0, 0) != OCT_RC_OK) continue;
        oct_reader reader;
        oct_reader_init(&reader, r->reply.data.data, r->reply.data.length);
        reader.position = 52; /* the filename follows the fixed ObjectInfo fields */
        char found[256];
        if (oct_read_string(&reader, found, sizeof found) && strcmp(found, name) == 0) return handles[i];
    }
    return 0;
}

static uint32_t children(rig *r, uint32_t parent)
{
    uint32_t query[3] = { OCT_DEFAULT_STORAGE_ID, 0, parent };
    CHECK_EQ(call(r, OCT_OP_GET_NUM_OBJECTS, query, 3, NULL, 0, 0), OCT_RC_OK);
    return r->reply.param_count ? r->reply.params[0] : 0;
}

static uint32_t next_random(uint32_t *state)
{
    *state ^= *state << 13;
    *state ^= *state >> 17;
    *state ^= *state << 5;
    return *state;
}

static void test_fragmentation(void)
{
    rig r;
    rig_create(&r, 0, true, true);
    CHECK_EQ(call(&r, OCT_OP_GET_DEVICE_INFO, NULL, 0, NULL, 0, 0), OCT_RC_OK);
    oct_buf reference;
    oct_buf_init(&reference, 0);
    oct_buf_bytes(&reference, r.reply.data.data, r.reply.data.length);
    CHECK(reference.length > 40);
    bool same = true;
    for (size_t chunk = 1; chunk <= 40; chunk++) {
        CHECK_EQ(call(&r, OCT_OP_GET_DEVICE_INFO, NULL, 0, NULL, 0, chunk), OCT_RC_OK);
        same = same && r.reply.data.length == reference.length &&
               memcmp(r.reply.data.data, reference.data, reference.length) == 0;
    }
    CHECK(same);
    oct_buf_free(&reference);

    /* Two containers in one piece of input are answered in order, each transfer whole. */
    open_session(&r, 1);
    oct_buf both;
    oct_buf_init(&both, 0);
    uint32_t query[3] = { OCT_DEFAULT_STORAGE_ID, 0, 0xFFFFFFFFu };
    oct_container_command(&both, OCT_OP_GET_STORAGE_IDS, r.tid, NULL, 0);
    oct_container_command(&both, OCT_OP_GET_NUM_OBJECTS, r.tid + 1, query, 3);
    CHECK_OK(oct_link_input(r.link, both.data, both.length, r.now));
    oct_buf transfer;
    oct_buf_init(&transfer, 0);
    uint16_t types[4] = { 0 };
    uint32_t tids[4] = { 0 };
    size_t transfers = 0;
    while (transfers < 4 && oct_loopback_next(r.loopback, NULL, &transfer, NULL)) {
        CHECK_EQ(read_u32(transfer.data), transfer.length);
        types[transfers] = (uint16_t)(transfer.data[4] | (transfer.data[5] << 8));
        tids[transfers++] = read_u32(transfer.data + 8);
    }
    CHECK_EQ(transfers, 3);
    CHECK(types[0] == OCT_CONTAINER_DATA && types[1] == OCT_CONTAINER_RESPONSE && types[2] == OCT_CONTAINER_RESPONSE);
    CHECK(tids[0] == r.tid && tids[1] == r.tid && tids[2] == r.tid + 1);
    r.tid += 2;
    oct_buf_free(&both);
    oct_buf_free(&transfer);

    /* An upload whose command and data phase arrive in small pieces. */
    uint32_t scratch = find_root_object(&r, "Scratch");
    CHECK(scratch != 0);
    oct_buf dataset;
    oct_buf_init(&dataset, 0);
    object_info_dataset(&dataset, OCT_DEFAULT_STORAGE_ID, "note.txt", oct_format_for_name("note.txt", false), 300);
    uint32_t where[2] = { OCT_DEFAULT_STORAGE_ID, scratch };
    CHECK_EQ(call(&r, OCT_OP_SEND_OBJECT_INFO, where, 2, dataset.data, dataset.length, 7), OCT_RC_OK);
    CHECK_EQ(r.reply.param_count, 3);
    uint32_t handle = r.reply.params[2];
    uint8_t content[300];
    for (size_t i = 0; i < sizeof content; i++) content[i] = (uint8_t)(i * 7 + 3);
    CHECK_EQ(call(&r, OCT_OP_SEND_OBJECT, NULL, 0, content, sizeof content, 11), OCT_RC_OK);
    CHECK_EQ(call(&r, OCT_OP_GET_OBJECT, &handle, 1, NULL, 0, 3), OCT_RC_OK);
    CHECK(r.reply.data.length == sizeof content && memcmp(r.reply.data.data, content, sizeof content) == 0);
    oct_buf_free(&dataset);

    /* A whole enumeration with random piece sizes. */
    uint32_t seed = 0x2545F491u;
    CHECK_EQ(call(&r, OCT_OP_GET_OBJECT_HANDLES, query, 3, NULL, 0, 1 + next_random(&seed) % 50), OCT_RC_OK);
    uint32_t count = r.reply.data.length >= 4 ? read_u32(r.reply.data.data) : 0, handles[32];
    CHECK(count >= 4);
    for (uint32_t i = 0; i < count && i < 32; i++) handles[i] = read_u32(r.reply.data.data + 4 + 4 * i);
    for (uint32_t i = 0; i < count && i < 32; i++) {
        CHECK_EQ(call(&r, OCT_OP_GET_OBJECT_INFO, &handles[i], 1, NULL, 0, 1 + next_random(&seed) % 50), OCT_RC_OK);
    }
    rig_destroy(&r);
}

static void check_reply_transfers(rig *r, bool response_needs_zlp)
{
    oct_buf command, transfer;
    oct_buf_init(&command, 0);
    oct_buf_init(&transfer, 0);
    oct_container_command(&command, OCT_OP_GET_DEVICE_INFO, 0, NULL, 0);
    CHECK_OK(oct_link_input(r->link, command.data, command.length, r->now));
    oct_link_pipe pipe = OCT_LINK_INTERRUPT_IN;
    bool zlp = false;
    CHECK(oct_loopback_next(r->loopback, &pipe, &transfer, &zlp));
    CHECK_EQ(pipe, OCT_LINK_BULK_IN);
    CHECK_EQ(transfer.data[4], OCT_CONTAINER_DATA);
    CHECK_EQ(read_u32(transfer.data), transfer.length);
    CHECK(oct_loopback_next(r->loopback, &pipe, &transfer, &zlp));
    CHECK_EQ(transfer.data[4], OCT_CONTAINER_RESPONSE);
    CHECK_EQ(transfer.length, 12);
    CHECK_EQ(zlp, response_needs_zlp);
    CHECK(!oct_loopback_next(r->loopback, &pipe, &transfer, &zlp));
    oct_buf_free(&command);
    oct_buf_free(&transfer);
}

static void test_framing(void)
{
    rig r;
    rig_create(&r, 12, false, true); /* 12-byte packets: a 12-byte response must end with a ZLP */
    check_reply_transfers(&r, true);
    rig_destroy(&r);
    rig_create(&r, 0, false, true); /* 512-byte packets */
    check_reply_transfers(&r, false);
    rig_destroy(&r);
}

static void test_malformed(void)
{
    rig r;
    rig_create(&r, 0, true, true);
    oct_link_status before = link_status_of(&r);
    uint8_t short_length[12] = { 4, 0, 0, 0, 1, 0, 0x01, 0x10, 0, 0, 0, 0 };
    CHECK_EQ(oct_link_input(r.link, short_length, sizeof short_length, r.now), OCT_E_MALFORMED);
    CHECK_EQ(link_status_of(&r).malformed, before.malformed + 1);
    CHECK_EQ(oct_loopback_pending(r.loopback), 0);
    CHECK(strstr(link_status_of(&r).last_failure, "less than its header") != NULL);
    CHECK_EQ(call(&r, OCT_OP_GET_DEVICE_INFO, NULL, 0, NULL, 0, 0), OCT_RC_OK);

    uint8_t wrong_type[12] = { 12, 0, 0, 0, OCT_CONTAINER_RESPONSE, 0, 0x01, 0x20, 5, 0, 0, 0 };
    CHECK_OK(oct_link_input(r.link, wrong_type, sizeof wrong_type, r.now));
    CHECK_EQ(link_status_of(&r).malformed, before.malformed + 2);
    drain(&r);

    oct_buf stray;
    oct_buf_init(&stray, 0);
    size_t start = oct_container_data_begin(&stray, OCT_OP_SEND_OBJECT, 9);
    oct_buf_bytes(&stray, "abc", 3);
    oct_container_data_end(&stray, start);
    CHECK_OK(oct_link_input(r.link, stray.data, stray.length, r.now));
    CHECK(strstr(link_status_of(&r).last_failure, "no command waiting") != NULL);
    CHECK_EQ(oct_loopback_pending(r.loopback), 0);
    oct_buf_free(&stray);

    /* An oversized container is skipped byte for byte, then the next one is answered. */
    uint32_t huge = OCT_PTP_HEADER_SIZE + OCT_PTP_MAX_TRANSFER + 1;
    uint8_t header[12] = { (uint8_t)huge, (uint8_t)(huge >> 8), (uint8_t)(huge >> 16), (uint8_t)(huge >> 24),
                           OCT_CONTAINER_DATA, 0, 0x0D, 0x10, 9, 0, 0, 0 };
    CHECK_EQ(oct_link_input(r.link, header, sizeof header, r.now), OCT_E_TOO_LARGE);
    static uint8_t zeros[1u << 20];
    for (size_t left = huge - sizeof header; left > 0;) {
        size_t n = left < sizeof zeros ? left : sizeof zeros;
        CHECK_OK(oct_link_input(r.link, zeros, n, r.now));
        left -= n;
    }
    CHECK_EQ(oct_loopback_pending(r.loopback), 0);
    CHECK_EQ(call(&r, OCT_OP_GET_DEVICE_INFO, NULL, 0, NULL, 0, 0), OCT_RC_OK);

    /* The same, abandoned midway, is dropped by the timeout. */
    uint64_t timeouts = link_status_of(&r).timeouts;
    CHECK_EQ(oct_link_input(r.link, header, sizeof header, r.now), OCT_E_TOO_LARGE);
    CHECK_OK(oct_link_input(r.link, zeros, 1000, r.now));
    r.now += 1000;
    CHECK_OK(oct_link_tick(r.link, r.now));
    CHECK_EQ(link_status_of(&r).timeouts, timeouts + 1);
    CHECK_EQ(call(&r, OCT_OP_GET_DEVICE_INFO, NULL, 0, NULL, 0, 0), OCT_RC_OK);

    /* Random garbage never breaks the link for good. */
    uint32_t seed = 0x9E3779B9u;
    for (int round = 0; round < 300; round++) {
        uint8_t junk[64];
        size_t n = 1 + next_random(&seed) % sizeof junk;
        for (size_t i = 0; i < n; i++) junk[i] = (uint8_t)next_random(&seed);
        if (round % 3 == 0 && n >= 12) { /* plausible lengths reach the responder more often */
            junk[0] = (uint8_t)n;
            junk[1] = junk[2] = junk[3] = 0;
            junk[4] = (uint8_t)(1 + round % 2);
        }
        oct_link_input(r.link, junk, n, r.now);
        r.now += 1001;
        oct_link_tick(r.link, r.now);
        drain(&r);
    }
    CHECK_OK(oct_link_device_reset(r.link));
    drain(&r);
    r.tid = 0;
    CHECK_EQ(call(&r, OCT_OP_GET_DEVICE_INFO, NULL, 0, NULL, 0, 5), OCT_RC_OK);
    open_session(&r, 1);
    CHECK_EQ(call(&r, OCT_OP_GET_STORAGE_IDS, NULL, 0, NULL, 0, 0), OCT_RC_OK);
    rig_destroy(&r);
}

static void test_sessions(void)
{
    rig r;
    rig_create(&r, 0, false, true);
    CHECK_EQ(link_status_of(&r).state, OCT_TRANSPORT_HOST_CONNECTED);
    open_session(&r, 1);
    oct_link_status status = link_status_of(&r);
    CHECK_EQ(status.state, OCT_TRANSPORT_SESSION_ACTIVE);
    CHECK(status.session_open);
    CHECK_EQ(status.session_id, 1);
    CHECK_EQ(call(&r, OCT_OP_GET_STORAGE_IDS, NULL, 0, NULL, 0, 0), OCT_RC_OK);
    uint32_t again = 1;
    CHECK_EQ(call(&r, OCT_OP_OPEN_SESSION, &again, 1, NULL, 0, 0), OCT_RC_SESSION_ALREADY_OPEN);
    CHECK_EQ(link_status_of(&r).state, OCT_TRANSPORT_SESSION_ACTIVE);
    CHECK_EQ(call(&r, OCT_OP_CLOSE_SESSION, NULL, 0, NULL, 0, 0), OCT_RC_OK);
    CHECK_EQ(link_status_of(&r).state, OCT_TRANSPORT_HOST_CONNECTED);
    CHECK(!link_status_of(&r).session_open);
    CHECK_EQ(call(&r, OCT_OP_GET_STORAGE_IDS, NULL, 0, NULL, 0, 0), OCT_RC_SESSION_NOT_OPEN);

    uint32_t query[3] = { OCT_DEFAULT_STORAGE_ID, 0, 0xFFFFFFFFu };
    for (uint32_t cycle = 0; cycle < 5; cycle++) {
        open_session(&r, 10 + cycle);
        CHECK_EQ(link_status_of(&r).session_id, 10 + cycle);
        CHECK_EQ(call(&r, OCT_OP_GET_NUM_OBJECTS, query, 3, NULL, 0, 0), OCT_RC_OK);
        CHECK_EQ(call(&r, OCT_OP_CLOSE_SESSION, NULL, 0, NULL, 0, 0), OCT_RC_OK);
    }
    status = link_status_of(&r);
    CHECK_EQ(status.transactions, 20);
    CHECK_EQ(status.last_operation, OCT_OP_CLOSE_SESSION);
    CHECK_EQ(status.last_response, OCT_RC_OK);

    open_session(&r, 20);
    CHECK_OK(oct_link_device_reset(r.link));
    CHECK_EQ(link_status_of(&r).state, OCT_TRANSPORT_HOST_CONNECTED);
    CHECK_EQ(link_status_of(&r).resets, 1);
    CHECK_EQ(call(&r, OCT_OP_GET_STORAGE_IDS, NULL, 0, NULL, 0, 0), OCT_RC_SESSION_NOT_OPEN);

    /* The service reports the transport's own state and the last failure. */
    oct_service_status service_status;
    CHECK_OK(oct_service_get_status(r.service, &service_status));
    CHECK_EQ(service_status.transport_state, OCT_TRANSPORT_HOST_CONNECTED);
    CHECK(service_status.transport_transactions > 0);
    CHECK_STR(service_status.transport_name, "Loopback");
    open_session(&r, 21);
    CHECK_OK(oct_service_get_status(r.service, &service_status));
    CHECK_EQ(service_status.transport_state, OCT_TRANSPORT_SESSION_ACTIVE);
    CHECK_STR(oct_transport_state_name(service_status.transport_state), "Session active");
    rig_destroy(&r);

    /* Without a transport, the service says so. */
    oct_service *plain = NULL;
    CHECK_OK(oct_service_create(&plain));
    oct_service_config config = { SEED_TIME, false, false };
    CHECK_OK(oct_service_start(plain, &config));
    CHECK_OK(oct_service_get_status(plain, &service_status));
    CHECK_EQ(service_status.state, OCT_SERVICE_READY);
    CHECK_EQ(service_status.transport_state, OCT_TRANSPORT_UNAVAILABLE);
    CHECK_STR(service_status.transport_failure, "");
    oct_service_destroy(plain);
}

static void test_interrupted(void)
{
    rig r;
    rig_create(&r, 0, true, true);
    open_session(&r, 1);
    uint32_t scratch = find_root_object(&r, "Scratch");
    CHECK(scratch != 0);
    uint32_t before = children(&r, scratch);
    uint32_t where[2] = { OCT_DEFAULT_STORAGE_ID, scratch };
    oct_buf dataset, command, data;
    oct_buf_init(&dataset, 0);
    oct_buf_init(&command, 0);
    oct_buf_init(&data, 0);
    object_info_dataset(&dataset, OCT_DEFAULT_STORAGE_ID, "cut.txt", oct_format_for_name("cut.txt", false), 10);

    /* Half a data phase, then the cable is pulled. */
    oct_container_command(&command, OCT_OP_SEND_OBJECT_INFO, r.tid, where, 2);
    size_t start = oct_container_data_begin(&data, OCT_OP_SEND_OBJECT_INFO, r.tid);
    oct_buf_bytes(&data, dataset.data, dataset.length);
    oct_container_data_end(&data, start);
    CHECK_OK(oct_link_input(r.link, command.data, command.length, r.now));
    CHECK_OK(oct_link_input(r.link, data.data, data.length / 2, r.now));
    CHECK_OK(oct_link_host_detached(r.link, "unplugged"));
    oct_link_status status = link_status_of(&r);
    CHECK_EQ(status.state, OCT_TRANSPORT_DISCONNECTED);
    CHECK_EQ(status.disconnects, 1);
    CHECK(strstr(status.last_failure, "mid-transfer") != NULL);
    CHECK_OK(oct_link_host_attached(r.link));
    CHECK_EQ(link_status_of(&r).state, OCT_TRANSPORT_HOST_CONNECTED);
    drain(&r);
    open_session(&r, 2);
    CHECK_EQ(children(&r, scratch), before);

    /* A command whose data phase never comes: a warning at the timeout, then the next command moves on. */
    oct_buf_clear(&command);
    oct_container_command(&command, OCT_OP_SEND_OBJECT_INFO, r.tid++, where, 2);
    CHECK_OK(oct_link_input(r.link, command.data, command.length, r.now));
    r.now += 1000;
    CHECK_OK(oct_link_tick(r.link, r.now));
    CHECK(strstr(link_status_of(&r).last_failure, "for its data phase") != NULL);
    CHECK_EQ(call(&r, OCT_OP_GET_STORAGE_IDS, NULL, 0, NULL, 0, 0), OCT_RC_OK);
    CHECK(strstr(link_status_of(&r).last_failure, "never got its data phase") != NULL);

    /* Half a data phase, then Cancel. */
    uint32_t cancelled = r.tid++;
    oct_buf_clear(&command);
    oct_container_command(&command, OCT_OP_SEND_OBJECT_INFO, cancelled, where, 2);
    oct_buf_clear(&data);
    start = oct_container_data_begin(&data, OCT_OP_SEND_OBJECT_INFO, cancelled);
    oct_buf_bytes(&data, dataset.data, dataset.length);
    oct_container_data_end(&data, start);
    CHECK_OK(oct_link_input(r.link, command.data, command.length, r.now));
    CHECK_OK(oct_link_input(r.link, data.data, data.length / 3, r.now));
    CHECK_OK(oct_link_cancel(r.link, cancelled));
    CHECK_EQ(oct_link_device_status(r.link), OCT_RC_TRANSACTION_CANCELLED);
    CHECK_EQ(call(&r, OCT_OP_GET_STORAGE_IDS, NULL, 0, NULL, 0, 0), OCT_RC_OK);
    CHECK_EQ(oct_link_device_status(r.link), OCT_RC_OK);
    CHECK_EQ(link_status_of(&r).cancels, 1);
    CHECK_EQ(children(&r, scratch), before);

    /* Half a command, then a stall. */
    uint64_t timeouts = link_status_of(&r).timeouts;
    oct_buf_clear(&command);
    oct_container_command(&command, OCT_OP_GET_STORAGE_IDS, r.tid, NULL, 0);
    CHECK_OK(oct_link_input(r.link, command.data, 5, r.now));
    r.now += 1000;
    CHECK_OK(oct_link_tick(r.link, r.now));
    CHECK_EQ(link_status_of(&r).timeouts, timeouts + 1);
    CHECK_EQ(call(&r, OCT_OP_GET_STORAGE_IDS, NULL, 0, NULL, 0, 0), OCT_RC_OK);

    /* After all that, a whole upload still works. */
    CHECK_EQ(call(&r, OCT_OP_SEND_OBJECT_INFO, where, 2, dataset.data, dataset.length, 9), OCT_RC_OK);
    CHECK_EQ(call(&r, OCT_OP_SEND_OBJECT, NULL, 0, "0123456789", 10, 4), OCT_RC_OK);
    CHECK_EQ(children(&r, scratch), before + 1);
    oct_buf_free(&dataset);
    oct_buf_free(&command);
    oct_buf_free(&data);
    rig_destroy(&r);
}

static void test_lifecycle(void)
{
    rig r;
    rig_create(&r, 0, false, false);
    CHECK_EQ(link_status_of(&r).state, OCT_TRANSPORT_UNAVAILABLE);
    uint8_t byte = 0;
    CHECK_EQ(oct_link_input(r.link, &byte, 1, r.now), OCT_E_STATE);
    CHECK_EQ(oct_link_host_attached(r.link), OCT_E_STATE);

    oct_loopback_set_failures(r.loopback, true, false);
    oct_service_config config = { SEED_TIME, false, false };
    CHECK(oct_service_start(r.service, &config) != OCT_OK);
    CHECK_EQ(oct_service_get_state(r.service), OCT_SERVICE_ERROR);
    CHECK_EQ(link_status_of(&r).state, OCT_TRANSPORT_ERROR);
    CHECK(strstr(link_status_of(&r).last_failure, "didn't start") != NULL);
    oct_service_status service_status;
    CHECK_OK(oct_service_get_status(r.service, &service_status));
    CHECK_EQ(service_status.transport_state, OCT_TRANSPORT_ERROR);
    CHECK(strstr(service_status.transport_failure, "didn't start") != NULL);
    char error[OCT_EVENT_TEXT_MAX];
    CHECK(oct_service_last_error(r.service, error, sizeof error));

    oct_loopback_set_failures(r.loopback, false, false);
    for (int cycle = 0; cycle < 3; cycle++) {
        CHECK_OK(oct_service_start(r.service, &config));
        CHECK_EQ(link_status_of(&r).state, OCT_TRANSPORT_WAITING);
        CHECK_EQ(oct_service_get_state(r.service), OCT_SERVICE_WAITING);
        CHECK_OK(oct_link_host_attached(r.link));
        CHECK_EQ(oct_service_get_state(r.service), OCT_SERVICE_CONNECTED);
        r.tid = 0;
        CHECK_EQ(call(&r, OCT_OP_GET_DEVICE_INFO, NULL, 0, NULL, 0, 0), OCT_RC_OK);
        CHECK_OK(oct_link_host_detached(r.link, "cycle"));
        CHECK_EQ(link_status_of(&r).state, OCT_TRANSPORT_DISCONNECTED);
        CHECK_EQ(oct_service_get_state(r.service), OCT_SERVICE_WAITING);
        CHECK_OK(oct_service_stop(r.service));
        CHECK_EQ(link_status_of(&r).state, OCT_TRANSPORT_UNAVAILABLE);
    }
    CHECK_EQ(link_status_of(&r).connects, 3);
    CHECK_EQ(link_status_of(&r).disconnects, 3);

    /* A send failure is counted and reported, and the link carries on. */
    CHECK_OK(oct_service_start(r.service, &config));
    CHECK_OK(oct_link_host_attached(r.link));
    oct_loopback_set_failures(r.loopback, false, true);
    CHECK_EQ(oct_link_transact(r.link, r.loopback, OCT_OP_GET_DEVICE_INFO, 0, NULL, 0, NULL, 0, 0, r.now, &r.reply),
             OCT_E_STATE);
    CHECK(link_status_of(&r).send_failures >= 1);
    CHECK(strstr(link_status_of(&r).last_failure, "Sending") != NULL);
    oct_loopback_set_failures(r.loopback, false, false);
    r.tid = 0;
    CHECK_EQ(call(&r, OCT_OP_GET_DEVICE_INFO, NULL, 0, NULL, 0, 0), OCT_RC_OK);

    /* A driver failure ends the connection; the service goes back to waiting. */
    oct_link_fail(r.link, "USB function removed");
    oct_link_status status = link_status_of(&r);
    CHECK_EQ(status.state, OCT_TRANSPORT_ERROR);
    CHECK(!status.host_attached);
    CHECK(strstr(status.last_failure, "USB function removed") != NULL);
    CHECK_EQ(oct_service_get_state(r.service), OCT_SERVICE_WAITING);
    CHECK_EQ(oct_link_input(r.link, &byte, 1, r.now), OCT_E_STATE);
    rig_destroy(&r);
}

static char *read_all(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    rewind(file);
    char *text = calloc(1, (size_t)(size > 0 ? size : 0) + 1);
    if (text && size > 0 && fread(text, 1, (size_t)size, file) != (size_t)size) text[0] = '\0';
    fclose(file);
    return text;
}

static void test_logging(void)
{
    char directory[] = "/tmp/octopus-link-XXXXXX", path[128];
    CHECK(mkdtemp(directory) != NULL);
    snprintf(path, sizeof path, "%s/octopus-debug.log", directory);
    oct_log_config config = { path, 0, 1, OCT_LOG_DEBUG };
    CHECK_OK(oct_log_open(&config));
    rig r;
    rig_create(&r, 0, false, true);
    CHECK_EQ(call(&r, OCT_OP_GET_DEVICE_INFO, NULL, 0, NULL, 0, 0), OCT_RC_OK);
    uint32_t handle = 1;
    CHECK_EQ(call(&r, OCT_OP_GET_OBJECT_INFO, &handle, 1, NULL, 0, 0), OCT_RC_SESSION_NOT_OPEN);
    uint8_t short_length[12] = { 4, 0, 0, 0, 1, 0, 0x01, 0x10, 0, 0, 0, 0 };
    oct_link_input(r.link, short_length, sizeof short_length, r.now);
    rig_destroy(&r);
    oct_log_close();
    char *text = read_all(path);
    CHECK(text != NULL);
    if (text) {
        CHECK(strstr(text, "INFO  linktest   state Waiting for a connection\n") != NULL);
        CHECK(strstr(text, "INFO  linktest   attach Host attached (connection 1)\n") != NULL);
        CHECK(strstr(text, "DEBUG linktest   transaction op=0x1001 GetDeviceInfo tid=0 rc=0x2001 OK in=12 out=") != NULL);
        CHECK(strstr(text, "INFO  linktest   transaction op=0x1008 GetObjectInfo tid=1 rc=0x2003 SessionNotOpen in=16") !=
              NULL);
        CHECK(strstr(text, "WARN  linktest   malformed A container declares 4 bytes, less than its header") != NULL);
        CHECK(strstr(text, "INFO  service    state Waiting for connection\n") != NULL);
        CHECK(strstr(text, " time=") != NULL);
        CHECK(strstr(text, "README") == NULL); /* object names never reach the log */
    }
    free(text);
    unlink(path);
    rmdir(directory);
    oct_log_config memory = { NULL, 0, 0, OCT_LOG_INFO };
    oct_log_open(&memory);
}

typedef struct race {
    rig *r;
    int  attempts;
    int  answered;
    bool until_refused; /* stop at the first refusal (the service was stopped) */
    bool refused;
} race;

static void *race_transactions(void *context)
{
    race *x = context;
    oct_link_reply reply = { 0 };
    for (int i = 0; i < x->attempts; i++) {
        oct_status status = oct_link_transact(x->r->link, x->r->loopback, OCT_OP_GET_DEVICE_INFO, 0, NULL, 0, NULL, 0,
                                              16, 1000, &reply);
        if (status == OCT_OK && reply.response == OCT_RC_OK) {
            x->answered++;
        } else if (x->until_refused) {
            x->refused = true;
            break;
        }
    }
    oct_buf_free(&reply.data);
    return NULL;
}

static void *race_queries(void *context)
{
    rig *r = context;
    for (int i = 0; i < 3000; i++) {
        oct_link_status status;
        oct_service_status service_status;
        oct_link_device_status(r->link);
        oct_link_get_status(r->link, &status);
        oct_service_get_status(r->service, &service_status);
    }
    return NULL;
}

static void test_concurrency(void)
{
    rig r;
    rig_create(&r, 0, false, true);
    race x = { &r, 300, 0, false, false };
    pthread_t transactions, queries;
    CHECK(pthread_create(&transactions, NULL, race_transactions, &x) == 0);
    CHECK(pthread_create(&queries, NULL, race_queries, &r) == 0);
    pthread_join(transactions, NULL);
    pthread_join(queries, NULL);
    CHECK_EQ(x.answered, 300);

    /* Stopping the service while the host is busy: later input is refused, and nothing hangs. */
    race busy = { &r, 50000000, 0, true, false }; /* far more than can finish before the stop */
    CHECK(pthread_create(&transactions, NULL, race_transactions, &busy) == 0);
    struct timespec delay = { 0, 2000000 };
    nanosleep(&delay, NULL);
    CHECK_OK(oct_service_stop(r.service));
    pthread_join(transactions, NULL);
    CHECK(busy.refused);
    CHECK(busy.answered > 0);
    CHECK_EQ(link_status_of(&r).state, OCT_TRANSPORT_UNAVAILABLE);
    rig_destroy(&r);
}

static void test_self_test(void)
{
    uint32_t checks = 0;
    char failure[OCT_EVENT_TEXT_MAX] = "unset";
    CHECK_OK(oct_link_self_test(&checks, failure, sizeof failure));
    CHECK(checks >= 40);
    CHECK_STR(failure, "");

    /* It runs as a phase of the service's own self-test, with its own line in the activity sheet. */
    oct_service *service = NULL;
    CHECK_OK(oct_service_create(&service));
    CHECK_OK(oct_service_self_test(service));
    oct_event events[256];
    size_t count = oct_service_drain_events(service, events, 256);
    bool found = false;
    for (size_t i = 0; i < count; i++) {
        if (events[i].kind == OCT_EVENT_STEP_END && strncmp(events[i].text, "Transport adapter: ", 19) == 0) {
            found = !events[i].failed;
        }
    }
    CHECK(found);
    oct_service_destroy(service);
}

void run_link_tests(void)
{
    test_fragmentation();
    test_framing();
    test_malformed();
    test_sessions();
    test_interrupted();
    test_lifecycle();
    test_logging();
    test_concurrency();
    test_self_test();
}
