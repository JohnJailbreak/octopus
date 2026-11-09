/*
 * test_bridge.c — the socket-based bridge transport between the Octopus responder
 * and an external integration client.
 *
 *  1. Wire encoding: encode/alloc round-trip, bad sizes.
 *  2. Ping/pong: client connects, pings, server responds with its socket path.
 *  3. Host lifecycle: attach, OpenSession and GetStorageIDs; the responder answers.
 *  4. Class requests: Get Device Status, a stalled request, Device Reset.
 *  5. Lifecycle messages: BUS_RESET, SUSPEND, RESUME.
 *  6. Unknown message: dropped without disconnecting.
 *  7. Client disconnect: the server serves a client that reconnects.
 *  8. The transport starts, stops, and starts again.
 *  9. The client leaves and the server handles it before the transport stops (the order that raced in CI).
 * 10. Stopping with an idle client connected returns promptly and closes the client.
 * 11. Clients that leave before reading their replies: no SIGPIPE, and the server still works.
 * 12. Frames from two writers (the server thread's replies, another thread's events) never interleave, and a
 *     client that stops reading is disconnected once a write times out.
 * 13. The self-test that Run Full Diagnostics runs, and its failure when no folder takes a socket.
 * 14. Bad arguments, a path nothing listens on, and a non-socket file at the socket path.
 *
 * Test checks run on the main thread only; helper threads report through their arguments.
 */
#include "test_support.h"

#include "../../Core/oct_bridge.h"
#include "../../Protocol/oct_ptp.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* One socket per test process, so concurrent runs (plain, sanitizer, TSan) never share it. */
static const char *test_socket(void)
{
    static char path[64];
    if (!path[0]) snprintf(path, sizeof path, "/tmp/oct-bridge-test-%ld.sock", (long)getpid());
    return path;
}
#define TEST_SOCKET test_socket()

static int64_t now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + (int64_t)(now.tv_nsec / 1000000);
}

static void pause_ms(long ms)
{
    struct timespec wait = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&wait, NULL);
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

/* Build an MTP command container for the test. */
static size_t command_container(uint16_t operation, uint32_t transaction_id, const uint32_t *params,
                                uint32_t param_count, uint8_t *out, size_t capacity)
{
    oct_buf b;
    oct_buf_init(&b, 0);
    oct_container_command(&b, operation, transaction_id, params, param_count);
    size_t length = b.length;
    if (length <= capacity && b.data) memcpy(out, b.data, length);
    else length = 0;
    oct_buf_free(&b);
    return length;
}

static oct_status send_command(oct_bridge_client *c, uint16_t operation, uint32_t transaction_id,
                               const uint32_t *params, uint32_t count)
{
    uint8_t cmd[32];
    size_t length = command_container(operation, transaction_id, params, count, cmd, sizeof cmd);
    return length ? oct_bridge_client_send_bulk(c, cmd, length) : OCT_E_INVALID_ARGUMENT;
}

/* Reads one BULK_TO_HOST container; returns its code (response code or operation), or 0. */
static uint16_t receive_container(oct_bridge_client *c, uint16_t *out_type)
{
    uint8_t type = 0;
    uint8_t *data = NULL;
    size_t length = 0;
    uint16_t code = 0;
    if (oct_bridge_client_receive(c, &type, &data, &length, 2000) == OCT_OK &&
        type == OCT_BRIDGE_MSG_BULK_TO_HOST && length >= 12) {
        if (out_type) *out_type = le16(data + 4);
        code = le16(data + 6);
    }
    free(data);
    return code;
}

/* One server + one or more client interactions, managed by the test. */
typedef struct {
    oct_bridge  *bridge;
    oct_service *service;
    oct_link    *link;
} test_rig;

static void rig_open(test_rig *r)
{
    CHECK_OK(oct_service_create(&r->service));
    CHECK_OK(oct_bridge_create(TEST_SOCKET, &r->bridge));
    oct_transport transport;
    CHECK_OK(oct_bridge_link_create(r->bridge, &r->link, &transport));
    CHECK_OK(oct_service_set_transport(r->service, &transport));
    oct_service_config config = { 0, false, false };
    CHECK_OK(oct_service_start(r->service, &config));
}

static void rig_close(test_rig *r)
{
    oct_service_stop(r->service);     /* stops the bridge: wakes and joins its thread */
    oct_service_destroy(r->service);
    oct_bridge_destroy(r->bridge);    /* nothing left to stop */
    oct_link_destroy(r->link);
    memset(r, 0, sizeof *r);
}

