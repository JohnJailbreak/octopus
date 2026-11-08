#include "test_support.h"

#include <pthread.h>

typedef struct loopback {
    int  starts;
    int  stops;
    bool fail;
} loopback;

static oct_status loopback_start(void *context, oct_service *service)
{
    loopback *l = context;
    (void)service;
    l->starts++;
    return l->fail ? OCT_E_IO : OCT_OK;
}

static void loopback_stop(void *context) { ((loopback *)context)->stops++; }

static const oct_transport_ops loopback_ops = { "Loopback", loopback_start, loopback_stop, NULL };

static oct_event events[256];

static size_t drain(oct_service *service) { return oct_service_drain_events(service, events, 256); }

/* Index of the first event of `kind` whose text starts with `prefix`, or -1. */
static int find_event(size_t count, oct_event_kind kind, const char *prefix)
{
    for (size_t i = 0; i < count; i++) {
        if (events[i].kind == kind && strncmp(oct_event_text(&events[i]), prefix, strlen(prefix)) == 0) return (int)i;
    }
    return -1;
}

static bool any_failed_step(size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (events[i].kind == OCT_EVENT_STEP_END && events[i].failed) return true;
    }
    return false;
}

static oct_service *create_service(void)
{
    oct_service *service = NULL;
    CHECK_OK(oct_service_create(&service));
    CHECK_OK(oct_service_set_identity(service, "Test iPhone", "TEST-0001", "17.0"));
    return service;
}

static void test_lifecycle(void)
{
    oct_service *service = create_service();
    CHECK_EQ(oct_service_get_state(service), OCT_SERVICE_DISABLED);
    oct_service_config config = { SEED_TIME, false, true };
    CHECK_OK(oct_service_start(service, &config));
    CHECK_EQ(oct_service_get_state(service), OCT_SERVICE_READY);
    size_t count = drain(service);
    struct { oct_event_kind kind; const char *text; bool failed; } expected[] = {
        { OCT_EVENT_OPERATION_BEGIN, "Enabling", false },
        { OCT_EVENT_STATE, "Starting", false },
        { OCT_EVENT_STEP_BEGIN, "Preparing Octopus storage", false },
        { OCT_EVENT_STEP_END, "Octopus storage: 6 objects, 4.", false },
        { OCT_EVENT_STEP_BEGIN, "Starting protocol responder", false },
        { OCT_EVENT_STEP_END, "Responder ready: 20 operations", false },
        { OCT_EVENT_STEP_BEGIN, "Running protocol self-test", false },
        { OCT_EVENT_STEP_END, "Self-test passed: ", false },
        { OCT_EVENT_STEP_BEGIN, "Checking USB transport", false },
        { OCT_EVENT_STEP_END, "No USB transport in this build", true },
        { OCT_EVENT_STATE, "Ready", false },
        { OCT_EVENT_OPERATION_END, "Ready \xC2\xB7 local service only", false },
    };
    size_t matched = 0;
    uint32_t last_sequence = 0, open_step = 0;
    for (size_t i = 0; i < count; i++) {
        CHECK(events[i].sequence > last_sequence);
        last_sequence = events[i].sequence;
        CHECK(events[i].state != OCT_SERVICE_WAITING && events[i].state != OCT_SERVICE_CONNECTED);
        if (events[i].kind == OCT_EVENT_LOG) continue;
        if (events[i].kind == OCT_EVENT_STEP_BEGIN) open_step = events[i].step;
        if (events[i].kind == OCT_EVENT_STEP_END) CHECK_EQ(events[i].step, open_step);
        if (matched < sizeof expected / sizeof expected[0]) {
            CHECK_EQ(events[i].kind, expected[matched].kind);
            CHECK(strncmp(oct_event_text(&events[i]), expected[matched].text, strlen(expected[matched].text)) == 0);
            CHECK_EQ(events[i].failed, expected[matched].failed);
        }
        matched++;
    }
    CHECK_EQ(matched, sizeof expected / sizeof expected[0]);
    CHECK(find_event(count, OCT_EVENT_LOG, "! No USB transport") >= 0);
    oct_service_status status;
    CHECK_OK(oct_service_get_status(service, &status));
    CHECK(!status.transport_available && !status.session_open && !status.scratch);
    CHECK_EQ(status.object_count, 6);
    CHECK_EQ(status.operation_count, 20);
    CHECK_EQ(status.storage_id, OCT_DEFAULT_STORAGE_ID);
    CHECK_EQ(status.starts, 1);
    oct_storage_info storage;
    CHECK_OK(oct_service_storage_info(service, &storage));
    CHECK_STR(storage.description, "Octopus");
    CHECK_EQ(storage.access_capability, 0x0001);
    oct_object_info items[8];
    size_t total = 0;
    CHECK_OK(oct_service_list(service, OCT_ROOT, items, 8, &total));
    CHECK_EQ(total, 3);
    CHECK_STR(oct_object_info_name(&items[0]), "README.txt");
    CHECK_STR(oct_object_info_name(&items[2]), "Information");
    CHECK_OK(oct_service_list(service, OCT_ROOT, items, 1, &total));
    CHECK_EQ(total, 3); /* total reported even when the buffer is smaller */
    char text[32] = { 0 };
    size_t read = 0;
    CHECK_OK(oct_service_read(service, items[0].handle, 0, text, 25, &read));
    CHECK_STR(text, "Octopus synthetic storage");
    CHECK(!oct_service_last_error(service, text, sizeof text));
    CHECK_OK(oct_service_start(service, &config)); /* idempotent */
    count = drain(service);
    CHECK_EQ(count, 1);
    CHECK(find_event(count, OCT_EVENT_LOG, "Service is already running") == 0);
    CHECK_EQ(oct_service_get_state(service), OCT_SERVICE_READY);
    CHECK_OK(oct_service_stop(service));
    CHECK_EQ(oct_service_get_state(service), OCT_SERVICE_DISABLED);
    count = drain(service);
    CHECK(find_event(count, OCT_EVENT_OPERATION_BEGIN, "Disabling") == 0);
    CHECK(find_event(count, OCT_EVENT_STEP_END, "No host session was open") > 0);
    CHECK(find_event(count, OCT_EVENT_STEP_END, "Responder stopped after 0 requests") > 0);
    CHECK(find_event(count, OCT_EVENT_STEP_END, "Storage released") > 0);
    CHECK(find_event(count, OCT_EVENT_STATE, "Disabled") > 0);
    CHECK(find_event(count, OCT_EVENT_OPERATION_END, "USB access disabled") > 0);
    CHECK(!any_failed_step(count));
    CHECK_OK(oct_service_stop(service)); /* idempotent */
    CHECK_EQ(drain(service), 0);
    CHECK_EQ(oct_service_storage_info(service, &storage), OCT_E_STATE);
    CHECK_EQ(oct_service_list(service, OCT_ROOT, items, 8, &total), OCT_E_STATE);
    CHECK_EQ(oct_service_read(service, 1, 0, text, 4, &read), OCT_E_STATE);
    oct_service_destroy(service);
}