static bool wait_until_disconnected(oct_bridge *bridge)
{
    int64_t deadline = now_ms() + 2000;
    while (oct_bridge_client_connected(bridge) && now_ms() < deadline) pause_ms(5);
    return !oct_bridge_client_connected(bridge);
}

/* ---- 1. Wire encoding -------------------------------------------------------------------- */

static void test_encode(void)
{
    uint8_t buf[64];
    size_t n = oct_bridge_encode(OCT_BRIDGE_MSG_PING, NULL, 0, buf, sizeof buf);
    CHECK_EQ(n, 5u);
    CHECK_EQ(buf[0], OCT_BRIDGE_MSG_PING);
    CHECK_EQ(buf[1], 0); CHECK_EQ(buf[2], 0); CHECK_EQ(buf[3], 0); CHECK_EQ(buf[4], 0);

    n = oct_bridge_encode(OCT_BRIDGE_MSG_HOST_DETACHED, "hello", 5, buf, sizeof buf);
    CHECK_EQ(n, 10u);
    CHECK_EQ(buf[0], OCT_BRIDGE_MSG_HOST_DETACHED);
    CHECK_EQ(buf[1], 5);
    CHECK(memcmp(buf + 5, "hello", 5) == 0);

    CHECK_EQ(oct_bridge_encode(OCT_BRIDGE_MSG_PING, NULL, 0, buf, 4), 0);    /* too small */
    CHECK_EQ(oct_bridge_encode(OCT_BRIDGE_MSG_PING, NULL, 3, buf, sizeof buf), 0); /* length without payload */
    CHECK_EQ(oct_bridge_encode(OCT_BRIDGE_MSG_PING, buf, OCT_BRIDGE_MAX_PAYLOAD + 1, buf, sizeof buf), 0);

    size_t out_len = 0;
    uint8_t *alloc = oct_bridge_alloc(OCT_BRIDGE_MSG_PONG, "path", 4, &out_len);
    CHECK(alloc != NULL && out_len == 9);
    CHECK_EQ(alloc[0], OCT_BRIDGE_MSG_PONG);
    CHECK(memcmp(alloc + 5, "path", 4) == 0);
    free(alloc);
    CHECK(oct_bridge_alloc(OCT_BRIDGE_MSG_PONG, NULL, OCT_BRIDGE_MAX_PAYLOAD + 1, &out_len) == NULL);
}

/* ---- 2. Ping / pong ---------------------------------------------------------------------- */

static void test_ping(void)
{
    test_rig r;
    rig_open(&r);
    oct_bridge_client *c = NULL;
    CHECK_OK(oct_bridge_client_connect(TEST_SOCKET, 2000, &c));
    char info[128] = "";
    CHECK_OK(oct_bridge_client_ping(c, info, sizeof info, 1000));
    CHECK_STR(info, TEST_SOCKET);
    oct_bridge_client_destroy(c);
    rig_close(&r);
}

/* ---- 3. PTP command round-trip ----------------------------------------------------------- */

static void test_mtp_round_trip(void)
{
    test_rig r;
    rig_open(&r);
    oct_bridge_client *c = NULL;
    CHECK_OK(oct_bridge_client_connect(TEST_SOCKET, 2000, &c));
    CHECK_OK(oct_bridge_client_host_event(c, true, NULL));
    uint32_t session = 1;
    uint16_t type = 0;
    CHECK_OK(send_command(c, OCT_OP_OPEN_SESSION, 0, &session, 1));
    CHECK_EQ(receive_container(c, &type), OCT_RC_OK);
    CHECK_EQ(type, OCT_CONTAINER_RESPONSE);
    CHECK_OK(send_command(c, OCT_OP_GET_STORAGE_IDS, 1, NULL, 0));
    CHECK_EQ(receive_container(c, &type), OCT_OP_GET_STORAGE_IDS); /* the data phase */
    CHECK_EQ(type, OCT_CONTAINER_DATA);
    CHECK_EQ(receive_container(c, &type), OCT_RC_OK);              /* then the response */
    CHECK_EQ(type, OCT_CONTAINER_RESPONSE);
    CHECK_OK(oct_bridge_client_host_event(c, false, "test done"));
    oct_bridge_client_destroy(c);
    rig_close(&r);
}

/* ---- 4. Class requests ------------------------------------------------------------------- */