static void test_repeated_cycles(void)
{
    oct_service *service = create_service();
    oct_service_config quick = { SEED_TIME, false, false }, full = { SEED_TIME, true, true };
    bool consistent = true;
    for (int i = 0; i < 200; i++) {
        consistent = consistent && oct_service_start(service, i % 50 == 0 ? &full : &quick) == OCT_OK;
        consistent = consistent && oct_service_get_state(service) == OCT_SERVICE_READY;
        consistent = consistent && oct_service_stop(service) == OCT_OK;
        consistent = consistent && oct_service_get_state(service) == OCT_SERVICE_DISABLED;
        drain(service);
    }
    CHECK(consistent);
    oct_service_status status;
    CHECK_OK(oct_service_get_status(service, &status));
    CHECK_EQ(status.starts, 200);
    CHECK_EQ(status.stops, 200);
    CHECK_EQ(status.dropped_events, 0);
    CHECK_OK(oct_service_start(service, &quick));
    oct_service_destroy(service); /* destroying a running service stops it */
}

static void test_self_test(void)
{
    oct_service *service = create_service();
    CHECK_OK(oct_service_self_test(service));
    size_t count = drain(service);
    CHECK(find_event(count, OCT_EVENT_OPERATION_BEGIN, "Testing") == 0);
    CHECK(find_event(count, OCT_EVENT_STEP_BEGIN, "Preparing temporary storage") > 0);
    CHECK(find_event(count, OCT_EVENT_STEP_END, "Device information: MTP, 20 operations") > 0);
    CHECK(find_event(count, OCT_EVENT_STEP_BEGIN, "Writing in Scratch") > 0);
    CHECK(find_event(count, OCT_EVENT_STEP_END, "Scratch: created, read, renamed and deleted") > 0);
    CHECK(find_event(count, OCT_EVENT_OPERATION_END, "Self-test passed: ") > 0);
    CHECK(!any_failed_step(count));
    CHECK_EQ(oct_service_get_state(service), OCT_SERVICE_DISABLED);
    oct_service_config config = { SEED_TIME, false, false };
    CHECK_OK(oct_service_start(service, &config));
    drain(service);
    CHECK_OK(oct_service_self_test(service));
    count = drain(service);
    CHECK(find_event(count, OCT_EVENT_STEP_END, "Read-only storage refused every write") > 0);
    CHECK(find_event(count, OCT_EVENT_OPERATION_END, "Self-test passed: ") > 0);
    CHECK(!any_failed_step(count));
    CHECK_OK(oct_service_stop(service));
    config.scratch = true;
    CHECK_OK(oct_service_start(service, &config));
    CHECK_OK(oct_service_self_test(service));
    count = drain(service);
    CHECK(find_event(count, OCT_EVENT_STEP_END, "Scratch: created, read, renamed and deleted") > 0);
    oct_object_info items[8];
    size_t total = 0;
    CHECK_OK(oct_service_list(service, OCT_ROOT, items, 8, &total));
    CHECK_EQ(total, 4);
    CHECK_STR(oct_object_info_name(&items[3]), "Scratch");
    CHECK_OK(oct_service_list(service, items[3].handle, items, 8, &total));
    CHECK_EQ(total, 0); /* the self-test leaves Scratch empty */
    oct_service_status status;
    CHECK_OK(oct_service_get_status(service, &status));
    CHECK(status.scratch);
    oct_service_destroy(service);
}

static void test_transport_states(void)
{
    oct_service *service = create_service();
    loopback good = { 0, 0, false }, bad = { 0, 0, true };
    oct_transport transport = { &loopback_ops, &good }, failing = { &loopback_ops, &bad };
    oct_service_config config = { SEED_TIME, false, false };
    CHECK_EQ(oct_service_host_attached(service), OCT_E_STATE);
    CHECK_OK(oct_service_set_transport(service, &transport));
    CHECK_OK(oct_service_start(service, &config));
    CHECK_EQ(oct_service_get_state(service), OCT_SERVICE_WAITING);
    size_t count = drain(service);
    CHECK(find_event(count, OCT_EVENT_STATE, "Waiting for connection") > 0);
    CHECK(find_event(count, OCT_EVENT_STEP_END, "Loopback transport listening") > 0);
    CHECK(find_event(count, OCT_EVENT_OPERATION_END, "Waiting for a host") > 0);
    CHECK_EQ(good.starts, 1);
    CHECK_EQ(oct_service_set_transport(service, NULL), OCT_E_STATE);
    CHECK_EQ(oct_service_host_detached(service), OCT_E_STATE);
    oct_buf request, reply;
    oct_buf_init(&request, 0);
    oct_buf_init(&reply, 0);
    oct_container_command(&request, OCT_OP_GET_DEVICE_INFO, 0, NULL, 0);
    CHECK_EQ(oct_service_receive(service, request.data, request.length, &reply), OCT_E_STATE);
    CHECK_OK(oct_service_host_attached(service));
    CHECK_EQ(oct_service_get_state(service), OCT_SERVICE_CONNECTED);
    CHECK_EQ(oct_service_self_test(service), OCT_E_STATE);
    CHECK_OK(oct_service_receive(service, request.data, request.length, &reply));
    CHECK(reply.length > 24 && reply.data[reply.length - 6] == 0x01 && reply.data[reply.length - 5] == 0x20);
    oct_buf_clear(&request);
    oct_buf_clear(&reply);
    uint32_t session = 3;
    oct_container_command(&request, OCT_OP_OPEN_SESSION, 0, &session, 1);
    CHECK_OK(oct_service_receive(service, request.data, request.length, &reply));
    oct_service_status status;
    CHECK_OK(oct_service_get_status(service, &status));
    CHECK(status.transport_available && status.session_open && status.session_id == 3);
    CHECK_STR(status.transport_name, "Loopback");
    CHECK_EQ(status.transactions, 2);
    CHECK_OK(oct_service_host_detached(service));
    CHECK_EQ(oct_service_get_state(service), OCT_SERVICE_WAITING);
    CHECK_OK(oct_service_get_status(service, &status));
    CHECK(!status.session_open); /* detaching resets the session */
    count = drain(service);
    CHECK(find_event(count, OCT_EVENT_STATE, "Connected") >= 0);
    CHECK(find_event(count, OCT_EVENT_LOG, "Host disconnected") >= 0);
    CHECK_OK(oct_service_host_attached(service));
    CHECK_OK(oct_service_stop(service)); /* connected straight to disabled */
    CHECK_EQ(oct_service_get_state(service), OCT_SERVICE_DISABLED);
    CHECK_EQ(good.stops, 1);
    count = drain(service);
    CHECK(find_event(count, OCT_EVENT_STEP_END, "Transport stopped") > 0);

    CHECK_OK(oct_service_set_transport(service, &failing));
    CHECK_EQ(oct_service_start(service, &config), OCT_E_IO);
    CHECK_EQ(oct_service_get_state(service), OCT_SERVICE_ERROR);
    char error[OCT_EVENT_TEXT_MAX];
    CHECK(oct_service_last_error(service, error, sizeof error));
    CHECK_STR(error, "Loopback transport failed: I/O error");
    count = drain(service);
    CHECK(find_event(count, OCT_EVENT_STATE, "Error") > 0);
    int failed_end = find_event(count, OCT_EVENT_OPERATION_END, "Couldn't enable USB access");
    CHECK(failed_end > 0 && events[failed_end].failed);
    oct_storage_info storage;
    CHECK_EQ(oct_service_storage_info(service, &storage), OCT_E_STATE); /* resources released */
    CHECK_OK(oct_service_set_transport(service, &transport));          /* allowed in Error */
    CHECK_OK(oct_service_start(service, &config));                     /* retry from Error */
    CHECK_EQ(oct_service_get_state(service), OCT_SERVICE_WAITING);
    CHECK(!oct_service_last_error(service, error, sizeof error));
    CHECK_OK(oct_service_stop(service));
    CHECK_OK(oct_service_set_transport(service, &failing));
    CHECK_EQ(oct_service_start(service, &config), OCT_E_IO);
    drain(service);
    CHECK_OK(oct_service_stop(service)); /* Error to Disabled */
    CHECK_EQ(oct_service_get_state(service), OCT_SERVICE_DISABLED);
    count = drain(service);
    CHECK(find_event(count, OCT_EVENT_STEP_END, "Service reset") > 0);
    CHECK(!oct_service_last_error(service, error, sizeof error));
    CHECK_OK(oct_service_set_transport(service, NULL));
    CHECK_OK(oct_service_start(service, &config));
    CHECK_EQ(oct_service_get_state(service), OCT_SERVICE_READY);
    CHECK_EQ(oct_service_host_attached(service), OCT_E_STATE); /* no transport, no connection */
    oct_buf_free(&request);
    oct_buf_free(&reply);
    oct_service_destroy(service);
}