static void test_class_requests(void)
{
    test_rig r;
    rig_open(&r);
    oct_bridge_client *c = NULL;
    CHECK_OK(oct_bridge_client_connect(TEST_SOCKET, 2000, &c));
    CHECK_OK(oct_bridge_client_host_event(c, true, NULL));
    uint8_t reply[8] = { 0 };
    size_t reply_len = 0;
    bool stall = true;
    /* Get Device Status: completed, 4 bytes, code OK */
    CHECK_OK(oct_bridge_client_class_request(c, OCT_USB_GET_DEVICE_STATUS, NULL, 0, reply, sizeof reply,
                                             &reply_len, &stall));
    CHECK(!stall);
    CHECK_EQ(reply_len, 4u);
    CHECK_EQ(le16(reply + 2), OCT_RC_OK);
    /* A reply that doesn't fit the caller's buffer */
    CHECK_EQ(oct_bridge_client_class_request(c, OCT_USB_GET_DEVICE_STATUS, NULL, 0, reply, 2, &reply_len, &stall),
             OCT_E_TOO_LARGE);
    /* Get Extended Event Data: stalled */
    stall = false;
    CHECK_OK(oct_bridge_client_class_request(c, OCT_USB_GET_EXTENDED_EVENT_DATA, NULL, 0, reply, sizeof reply,
                                             &reply_len, &stall));
    CHECK(stall);
    /* Device Reset has no data stage: completed (acknowledged), not stalled */
    stall = true;
    CHECK_OK(oct_bridge_client_class_request(c, OCT_USB_DEVICE_RESET_REQUEST, NULL, 0, reply, sizeof reply,
                                             &reply_len, &stall));
    CHECK(!stall);
    CHECK_EQ(reply_len, 0u);
    /* After the reset there is no session */
    CHECK_OK(send_command(c, OCT_OP_GET_STORAGE_IDS, 1, NULL, 0));
    CHECK_EQ(receive_container(c, NULL), OCT_RC_SESSION_NOT_OPEN);
    CHECK_OK(oct_bridge_client_host_event(c, false, NULL));
    oct_bridge_client_destroy(c);
    rig_close(&r);
}

/* ---- 5. Lifecycle messages --------------------------------------------------------------- */

static void test_lifecycle(void)
{
    test_rig r;
    rig_open(&r);
    oct_bridge_client *c = NULL;
    CHECK_OK(oct_bridge_client_connect(TEST_SOCKET, 2000, &c));
    CHECK_OK(oct_bridge_client_host_event(c, true, NULL));
    CHECK_OK(oct_bridge_client_lifecycle(c, OCT_BRIDGE_MSG_SUSPEND));
    CHECK_OK(oct_bridge_client_lifecycle(c, OCT_BRIDGE_MSG_RESUME));
    CHECK_OK(oct_bridge_client_lifecycle(c, OCT_BRIDGE_MSG_BUS_RESET));
    CHECK_EQ(oct_bridge_client_lifecycle(c, 0xFE), OCT_E_INVALID_ARGUMENT);
    char info[128] = "";
    CHECK_OK(oct_bridge_client_ping(c, info, sizeof info, 1000)); /* all of them handled, still connected */
    CHECK_OK(oct_bridge_client_host_event(c, false, NULL));
    oct_bridge_client_destroy(c);
    rig_close(&r);
}

/* ---- 6. Unknown message is dropped ------------------------------------------------------- */

static void test_unknown_message(void)
{
    test_rig r;
    rig_open(&r);
    oct_bridge_client *c = NULL;
    CHECK_OK(oct_bridge_client_connect(TEST_SOCKET, 2000, &c));
    CHECK_OK(oct_bridge_client_send_frame(c, 0xEF, NULL, 0));
    CHECK_OK(oct_bridge_client_send_frame(c, 0xEE, "payload", 7));
    char info[128] = "";
    CHECK_OK(oct_bridge_client_ping(c, info, sizeof info, 1000)); /* the connection survived both */
    CHECK_STR(info, TEST_SOCKET);
    CHECK(oct_bridge_client_connected(r.bridge));
    oct_bridge_client_destroy(c);
    rig_close(&r);
}

/* ---- 7. Client disconnect and reconnect -------------------------------------------------- */

static void test_reconnect(void)
{
    test_rig r;
    rig_open(&r);
    oct_bridge_client *c1 = NULL, *c2 = NULL;
    CHECK_OK(oct_bridge_client_connect(TEST_SOCKET, 2000, &c1));
    CHECK_OK(oct_bridge_client_host_event(c1, true, NULL));
    oct_bridge_client_destroy(c1); /* without a detach: the server cleans up */
    CHECK_OK(oct_bridge_client_connect(TEST_SOCKET, 2000, &c2));
    char info[128] = "";
    CHECK_OK(oct_bridge_client_ping(c2, info, sizeof info, 2000));
    CHECK(info[0] != '\0');
    oct_bridge_client_destroy(c2);
    rig_close(&r);
}