static void *poll_service(void *argument)
{
    oct_service *service = argument;
    oct_event batch[8];
    for (int i = 0; i < 20000; i++) {
        oct_service_status status;
        oct_storage_info storage;
        oct_object_info items[4];
        size_t total = 0;
        oct_service_get_status(service, &status);
        (void)oct_service_get_state(service);
        oct_service_storage_info(service, &storage);
        oct_service_list(service, OCT_ROOT, items, 4, &total);
        oct_service_drain_events(service, batch, 8);
    }
    return NULL;
}

static void test_concurrent_access(void)
{
    oct_service *service = create_service();
    pthread_t poller;
    CHECK_EQ(pthread_create(&poller, NULL, poll_service, service), 0);
    oct_service_config config = { SEED_TIME, true, false };
    bool consistent = true;
    for (int i = 0; i < 100; i++) {
        consistent = consistent && oct_service_start(service, &config) == OCT_OK;
        consistent = consistent && oct_service_stop(service) == OCT_OK;
    }
    pthread_join(poller, NULL);
    CHECK(consistent);
    CHECK_EQ(oct_service_get_state(service), OCT_SERVICE_DISABLED);
    oct_service_destroy(service);
}

static void test_event_overflow_and_arguments(void)
{
    oct_service *service = create_service();
    for (int i = 0; i < 10; i++) CHECK_OK(oct_service_self_test(service));
    oct_service_status status;
    CHECK_OK(oct_service_get_status(service, &status));
    CHECK(status.dropped_events > 0);
    size_t count = drain(service);
    CHECK_EQ(count, 256);
    bool ordered = true;
    for (size_t i = 1; i < count; i++) ordered = ordered && events[i].sequence == events[i - 1].sequence + 1;
    CHECK(ordered);
    CHECK_EQ(drain(service), 0);
    oct_service_destroy(service);
    CHECK_EQ(oct_service_create(NULL), OCT_E_INVALID_ARGUMENT);
    CHECK_EQ(oct_service_start(NULL, NULL), OCT_E_INVALID_ARGUMENT);
    CHECK_EQ(oct_service_stop(NULL), OCT_E_INVALID_ARGUMENT);
    CHECK_EQ(oct_service_get_state(NULL), OCT_SERVICE_DISABLED);
    oct_transport incomplete = { NULL, NULL };
    service = create_service();
    CHECK_EQ(oct_service_set_transport(service, &incomplete), OCT_E_INVALID_ARGUMENT);
    CHECK_OK(oct_service_start(service, NULL)); /* defaults: self-test on, no Scratch */
    CHECK_EQ(oct_service_get_state(service), OCT_SERVICE_READY);
    oct_service_destroy(service);
    oct_service_destroy(NULL);
    CHECK_STR(oct_service_state_name(OCT_SERVICE_WAITING), "Waiting for connection");
}

void run_service_tests(void)
{
    test_lifecycle();
    test_repeated_cycles();
    test_self_test();
    test_transport_states();
    test_concurrent_access();
    test_event_overflow_and_arguments();
}