/* ---- 8. Transport stop-start cycle ------------------------------------------------------- */

static void test_restart(void)
{
    test_rig r;
    rig_open(&r);
    oct_bridge_client *c = NULL;
    CHECK_OK(oct_bridge_client_connect(TEST_SOCKET, 2000, &c));
    char info[128] = "";
    CHECK_OK(oct_bridge_client_ping(c, info, sizeof info, 1000));
    oct_bridge_client_destroy(c);
    oct_service_stop(r.service);
    struct stat st;
    CHECK(lstat(TEST_SOCKET, &st) != 0); /* the stop removed the socket */
    oct_link_destroy(r.link);
    r.link = NULL;
    oct_transport transport;
    CHECK_OK(oct_bridge_link_create(r.bridge, &r.link, &transport));
    CHECK_OK(oct_service_set_transport(r.service, &transport));
    oct_service_config config = { 0, false, false };
    CHECK_OK(oct_service_start(r.service, &config));
    CHECK_OK(oct_bridge_client_connect(TEST_SOCKET, 2000, &c));
    CHECK_OK(oct_bridge_client_ping(c, info, sizeof info, 1000));
    oct_bridge_client_destroy(c);
    rig_close(&r);
}

/* ---- 9. The client leaves, then the transport stops ------------------------------------- */

static void test_disconnect_then_stop(void)
{
    for (int round = 0; round < 3; round++) {
        test_rig r;
        rig_open(&r);
        oct_bridge_client *c = NULL;
        CHECK_OK(oct_bridge_client_connect(TEST_SOCKET, 2000, &c));
        CHECK_OK(oct_bridge_client_host_event(c, true, NULL));
        if (round == 1) CHECK_OK(oct_bridge_client_host_event(c, false, "leaving"));
        char info[128] = "";
        CHECK_OK(oct_bridge_client_ping(c, info, sizeof info, 1000)); /* the server has read everything */
        oct_bridge_client_destroy(c);
        CHECK(wait_until_disconnected(r.bridge)); /* the server thread handles the departure first */
        rig_close(&r);                           /* then the stop clears the link */
    }
}

/* ---- 10. Stop with an idle client connected ---------------------------------------------- */

static void test_stop_with_idle_client(void)
{
    test_rig r;
    rig_open(&r);
    oct_bridge_client *c = NULL;
    CHECK_OK(oct_bridge_client_connect(TEST_SOCKET, 2000, &c));
    char info[128] = "";
    CHECK_OK(oct_bridge_client_ping(c, info, sizeof info, 1000));
    CHECK(oct_bridge_client_connected(r.bridge)); /* the server thread is now waiting on this client */
    int64_t started = now_ms();
    oct_service_stop(r.service);
    int64_t took = now_ms() - started;
    if (took >= 900) fprintf(stderr, "stopping with an idle client took %lld ms\n", (long long)took);
    CHECK(took < 900); /* woken, not left to a timeout */
    CHECK(!oct_bridge_client_connected(r.bridge));
    uint8_t type = 0;
    uint8_t *data = NULL;
    size_t length = 0;
    CHECK_EQ(oct_bridge_client_receive(c, &type, &data, &length, 1000), OCT_E_IO); /* closed, not a timeout */
    free(data);
    oct_bridge_client_destroy(c);
    rig_close(&r);
}

/* ---- 11. Clients that leave before reading their replies ---------------------------------- */

static void test_vanishing_clients(void)
{
    test_rig r;
    rig_open(&r);
    int failures = 0;
    for (int i = 0; i < 30; i++) {
        oct_bridge_client *c = NULL;
        if (oct_bridge_client_connect(TEST_SOCKET, 2000, &c) != OCT_OK) {
            failures++;
            continue;
        }
        uint32_t session = 1;
        if (oct_bridge_client_host_event(c, true, NULL) != OCT_OK ||
            send_command(c, OCT_OP_OPEN_SESSION, 0, &session, 1) != OCT_OK ||
            oct_bridge_client_send_frame(c, OCT_BRIDGE_MSG_PING, NULL, 0) != OCT_OK) {
            failures++;
        }
        oct_bridge_client_destroy(c); /* the server's replies go to a closed peer: EPIPE, never SIGPIPE */
    }
    CHECK_EQ(failures, 0);
    /* The process is still here, and a well-behaved client gets a whole session. */
    oct_bridge_client *c = NULL;
    CHECK_OK(oct_bridge_client_connect(TEST_SOCKET, 2000, &c));
    CHECK_OK(oct_bridge_client_host_event(c, true, NULL));
    uint32_t session = 2;
    CHECK_OK(send_command(c, OCT_OP_OPEN_SESSION, 0, &session, 1));
    CHECK_EQ(receive_container(c, NULL), OCT_RC_OK);
    CHECK_OK(send_command(c, OCT_OP_CLOSE_SESSION, 1, NULL, 0));
    CHECK_EQ(receive_container(c, NULL), OCT_RC_OK);
    oct_bridge_client_destroy(c);
    rig_close(&r);
}

/* ---- 12. Two writers, whole frames ------------------------------------------------------- */

typedef struct {
    oct_link *link;
    int       wanted;
    int       sent;
} event_sender;

static void *send_events(void *arg)
{
    event_sender *e = arg;
    for (int i = 0; i < e->wanted; i++) {
        if (oct_link_send_event(e->link, 0x4008 /* DeviceInfoChanged */, 0, NULL, 0) == OCT_OK) e->sent++;
    }
    return NULL;
}

static void test_frames_never_interleave(void)
{
    test_rig r;
    rig_open(&r);
    oct_bridge_client *c = NULL;
    CHECK_OK(oct_bridge_client_connect(TEST_SOCKET, 2000, &c));
    CHECK_OK(oct_bridge_client_host_event(c, true, NULL));
    int64_t deadline = now_ms() + 2000; /* the server thread attaches the host asynchronously */
    oct_status first = OCT_E_STATE;
    while ((first = oct_link_send_event(r.link, 0x4008, 0, NULL, 0)) == OCT_E_STATE && now_ms() < deadline) {
        pause_ms(2);
    }
    CHECK_OK(first);
    event_sender sender = { r.link, 200, 0 };
    pthread_t thread;
    CHECK_EQ(pthread_create(&thread, NULL, send_events, &sender), 0);
    const int pings = 50;
    for (int i = 0; i < pings; i++) CHECK_OK(oct_bridge_client_send_frame(c, OCT_BRIDGE_MSG_PING, NULL, 0));
    /* Read while the other thread writes: pongs from the server thread and events from that one share the
     * socket, and every frame must arrive whole. */
    int pongs = 0, events = 0, malformed = 0;
    const int expected_events = 1 + sender.wanted;
    deadline = now_ms() + 10000;
    while ((pongs < pings || events < expected_events) && now_ms() < deadline) {
        uint8_t type = 0;
        uint8_t *data = NULL;
        size_t length = 0;
        if (oct_bridge_client_receive(c, &type, &data, &length, 2000) != OCT_OK) break;
        if (type == OCT_BRIDGE_MSG_PONG && length == strlen(TEST_SOCKET) && memcmp(data, TEST_SOCKET, length) == 0) {
            pongs++;
        } else if (type == OCT_BRIDGE_MSG_INTERRUPT_EVENT && length == 12 && data[0] == 12 &&
                   le16(data + 4) == OCT_CONTAINER_EVENT && le16(data + 6) == 0x4008) {
            events++;
        } else {
            malformed++;
        }
        free(data);
    }
    pthread_join(thread, NULL);
    CHECK_EQ(malformed, 0);
    CHECK_EQ(pongs, pings);
    CHECK_EQ(sender.sent, sender.wanted);
    CHECK_EQ(events, expected_events);
    CHECK(oct_bridge_client_connected(r.bridge));
    oct_bridge_client_destroy(c);
    rig_close(&r);
}

/* ---- 12b. A client that stops reading is disconnected, not waited on forever --------------- */

static void test_stalled_reader(void)
{
    test_rig r;
    rig_open(&r);
    oct_bridge_client *c = NULL;
    CHECK_OK(oct_bridge_client_connect(TEST_SOCKET, 2000, &c));
    CHECK_OK(oct_bridge_client_host_event(c, true, NULL));
    int64_t deadline = now_ms() + 2000;
    oct_status status = OCT_E_STATE;
    while ((status = oct_link_send_event(r.link, 0x4008, 0, NULL, 0)) == OCT_E_STATE && now_ms() < deadline) {
        pause_ms(2);
    }
    CHECK_OK(status);
    /* The client never reads. Events fill the socket until a write times out; then the server drops it. */
    int64_t started = now_ms();
    int sent = 0;
    while (now_ms() - started < 20000) {
        status = oct_link_send_event(r.link, 0x4008, 0, NULL, 0);
        if (status != OCT_OK) break;
        sent++;
    }
    CHECK(status != OCT_OK); /* a timeout, then no client */
    CHECK(sent > 0);
    CHECK(wait_until_disconnected(r.bridge));
    oct_bridge_client_destroy(c);
    rig_close(&r);
}

/* ---- 13. The diagnostics self-test ------------------------------------------------------- */

static void test_self_test(void)
{
    oct_compat_report report;
    oct_status status = oct_bridge_self_test(NULL, &report);
    if (status != OCT_OK) fprintf(stderr, "bridge self-test: %s\n", report.failure);
    CHECK_OK(status);
    CHECK(report.checks >= 12);
    CHECK(strstr(report.summary, "checks on a socket in /tmp") != NULL);
    CHECK(report.failure[0] == '\0');
    CHECK_EQ(oct_bridge_self_test("/nonexistent-octopus-folder", &report), OCT_E_IO);
    CHECK(strstr(report.failure, "/nonexistent-octopus-folder") != NULL);
    CHECK_EQ(oct_bridge_self_test(NULL, NULL), OCT_E_INVALID_ARGUMENT);
}

/* ---- 14. Bad arguments and paths --------------------------------------------------------- */

static void test_bad_arguments(void)
{
    char long_path[200];
    memset(long_path, 'a', sizeof long_path - 1);
    long_path[0] = '/';
    long_path[sizeof long_path - 1] = '\0';
    oct_bridge *b = NULL;
    oct_bridge_client *c = NULL;
    CHECK_EQ(oct_bridge_create(long_path, &b), OCT_E_INVALID_ARGUMENT);
    CHECK_EQ(oct_bridge_create(TEST_SOCKET, NULL), OCT_E_INVALID_ARGUMENT);
    CHECK_EQ(oct_bridge_client_connect(long_path, 100, &c), OCT_E_INVALID_ARGUMENT);
    CHECK(!oct_bridge_client_connected(NULL));
    CHECK_EQ(oct_bridge_client_send_frame(NULL, OCT_BRIDGE_MSG_PING, NULL, 0), OCT_E_INVALID_ARGUMENT);
    CHECK_EQ(oct_bridge_client_ping(NULL, NULL, 0, 100), OCT_E_INVALID_ARGUMENT);
    oct_bridge_destroy(NULL);
    oct_bridge_client_destroy(NULL);

    /* Nothing listens: the connect gives up with OCT_E_STATE once its timeout passes. */
    unlink(TEST_SOCKET);
    int64_t started = now_ms();
    CHECK_EQ(oct_bridge_client_connect(TEST_SOCKET, 150, &c), OCT_E_STATE);
    CHECK(now_ms() - started >= 100);
    CHECK(c == NULL);

    /* A file that isn't a socket is never replaced: the transport doesn't start, the file stays. */
    FILE *file = fopen(TEST_SOCKET, "w");
    CHECK(file != NULL);
    if (file) {
        fputs("not a socket", file);
        fclose(file);
    }
    oct_service *service = NULL;
    oct_link *link = NULL;
    oct_transport transport;
    CHECK_OK(oct_service_create(&service));
    CHECK_OK(oct_bridge_create(TEST_SOCKET, &b));
    CHECK_OK(oct_bridge_link_create(b, &link, &transport));
    CHECK_OK(oct_service_set_transport(service, &transport));
    oct_service_config config = { 0, false, false };
    CHECK(oct_service_start(service, &config) != OCT_OK);
    struct stat st;
    CHECK(lstat(TEST_SOCKET, &st) == 0 && S_ISREG(st.st_mode));
    oct_service_stop(service);
    oct_service_destroy(service);
    oct_bridge_destroy(b);
    oct_link_destroy(link);
    unlink(TEST_SOCKET);
}

void run_bridge_tests(void)
{
    test_encode();
    test_ping();
    test_mtp_round_trip();
    test_class_requests();
    test_lifecycle();
    test_unknown_message();
    test_reconnect();
    test_restart();
    test_disconnect_then_stop();
    test_stop_with_idle_client();
    test_vanishing_clients();
    test_frames_never_interleave();
    test_stalled_reader();
    test_self_test();
    test_bad_arguments();
}
