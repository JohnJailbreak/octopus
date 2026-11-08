#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#elif !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "oct_service.h"
#include "oct_link.h"
#include "oct_log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define EVENT_CAPACITY 256u
#define SELF_TEST_SESSION 0x4F43u
#define SELF_TEST_MAX_OBJECTS 256u

#if defined(__GNUC__) || defined(__clang__)
#define PRINTF_LIKE(f, a) __attribute__((format(printf, f, a)))
#else
#define PRINTF_LIKE(f, a)
#endif

struct oct_service {
    pthread_mutex_t    lock;
    atomic_int         state;
    bool               stopping;      /* transport stop in progress, lock released */
    oct_store         *store;
    oct_responder     *responder;
    oct_transport      transport;
    bool               has_transport;
    bool               transport_running;
    oct_service_config config;
    char               device_name[OCT_LABEL_MAX + 1];
    char               serial_number[OCT_LABEL_MAX + 1];
    char               system_version[OCT_LABEL_MAX + 1];
    char               last_error[OCT_EVENT_TEXT_MAX];
    bool               has_error;
    oct_event          events[EVENT_CAPACITY];
    size_t             event_head;
    size_t             event_count;
    uint32_t           dropped_events;
    uint32_t           next_sequence;
    uint32_t           next_step;
    uint32_t           starts;
    uint32_t           stops;
    char               log_component[OCT_LOG_NAME_MAX + 1]; /* empty: no debug-log lines */
};

const char *oct_service_state_name(oct_service_state state)
{
    switch (state) {
    case OCT_SERVICE_DISABLED: return "Disabled";
    case OCT_SERVICE_STARTING: return "Starting";
    case OCT_SERVICE_READY: return "Ready";
    case OCT_SERVICE_WAITING: return "Waiting for connection";
    case OCT_SERVICE_CONNECTED: return "Connected";
    case OCT_SERVICE_ERROR: return "Error";
    }
    return "Unknown";
}

const char *oct_event_text(const oct_event *event) { return event ? event->text : ""; }
const char *oct_object_info_name(const oct_object_info *info) { return info ? info->name : ""; }

/* ---- Events ------------------------------------------------------------------- */

static int64_t now_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) return 0;
    return (int64_t)now.tv_sec * 1000 + (int64_t)(now.tv_nsec / 1000000);
}

static void vemit(oct_service *s, oct_event_kind kind, uint32_t step, bool failed, const char *format, va_list args)
    PRINTF_LIKE(5, 0);
static void vemit(oct_service *s, oct_event_kind kind, uint32_t step, bool failed, const char *format, va_list args)
{
    if (s->event_count == EVENT_CAPACITY) {
        s->event_head = (s->event_head + 1) % EVENT_CAPACITY;
        s->event_count--;
        s->dropped_events++;
    }
    oct_event *event = &s->events[(s->event_head + s->event_count) % EVENT_CAPACITY];
    s->event_count++;
    memset(event, 0, sizeof *event);
    event->sequence = ++s->next_sequence;
    event->kind = kind;
    event->state = (oct_service_state)atomic_load(&s->state);
    event->step = step;
    event->failed = failed;
    event->time_ms = now_ms();
    char text[512];
    vsnprintf(text, sizeof text, format, args);
    oct_copy_utf8(event->text, sizeof event->text, text);
    if (!s->log_component[0]) return;
    /* The debug log gets the state changes, the operations and their steps. OCT_EVENT_LOG repeats the steps. */
    oct_log_level level = failed ? OCT_LOG_ERROR : OCT_LOG_INFO;
    switch (kind) {
    case OCT_EVENT_STATE: oct_logf(level, s->log_component, "state", "%s", event->text); break;
    case OCT_EVENT_OPERATION_BEGIN: oct_logf(OCT_LOG_INFO, s->log_component, "begin", "%s", event->text); break;
    case OCT_EVENT_STEP_BEGIN: oct_logf(OCT_LOG_DEBUG, s->log_component, "step_begin", "%s", event->text); break;
    case OCT_EVENT_STEP_END: oct_logf(level, s->log_component, "step", "%s", event->text); break;
    case OCT_EVENT_OPERATION_END: oct_logf(level, s->log_component, "end", "%s", event->text); break;
    case OCT_EVENT_LOG: break;
    }
}

static void emit(oct_service *s, oct_event_kind kind, uint32_t step, bool failed, const char *format, ...)
    PRINTF_LIKE(5, 6);
static void emit(oct_service *s, oct_event_kind kind, uint32_t step, bool failed, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vemit(s, kind, step, failed, format, args);
    va_end(args);
}

static uint32_t step_begin(oct_service *s, const char *text)
{
    uint32_t step = ++s->next_step;
    emit(s, OCT_EVENT_STEP_BEGIN, step, false, "%s", text);
    return step;
}

static void step_end(oct_service *s, uint32_t step, bool failed, const char *format, ...) PRINTF_LIKE(4, 5);
static void step_end(oct_service *s, uint32_t step, bool failed, const char *format, ...)
{
    char text[OCT_EVENT_TEXT_MAX];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof text, format, args);
    va_end(args);
    emit(s, OCT_EVENT_STEP_END, step, failed, "%s", text);
    emit(s, OCT_EVENT_LOG, step, failed, "%s%s", failed ? "! " : "", text);
}

static uint32_t operation_begin(oct_service *s, const char *title)
{
    uint32_t operation = ++s->next_step;
    emit(s, OCT_EVENT_OPERATION_BEGIN, operation, false, "%s", title);
    emit(s, OCT_EVENT_LOG, operation, false, "%s", title);
    return operation;
}

static void operation_end(oct_service *s, uint32_t operation, bool failed, const char *text)
{
    emit(s, OCT_EVENT_OPERATION_END, operation, failed, "%s", text);
    emit(s, OCT_EVENT_LOG, operation, failed, "%s%s", failed ? "! " : "", text);
}

static void record_error(oct_service *s, const char *text)
{
    oct_copy_utf8(s->last_error, sizeof s->last_error, text);
    s->has_error = true;
    if (s->log_component[0]) oct_logf(OCT_LOG_ERROR, s->log_component, "failure", "%s", text);
}

static bool transition_allowed(int from, oct_service_state to)
{
    static const unsigned allowed[] = {
        [OCT_SERVICE_DISABLED] = 1u << OCT_SERVICE_STARTING,
        [OCT_SERVICE_STARTING] = (1u << OCT_SERVICE_READY) | (1u << OCT_SERVICE_WAITING) | (1u << OCT_SERVICE_ERROR),
        [OCT_SERVICE_READY] = (1u << OCT_SERVICE_DISABLED) | (1u << OCT_SERVICE_ERROR),
        [OCT_SERVICE_WAITING] = (1u << OCT_SERVICE_CONNECTED) | (1u << OCT_SERVICE_DISABLED) | (1u << OCT_SERVICE_ERROR),
        [OCT_SERVICE_CONNECTED] = (1u << OCT_SERVICE_WAITING) | (1u << OCT_SERVICE_DISABLED) | (1u << OCT_SERVICE_ERROR),
        [OCT_SERVICE_ERROR] = (1u << OCT_SERVICE_STARTING) | (1u << OCT_SERVICE_DISABLED),
    };
    return from >= 0 && from <= OCT_SERVICE_ERROR && ((allowed[from] >> (unsigned)to) & 1u) != 0;
}

static oct_status set_state(oct_service *s, oct_service_state to)
{
    if (!transition_allowed(atomic_load(&s->state), to)) return OCT_E_STATE;
    atomic_store(&s->state, (int)to);
    emit(s, OCT_EVENT_STATE, 0, to == OCT_SERVICE_ERROR, "%s", oct_service_state_name(to));
    return OCT_OK;
}

static bool active(int state)
{
    return state == OCT_SERVICE_READY || state == OCT_SERVICE_WAITING || state == OCT_SERVICE_CONNECTED;
}

static void format_size(uint64_t bytes, char *out, size_t capacity)
{
    if (bytes < 1000u) {
        snprintf(out, capacity, "%u bytes", (unsigned)bytes);
    } else if (bytes < 1000000u) {
        snprintf(out, capacity, "%.1f KB", (double)bytes / 1e3);
    } else if (bytes < 1000000000u) {
        snprintf(out, capacity, "%.1f MB", (double)bytes / 1e6);
    } else {
        snprintf(out, capacity, "%.1f GB", (double)bytes / 1e9);
    }
}

/* ---- Self-test ------------------------------------------------------------------ */

typedef struct self_test {
    oct_service   *service;
    oct_store     *store;
    oct_responder *responder;
    oct_buf        scratch;
    uint32_t       transaction;
    bool           in_session;
    bool           verbose;
    uint32_t       checks;
    uint32_t       step;
    char           failure[OCT_EVENT_TEXT_MAX];
    uint32_t       handles[SELF_TEST_MAX_OBJECTS];
    uint32_t       handle_count;
} self_test;

static void fail_text(self_test *t, const char *format, ...) PRINTF_LIKE(2, 3);
static void fail_text(self_test *t, const char *format, ...)
{
    char text[512];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof text, format, args);
    va_end(args);
    oct_copy_utf8(t->failure, sizeof t->failure, text);
}

#define EXPECT(t, condition, ...)        \
    do {                                 \
        if (!(condition)) {              \
            fail_text((t), __VA_ARGS__); \
            return false;                \
        }                                \
        (t)->checks++;                   \
    } while (0)

static void phase_begin(self_test *t, const char *text)
{
    if (t->verbose) t->step = step_begin(t->service, text);
}

static void phase_end(self_test *t, const char *format, ...) PRINTF_LIKE(2, 3);
static void phase_end(self_test *t, const char *format, ...)
{
    if (!t->verbose) return;
    char text[OCT_EVENT_TEXT_MAX];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof text, format, args);
    va_end(args);
    step_end(t->service, t->step, false, "%s", text);
}

/* One transaction; numbering follows the session like a real initiator. */
static bool call(self_test *t, uint16_t operation, const uint32_t *params, uint32_t count, bool send,
                 const void *data, size_t length, oct_exchange *result)
{
    uint32_t transaction = 0;
    if (t->in_session) {
        transaction = t->transaction;
        t->transaction = t->transaction >= 0xFFFFFFFEu ? 1u : t->transaction + 1u;
    }
    oct_status status = oct_exchange_run(t->responder, operation, transaction, params, count, send, data,
                                         length, &t->scratch, result);
    if (status != OCT_OK) {
        fail_text(t, "operation 0x%04X: %s", operation, oct_status_name(status));
        return false;
    }
    t->checks++;
    return true;
}

static bool expect_code(self_test *t, uint16_t operation, const uint32_t *params, uint32_t count, uint16_t code)
{
    oct_exchange x;
    if (!call(t, operation, params, count, false, NULL, 0, &x)) return false;
    EXPECT(t, x.response_code == code, "operation 0x%04X answered 0x%04X, expected 0x%04X", operation,
           x.response_code, code);
    return true;
}

static bool phase_device_info(self_test *t)
{
    phase_begin(t, "Reading device information");
    oct_exchange x;
    if (!call(t, OCT_OP_GET_DEVICE_INFO, NULL, 0, false, NULL, 0, &x)) return false;
    EXPECT(t, x.response_code == OCT_RC_OK && x.has_data, "GetDeviceInfo answered 0x%04X", x.response_code);
    oct_reader reader;
    char text[256];
    oct_reader_init(&reader, x.data, x.data_length);
    uint16_t standard = oct_read_u16(&reader);
    uint32_t vendor = oct_read_u32(&reader);
    (void)oct_read_u16(&reader);
    bool described = oct_read_string(&reader, text, sizeof text);
    EXPECT(t, standard == 100 && vendor == 6 && described && strstr(text, "microsoft.com: 1.0"),
           "DeviceInfo does not identify an MTP device");
    (void)oct_read_u16(&reader);
    uint32_t advertised = oct_read_u32(&reader);
    size_t implemented = 0;
    const uint16_t *operations = oct_responder_operations(&implemented);
    EXPECT(t, !reader.failed && advertised == implemented, "DeviceInfo lists %u operations, %zu implemented",
           advertised, implemented);
    for (uint32_t i = 0; i < advertised; i++) {
        uint16_t code = oct_read_u16(&reader);
        EXPECT(t, !reader.failed && code == operations[i], "DeviceInfo operation list differs at %u", i);
    }
    phase_end(t, "Device information: MTP, %u operations", advertised);
    return true;
}

static bool phase_session(self_test *t)
{
    phase_begin(t, "Checking session rules");
    if (!expect_code(t, OCT_OP_GET_STORAGE_IDS, NULL, 0, OCT_RC_SESSION_NOT_OPEN)) return false;
    uint32_t zero = 0, id = SELF_TEST_SESSION;
    if (!expect_code(t, OCT_OP_OPEN_SESSION, &zero, 1, OCT_RC_INVALID_PARAMETER)) return false;
    if (!expect_code(t, OCT_OP_OPEN_SESSION, &id, 1, OCT_RC_OK)) return false;
    t->in_session = true;
    t->transaction = 1;
    oct_exchange x;
    if (!call(t, OCT_OP_OPEN_SESSION, &id, 1, false, NULL, 0, &x)) return false;
    EXPECT(t, x.response_code == OCT_RC_SESSION_ALREADY_OPEN && x.param_count == 1 && x.params[0] == id,
           "second OpenSession answered 0x%04X", x.response_code);
    phase_end(t, "Session rules enforced");
    return true;
}

static bool phase_enumerate(self_test *t)
{
    phase_begin(t, "Enumerating storage");
    uint32_t storage = oct_store_storage_id(t->store);
    oct_exchange x;
    if (!call(t, OCT_OP_GET_STORAGE_IDS, NULL, 0, false, NULL, 0, &x)) return false;
    oct_reader reader;
    oct_reader_init(&reader, x.data, x.data_length);
    uint32_t count = oct_read_u32(&reader), first = oct_read_u32(&reader);
    EXPECT(t, x.response_code == OCT_RC_OK && !reader.failed && count == 1 && first == storage,
           "GetStorageIDs did not report storage 0x%08X", storage);
    if (!call(t, OCT_OP_GET_STORAGE_INFO, &storage, 1, false, NULL, 0, &x)) return false;
    char description[256];
    oct_reader_init(&reader, x.data, x.data_length);
    for (int i = 0; i < 3; i++) (void)oct_read_u16(&reader);
    (void)oct_read_u64(&reader);
    (void)oct_read_u64(&reader);
    (void)oct_read_u32(&reader);
    EXPECT(t, x.response_code == OCT_RC_OK && oct_read_string(&reader, description, sizeof description) &&
                  description[0] != '\0', "GetStorageInfo returned no description");
    uint32_t wrong = 0xDEAD0001u;
    if (!expect_code(t, OCT_OP_GET_STORAGE_INFO, &wrong, 1, OCT_RC_INVALID_STORAGE_ID)) return false;
    uint32_t all[3] = { 0xFFFFFFFFu, 0, 0 };
    if (!call(t, OCT_OP_GET_NUM_OBJECTS, all, 3, false, NULL, 0, &x)) return false;
    uint32_t expected = oct_store_object_count(t->store);
    EXPECT(t, x.response_code == OCT_RC_OK && x.param_count == 1 && x.params[0] == expected,
           "GetNumObjects reported %u of %u objects", x.params[0], expected);
    if (!call(t, OCT_OP_GET_OBJECT_HANDLES, all, 3, false, NULL, 0, &x)) return false;
    oct_reader_init(&reader, x.data, x.data_length);
    count = oct_read_u32(&reader);
    EXPECT(t, x.response_code == OCT_RC_OK && count == expected, "GetObjectHandles listed %u of %u objects", count,
           expected);
    t->handle_count = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t handle = oct_read_u32(&reader);
        if (t->handle_count < SELF_TEST_MAX_OBJECTS) t->handles[t->handle_count++] = handle;
    }
    EXPECT(t, !reader.failed, "GetObjectHandles data is truncated");
    uint32_t root[3] = { storage, 0, 0xFFFFFFFFu };
    if (!call(t, OCT_OP_GET_OBJECT_HANDLES, root, 3, false, NULL, 0, &x)) return false;
    oct_reader_init(&reader, x.data, x.data_length);
    uint32_t root_count = oct_read_u32(&reader);
    EXPECT(t, x.response_code == OCT_RC_OK && root_count <= count, "root listing is inconsistent");
    phase_end(t, "%s: %u objects, %u at the root", description, count, root_count);
    return true;
}

static bool known_handle(const self_test *t, uint32_t handle)
{
    for (uint32_t i = 0; i < t->handle_count; i++) {
        if (t->handles[i] == handle) return true;
    }
    return false;
}

static bool phase_object_info(self_test *t)
{
    phase_begin(t, "Reading object information");
    for (uint32_t i = 0; i < t->handle_count; i++) {
        oct_exchange x;
        oct_object_info expected;
        oct_object_info_request decoded;
        if (!call(t, OCT_OP_GET_OBJECT_INFO, &t->handles[i], 1, false, NULL, 0, &x)) return false;
        EXPECT(t, x.response_code == OCT_RC_OK, "GetObjectInfo(0x%08X) answered 0x%04X", t->handles[i], x.response_code);
        EXPECT(t, oct_dataset_parse_object_info(x.data, x.data_length, &decoded) == OCT_OK &&
                      oct_store_get(t->store, t->handles[i], &expected) == OCT_OK,
               "ObjectInfo for 0x%08X does not decode", t->handles[i]);
        EXPECT(t, decoded.storage_id == expected.storage_id && decoded.format == expected.format &&
                      strcmp(decoded.name, expected.name) == 0 &&
                      (decoded.parent == 0 || known_handle(t, decoded.parent)) &&
                      decoded.compressed_size == (expected.size >= 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)expected.size),
               "ObjectInfo for %s disagrees with the store", expected.name);
    }
    phase_end(t, "Object information: %u datasets verified", t->handle_count);
    return true;
}

static bool phase_read(self_test *t)
{
    phase_begin(t, "Reading files");
    uint32_t files = 0;
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < t->handle_count; i++) {
        oct_object_info info;
        if (oct_store_get(t->store, t->handles[i], &info) != OCT_OK || info.is_directory || info.size > 65536u) continue;
        oct_exchange x;
        if (!call(t, OCT_OP_GET_OBJECT, &info.handle, 1, false, NULL, 0, &x)) return false;
        EXPECT(t, x.response_code == OCT_RC_OK && x.data_length == info.size, "GetObject(%s) returned %zu of %llu bytes",
               info.name, x.data_length, (unsigned long long)info.size);
        uint8_t *copy = malloc(info.size ? (size_t)info.size : 1u);
        size_t read = 0;
        bool same = copy && oct_store_read(t->store, info.handle, 0, copy, (size_t)info.size, &read) == OCT_OK &&
                    read == info.size && (info.size == 0 || memcmp(copy, x.data, read) == 0);
        if (same && info.size > 16) {
            uint32_t params[3] = { info.handle, (uint32_t)(info.size / 3), 64 };
            uint32_t want = (uint32_t)(info.size - params[1] < 64 ? info.size - params[1] : 64);
            same = call(t, OCT_OP_GET_PARTIAL_OBJECT, params, 3, false, NULL, 0, &x) &&
                   x.response_code == OCT_RC_OK && x.param_count == 1 && x.params[0] == want &&
                   x.data_length == want && memcmp(copy + params[1], x.data, want) == 0;
        }
        free(copy);
        EXPECT(t, same, "contents of %s differ between protocol and store", info.name);
        files++;
        bytes += info.size;
    }
    char size[32];
    format_size(bytes, size, sizeof size);
    phase_end(t, "Read %u files, %s, full and partial", files, size);
    return true;
}

static bool phase_properties(self_test *t)
{
    phase_begin(t, "Querying object properties");
    size_t property_count = 0;
    oct_object_properties(&property_count);
    uint32_t format = OCT_FORMAT_UNDEFINED;
    oct_exchange x;
    if (!call(t, OCT_OP_GET_OBJECT_PROPS_SUPPORTED, &format, 1, false, NULL, 0, &x)) return false;
    oct_reader reader;
    oct_reader_init(&reader, x.data, x.data_length);
    EXPECT(t, x.response_code == OCT_RC_OK && oct_read_u32(&reader) == property_count, "property list is incomplete");
    oct_handle_list root = { 0 };
    if (oct_store_list(t->store, OCT_ROOT, false, &root) != OCT_OK) {
        fail_text(t, "store listing failed");
        return false;
    }
    uint32_t root_count = root.count, first = root.count ? root.handles[0] : 0;
    oct_handle_list_free(&root);
    uint32_t list[5] = { 0, 0, 0xFFFFFFFFu, 0, 1 };
    if (!call(t, OCT_OP_GET_OBJECT_PROP_LIST, list, 5, false, NULL, 0, &x)) return false;
    oct_reader_init(&reader, x.data, x.data_length);
    uint32_t elements = oct_read_u32(&reader);
    EXPECT(t, x.response_code == OCT_RC_OK && elements == root_count * property_count,
           "GetObjectPropList returned %u values, expected %u", elements, root_count * (uint32_t)property_count);
    if (first) {
        oct_object_info info;
        char name[512];
        uint32_t params[2] = { first, OCT_PROP_OBJECT_FILE_NAME };
        if (!call(t, OCT_OP_GET_OBJECT_PROP_VALUE, params, 2, false, NULL, 0, &x)) return false;
        oct_reader_init(&reader, x.data, x.data_length);
        EXPECT(t, x.response_code == OCT_RC_OK && oct_read_string(&reader, name, sizeof name) &&
                      oct_store_get(t->store, first, &info) == OCT_OK && strcmp(name, info.name) == 0,
               "ObjectFileName property is wrong");
    }
    phase_end(t, "Property list: %u values", elements);
    return true;
}

static bool phase_invalid(self_test *t)
{
    phase_begin(t, "Rejecting invalid requests");
    uint32_t bogus = 0x7FFFFFF0u;
    if (!expect_code(t, OCT_OP_GET_OBJECT_INFO, &bogus, 1, OCT_RC_INVALID_OBJECT_HANDLE)) return false;
    if (!expect_code(t, OCT_OP_MOVE_OBJECT, NULL, 0, OCT_RC_OPERATION_NOT_SUPPORTED)) return false;
    for (uint32_t i = 0; i < t->handle_count; i++) {
        oct_object_info info;
        if (oct_store_get(t->store, t->handles[i], &info) == OCT_OK && info.is_directory) {
            if (!expect_code(t, OCT_OP_GET_OBJECT, &info.handle, 1, OCT_RC_INVALID_OBJECT_HANDLE)) return false;
            break;
        }
    }
    oct_exchange x; /* out-of-sequence transaction: refused, numbering unchanged */
    oct_status status = oct_exchange_run(t->responder, OCT_OP_GET_STORAGE_IDS, t->transaction + 5u, NULL, 0, false,
                                         NULL, 0, &t->scratch, &x);
    EXPECT(t, status == OCT_OK && x.response_code == OCT_RC_INVALID_TRANSACTION_ID, "out-of-sequence request accepted");
    static const uint8_t truncated[8] = { 8, 0, 0, 0, 1, 0, 0x01, 0x10 };
    oct_buf_clear(&t->scratch);
    status = oct_responder_handle(t->responder, truncated, sizeof truncated, &t->scratch);
    EXPECT(t, status == OCT_E_MALFORMED && t->scratch.length == 0, "truncated container was not rejected");
    uint8_t oversized[36] = { 36, 0, 0, 0, 1, 0, 0x08, 0x10 };
    oversized[8] = (uint8_t)(t->transaction & 0xFFu);
    oct_buf_clear(&t->scratch);
    status = oct_responder_handle(t->responder, oversized, sizeof oversized, &t->scratch);
    EXPECT(t, status == OCT_E_MALFORMED && t->scratch.length == 12 && t->scratch.data[6] == 0x02 &&
                  t->scratch.data[7] == 0x20, "oversized command was not answered with GeneralError");
    phase_end(t, "Invalid requests rejected: 6 of 6");
    return true;
}

static bool send_object(self_test *t, uint32_t parent, const char *name, bool directory, const void *data,
                        size_t length, uint32_t *handle)
{
    oct_object_info info;
    memset(&info, 0, sizeof info);
    info.storage_id = oct_store_storage_id(t->store);
    info.format = (uint16_t)(directory ? OCT_FORMAT_ASSOCIATION : oct_format_for_name(name, false));
    info.is_directory = directory;
    info.size = length;
    info.created = info.modified = (int64_t)time(NULL);
    oct_copy_utf8(info.name, sizeof info.name, name);
    oct_buf dataset;
    oct_buf_init(&dataset, 0);
    oct_dataset_object_info(&dataset, &info);
    uint32_t params[2] = { info.storage_id, parent };
    oct_exchange x;
    bool ok = !dataset.failed && call(t, OCT_OP_SEND_OBJECT_INFO, params, 2, true, dataset.data, dataset.length, &x);
    oct_buf_free(&dataset);
    if (!ok) return false;
    EXPECT(t, x.response_code == OCT_RC_OK && x.param_count == 3, "SendObjectInfo(%s) answered 0x%04X", name,
           x.response_code);
    *handle = x.params[2];
    if (directory) return true;
    if (!call(t, OCT_OP_SEND_OBJECT, NULL, 0, true, data, length, &x)) return false;
    EXPECT(t, x.response_code == OCT_RC_OK, "SendObject(%s) answered 0x%04X", name, x.response_code);
    return true;
}

static bool phase_writes(self_test *t)
{
    oct_storage_info storage;
    uint32_t scratch = 0, protected_file = 0;
    for (uint32_t i = 0; i < t->handle_count && !protected_file; i++) {
        oct_object_info info;
        if (oct_store_get(t->store, t->handles[i], &info) == OCT_OK && !info.is_directory && info.write_protected) {
            protected_file = info.handle;
        }
    }
    if (oct_store_storage_info(t->store, &storage) != OCT_OK) return false;
    if (storage.access_capability != 0x0000u) {
        phase_begin(t, "Checking read-only policy");
        uint32_t params[2] = { storage.storage_id, 0xFFFFFFFFu };
        oct_buf dataset;
        oct_object_info info;
        memset(&info, 0, sizeof info);
        info.storage_id = storage.storage_id;
        info.format = OCT_FORMAT_TEXT;
        oct_copy_utf8(info.name, sizeof info.name, "refused.txt");
        oct_buf_init(&dataset, 0);
        oct_dataset_object_info(&dataset, &info);
        oct_exchange x;
        bool ok = call(t, OCT_OP_SEND_OBJECT_INFO, params, 2, true, dataset.data, dataset.length, &x);
        oct_buf_free(&dataset);
        if (!ok) return false;
        EXPECT(t, x.response_code == OCT_RC_STORE_READ_ONLY, "read-only storage accepted SendObjectInfo");
        if (protected_file) {
            uint32_t remove[2] = { protected_file, 0 };
            if (!expect_code(t, OCT_OP_DELETE_OBJECT, remove, 2, OCT_RC_STORE_READ_ONLY)) return false;
        }
        phase_end(t, "Read-only storage refused every write");
        return true;
    }
    if (oct_store_find(t->store, OCT_ROOT, "Scratch", &scratch) != OCT_OK) {
        phase_begin(t, "Checking write policy");
        phase_end(t, "No Scratch folder; write checks skipped");
        return true;
    }
    phase_begin(t, "Writing in Scratch");
    static const char body[] = "Octopus self-test payload\n";
    uint32_t file = 0, folder = 0;
    if (!send_object(t, scratch, "octopus-self-test.txt", false, body, sizeof body - 1, &file)) return false;
    oct_exchange x;
    if (!call(t, OCT_OP_GET_OBJECT, &file, 1, false, NULL, 0, &x)) return false;
    EXPECT(t, x.response_code == OCT_RC_OK && x.data_length == sizeof body - 1 && memcmp(x.data, body, sizeof body - 1) == 0,
           "uploaded file reads back differently");
    oct_buf value;
    oct_buf_init(&value, 0);
    oct_buf_string(&value, "octopus-self-test-renamed.txt");
    uint32_t rename[2] = { file, OCT_PROP_OBJECT_FILE_NAME };
    bool ok = call(t, OCT_OP_SET_OBJECT_PROP_VALUE, rename, 2, true, value.data, value.length, &x);
    oct_buf_free(&value);
    if (!ok) return false;
    oct_object_info info;
    EXPECT(t, x.response_code == OCT_RC_OK && oct_store_get(t->store, file, &info) == OCT_OK &&
                  strcmp(info.name, "octopus-self-test-renamed.txt") == 0, "rename through SetObjectPropValue failed");
    if (!send_object(t, scratch, "octopus-self-test-folder", true, NULL, 0, &folder)) return false;
    uint32_t remove_folder[2] = { folder, 0 }, remove_file[2] = { file, 0 };
    if (!expect_code(t, OCT_OP_DELETE_OBJECT, remove_folder, 2, OCT_RC_OK)) return false;
    if (!expect_code(t, OCT_OP_DELETE_OBJECT, remove_file, 2, OCT_RC_OK)) return false;
    if (!expect_code(t, OCT_OP_GET_OBJECT_INFO, &file, 1, OCT_RC_INVALID_OBJECT_HANDLE)) return false;
    if (protected_file) {
        uint32_t remove[2] = { protected_file, 0 };
        if (!expect_code(t, OCT_OP_DELETE_OBJECT, remove, 2, OCT_RC_OBJECT_WRITE_PROTECTED)) return false;
    }
    phase_end(t, "Scratch: created, read, renamed and deleted");
    return true;
}

static bool phase_close(self_test *t)
{
    phase_begin(t, "Closing the session");
    if (!expect_code(t, OCT_OP_CLOSE_SESSION, NULL, 0, OCT_RC_OK)) return false;
    t->in_session = false;
    if (!expect_code(t, OCT_OP_GET_STORAGE_IDS, NULL, 0, OCT_RC_SESSION_NOT_OPEN)) return false;
    phase_end(t, "Session closed");
    return true;
}

/* The transport adapter, end to end: a temporary service on a loopback link (oct_link_self_test). */
static bool phase_transport(self_test *t)
{
    phase_begin(t, "Checking the transport adapter");
    uint32_t checks = 0;
    char failure[OCT_EVENT_TEXT_MAX] = "";
    oct_status status = oct_link_self_test(&checks, failure, sizeof failure);
    t->checks += checks;
    if (status != OCT_OK) {
        fail_text(t, "%s", failure[0] ? failure : oct_status_name(status));
        return false;
    }
    phase_end(t, "Transport adapter: %u checks, fragmented input", (unsigned)checks);
    return true;
}

static oct_status run_self_test(oct_service *s, oct_store *store, bool verbose, uint32_t *checks, char *failure,
                                size_t capacity)
{
    self_test *t = calloc(1, sizeof *t);
    if (!t) return OCT_E_NO_MEMORY;
    t->service = s;
    t->store = store;
    t->verbose = verbose;
    oct_buf_init(&t->scratch, 0);
    oct_responder_identity identity = { "Octopus", "Octopus Self-Test", OCT_CORE_VERSION, "self-test", NULL };
    oct_status status = oct_responder_create(store, &identity, &t->responder);
    bool passed = status == OCT_OK && phase_device_info(t) && phase_session(t) && phase_enumerate(t) &&
                  phase_object_info(t) && phase_read(t) && phase_properties(t) && phase_invalid(t) &&
                  phase_writes(t) && phase_close(t) && phase_transport(t);
    if (!passed && verbose && t->step) step_end(s, t->step, true, "%s", t->failure);
    if (status == OCT_OK && !passed) status = OCT_E_IO;
    *checks = t->checks;
    oct_copy_utf8(failure, capacity, status == OCT_OK ? "" : t->failure[0] ? t->failure : oct_status_name(status));
    oct_responder_destroy(t->responder);
    oct_buf_free(&t->scratch);
    free(t);
    return status;
}

/* ---- Lifecycle ------------------------------------------------------------------- */

static void release_locked(oct_service *s)
{
    if (s->transport_running) {
        oct_transport transport = s->transport;
        s->transport_running = false;
        s->stopping = true;
        pthread_mutex_unlock(&s->lock); /* the transport may be joining threads that call in */
        transport.ops->stop(transport.context);
        pthread_mutex_lock(&s->lock);
        s->stopping = false;
    }
    if (s->responder) {
        oct_responder_destroy(s->responder);
        s->responder = NULL;
    }
    if (s->store) {
        oct_store_close(s->store);
        s->store = NULL;
    }
}

static oct_status fail_step(oct_service *s, uint32_t step, oct_status status, const char *text)
{
    step_end(s, step, true, "%s", text);
    record_error(s, text);
    return status;
}

static oct_status start_locked(oct_service *s, uint32_t operation)
{
    char text[512], size[32];
    uint32_t step = step_begin(s, "Preparing Octopus storage");
    oct_synthetic_options synthetic = { s->config.seed_time, s->config.scratch };
    oct_backend backend;
    oct_status status = oct_synthetic_backend_create(&synthetic, &backend);
    if (status == OCT_OK) {
        oct_store_options options = { 0, 0, "Octopus", "Octopus" };
        status = oct_store_open(&options, backend, &s->store);
    }
    oct_handle_list list = { 0 };
    if (status == OCT_OK) status = oct_store_list(s->store, OCT_ROOT, true, &list);
    uint64_t bytes = 0;
    for (uint32_t i = 0; status == OCT_OK && i < list.count; i++) {
        oct_object_info info;
        if (oct_store_get(s->store, list.handles[i], &info) == OCT_OK) bytes += info.size;
    }
    uint32_t objects = list.count;
    oct_handle_list_free(&list);
    if (status != OCT_OK) {
        snprintf(text, sizeof text, "Storage failed: %s", oct_status_name(status));
        return fail_step(s, step, status, text);
    }
    format_size(bytes, size, sizeof size);
    step_end(s, step, false, "Octopus storage: %u objects, %s", objects, size);

    step = step_begin(s, "Starting protocol responder");
    char version[160];
    snprintf(version, sizeof version, "%s (iOS %s)", OCT_CORE_VERSION, s->system_version);
    oct_responder_identity identity = { "Octopus", "Octopus File Provider", version, s->serial_number,
                                        s->device_name };
    status = oct_responder_create(s->store, &identity, &s->responder);
    if (status != OCT_OK) {
        snprintf(text, sizeof text, "Responder failed: %s", oct_status_name(status));
        return fail_step(s, step, status, text);
    }
    size_t operations = 0;
    oct_responder_operations(&operations);
    step_end(s, step, false, "Responder ready: %zu operations", operations);

    if (s->config.self_test) {
        step = step_begin(s, "Running protocol self-test");
        uint32_t checks = 0;
        char failure[OCT_EVENT_TEXT_MAX];
        status = run_self_test(s, s->store, false, &checks, failure, sizeof failure);
        if (status != OCT_OK) {
            snprintf(text, sizeof text, "Self-test failed: %s", failure);
            return fail_step(s, step, status, text);
        }
        step_end(s, step, false, "Self-test passed: %u checks", checks);
    }

    step = step_begin(s, "Checking USB transport");
    if (!s->has_transport) {
        step_end(s, step, true, "No USB transport in this build");
        set_state(s, OCT_SERVICE_READY);
        operation_end(s, operation, false, "Ready \xC2\xB7 local service only");
        return OCT_OK;
    }
    status = s->transport.ops->start(s->transport.context, s);
    if (status != OCT_OK) {
        snprintf(text, sizeof text, "%s transport failed: %s", s->transport.ops->name, oct_status_name(status));
        return fail_step(s, step, status, text);
    }
    s->transport_running = true;
    step_end(s, step, false, "%s transport listening", s->transport.ops->name);
    set_state(s, OCT_SERVICE_WAITING);
    operation_end(s, operation, false, "Waiting for a host");
    return OCT_OK;
}

oct_status oct_service_create(oct_service **out)
{
    if (!out) return OCT_E_INVALID_ARGUMENT;
    *out = NULL;
    oct_service *s = calloc(1, sizeof *s);
    if (!s) return OCT_E_NO_MEMORY;
    if (pthread_mutex_init(&s->lock, NULL) != 0) {
        free(s);
        return OCT_E_NO_MEMORY;
    }
    atomic_init(&s->state, OCT_SERVICE_DISABLED);
    oct_copy_utf8(s->device_name, sizeof s->device_name, "iPhone");
    oct_copy_utf8(s->serial_number, sizeof s->serial_number, "0");
    oct_copy_utf8(s->system_version, sizeof s->system_version, "unknown");
    oct_copy_utf8(s->log_component, sizeof s->log_component, "service");
    *out = s;
    return OCT_OK;
}

void oct_service_destroy(oct_service *s)
{
    if (!s) return;
    oct_service_stop(s);
    pthread_mutex_lock(&s->lock);
    release_locked(s);
    pthread_mutex_unlock(&s->lock);
    pthread_mutex_destroy(&s->lock);
    free(s);
}

oct_status oct_service_set_identity(oct_service *s, const char *device_name, const char *serial_number,
                                    const char *system_version)
{
    if (!s) return OCT_E_INVALID_ARGUMENT;
    pthread_mutex_lock(&s->lock);
    if (device_name && device_name[0]) oct_copy_utf8(s->device_name, sizeof s->device_name, device_name);
    if (serial_number && serial_number[0]) oct_copy_utf8(s->serial_number, sizeof s->serial_number, serial_number);
    if (system_version && system_version[0]) oct_copy_utf8(s->system_version, sizeof s->system_version, system_version);
    pthread_mutex_unlock(&s->lock);
    return OCT_OK;
}

oct_status oct_service_set_transport(oct_service *s, const oct_transport *transport)
{
    if (!s || (transport && (!transport->ops || !transport->ops->start || !transport->ops->stop))) {
        return OCT_E_INVALID_ARGUMENT;
    }
    pthread_mutex_lock(&s->lock);
    int state = atomic_load(&s->state);
    oct_status status = OCT_E_STATE;
    if (state == OCT_SERVICE_DISABLED || state == OCT_SERVICE_ERROR) {
        s->has_transport = transport != NULL;
        if (transport) s->transport = *transport;
        status = OCT_OK;
    }
    pthread_mutex_unlock(&s->lock);
    return status;
}

oct_status oct_service_start(oct_service *s, const oct_service_config *config)
{
    if (!s) return OCT_E_INVALID_ARGUMENT;
    pthread_mutex_lock(&s->lock);
    int state = atomic_load(&s->state);
    if (active(state)) {
        emit(s, OCT_EVENT_LOG, 0, false, "Service is already running");
        pthread_mutex_unlock(&s->lock);
        return OCT_OK;
    }
    if (state == OCT_SERVICE_STARTING || s->stopping) {
        pthread_mutex_unlock(&s->lock);
        return OCT_E_STATE;
    }
    release_locked(s);
    oct_service_config defaults = { 0, false, true };
    s->config = config ? *config : defaults;
    s->has_error = false;
    s->last_error[0] = '\0';
    s->starts++;
    uint32_t operation = operation_begin(s, "Enabling");
    set_state(s, OCT_SERVICE_STARTING);
    oct_status status = start_locked(s, operation);
    if (status != OCT_OK) {
        release_locked(s);
        set_state(s, OCT_SERVICE_ERROR);
        operation_end(s, operation, true, "Couldn't enable USB access");
    }
    pthread_mutex_unlock(&s->lock);
    return status;
}

oct_status oct_service_stop(oct_service *s)
{
    if (!s) return OCT_E_INVALID_ARGUMENT;
    pthread_mutex_lock(&s->lock);
    int state = atomic_load(&s->state);
    if (s->stopping || state == OCT_SERVICE_STARTING) {
        pthread_mutex_unlock(&s->lock);
        return OCT_E_STATE;
    }
    if (state == OCT_SERVICE_DISABLED) {
        pthread_mutex_unlock(&s->lock);
        return OCT_OK;
    }
    s->stops++;
    uint32_t operation = operation_begin(s, "Disabling");
    if (state == OCT_SERVICE_ERROR) {
        uint32_t step = step_begin(s, "Clearing the error");
        release_locked(s);
        s->has_error = false;
        s->last_error[0] = '\0';
        step_end(s, step, false, "Service reset");
    } else {
        uint32_t step = step_begin(s, "Closing host session");
        uint32_t session = 0;
        bool open = s->responder && oct_responder_session(s->responder, &session);
        if (open) oct_responder_reset(s->responder);
        if (open) {
            step_end(s, step, false, "Host session %u closed", session);
        } else {
            step_end(s, step, false, "No host session was open");
        }
        if (s->transport_running) {
            step = step_begin(s, "Stopping USB transport");
            oct_responder *responder = s->responder;
            s->responder = NULL; /* nothing may reach the responder while unlocked */
            oct_store *store = s->store;
            s->store = NULL;
            release_locked(s);
            s->responder = responder;
            s->store = store;
            step_end(s, step, false, "Transport stopped");
        }
        step = step_begin(s, "Stopping protocol responder");
        oct_responder_stats stats = { 0 };
        oct_responder_get_stats(s->responder, &stats);
        oct_responder_destroy(s->responder);
        s->responder = NULL;
        step_end(s, step, false, "Responder stopped after %llu requests", (unsigned long long)stats.commands);
        step = step_begin(s, "Releasing Octopus storage");
        oct_store_close(s->store);
        s->store = NULL;
        step_end(s, step, false, "Storage released");
    }
    set_state(s, OCT_SERVICE_DISABLED);
    operation_end(s, operation, false, "USB access disabled");
    pthread_mutex_unlock(&s->lock);
    return OCT_OK;
}

oct_status oct_service_self_test(oct_service *s)
{
    if (!s) return OCT_E_INVALID_ARGUMENT;
    pthread_mutex_lock(&s->lock);
    int state = atomic_load(&s->state);
    if (state == OCT_SERVICE_STARTING || state == OCT_SERVICE_CONNECTED || s->stopping) {
        pthread_mutex_unlock(&s->lock);
        return OCT_E_STATE;
    }
    uint32_t operation = operation_begin(s, "Testing");
    oct_store *store = s->store, *temporary = NULL;
    oct_status status = OCT_OK;
    if (!store) {
        uint32_t step = step_begin(s, "Preparing temporary storage");
        oct_synthetic_options synthetic = { 0, true };
        oct_backend backend;
        status = oct_synthetic_backend_create(&synthetic, &backend);
        if (status == OCT_OK) status = oct_store_open(NULL, backend, &temporary);
        if (status == OCT_OK) status = oct_store_index_all(temporary);
        step_end(s, step, status != OCT_OK, status == OCT_OK ? "Temporary storage with Scratch" : "Temporary storage failed");
        store = temporary;
    }
    uint32_t checks = 0;
    char failure[OCT_EVENT_TEXT_MAX] = "";
    if (status == OCT_OK) status = run_self_test(s, store, true, &checks, failure, sizeof failure);
    oct_store_close(temporary);
    char summary[512];
    if (status == OCT_OK) {
        snprintf(summary, sizeof summary, "Self-test passed: %u checks", checks);
    } else {
        snprintf(summary, sizeof summary, "Self-test failed: %s", failure[0] ? failure : oct_status_name(status));
    }
    operation_end(s, operation, status != OCT_OK, summary);
    pthread_mutex_unlock(&s->lock);
    return status;
}

oct_service_state oct_service_get_state(const oct_service *s)
{
    return s ? (oct_service_state)atomic_load(&((oct_service *)s)->state) : OCT_SERVICE_DISABLED;
}

oct_status oct_service_get_status(oct_service *s, oct_service_status *out)
{
    if (!s || !out) return OCT_E_INVALID_ARGUMENT;
    memset(out, 0, sizeof *out);
    pthread_mutex_lock(&s->lock);
    out->state = (oct_service_state)atomic_load(&s->state);
    out->transport_available = s->has_transport;
    if (s->has_transport && s->transport.ops->name) {
        oct_copy_utf8(out->transport_name, sizeof out->transport_name, s->transport.ops->name);
    }
    out->session_open = s->responder && oct_responder_session(s->responder, &out->session_id);
    out->storage_id = s->store ? oct_store_storage_id(s->store) : 0;
    out->object_count = s->store ? oct_store_object_count(s->store) : 0;
    size_t operations = 0;
    oct_responder_operations(&operations);
    out->operation_count = (uint32_t)operations;
    oct_responder_stats stats = { 0 };
    if (s->responder) oct_responder_get_stats(s->responder, &stats);
    out->transactions = stats.commands;
    out->starts = s->starts;
    out->stops = s->stops;
    out->dropped_events = s->dropped_events;
    out->scratch = s->store && s->config.scratch;
    out->transport_state = OCT_TRANSPORT_UNAVAILABLE;
    if (s->has_transport && s->transport.ops->status) {
        oct_transport_status transport = { 0 };
        s->transport.ops->status(s->transport.context, &transport);
        out->transport_state = transport.state;
        out->transport_transactions = transport.transactions;
        oct_copy_utf8(out->transport_failure, sizeof out->transport_failure, transport.failure);
    } else if (s->has_transport) {
        int state = atomic_load(&s->state);
        out->transport_state = state == OCT_SERVICE_STARTING    ? OCT_TRANSPORT_INITIALIZING
                               : state == OCT_SERVICE_WAITING   ? OCT_TRANSPORT_WAITING
                               : state == OCT_SERVICE_CONNECTED ? OCT_TRANSPORT_HOST_CONNECTED
                                                                : OCT_TRANSPORT_UNAVAILABLE;
    }
    pthread_mutex_unlock(&s->lock);
    return OCT_OK;
}

bool oct_service_last_error(oct_service *s, char *buffer, size_t capacity)
{
    if (!s) return false;
    pthread_mutex_lock(&s->lock);
    bool has = s->has_error;
    if (buffer && capacity) oct_copy_utf8(buffer, capacity, has ? s->last_error : "");
    pthread_mutex_unlock(&s->lock);
    return has;
}

size_t oct_service_drain_events(oct_service *s, oct_event *events, size_t capacity)
{
    if (!s || !events) return 0;
    pthread_mutex_lock(&s->lock);
    size_t count = s->event_count < capacity ? s->event_count : capacity;
    for (size_t i = 0; i < count; i++) events[i] = s->events[(s->event_head + i) % EVENT_CAPACITY];
    s->event_head = (s->event_head + count) % EVENT_CAPACITY;
    s->event_count -= count;
    pthread_mutex_unlock(&s->lock);
    return count;
}

oct_status oct_service_storage_info(oct_service *s, oct_storage_info *out)
{
    if (!s || !out) return OCT_E_INVALID_ARGUMENT;
    pthread_mutex_lock(&s->lock);
    oct_status status = s->store && !s->stopping ? oct_store_storage_info(s->store, out) : OCT_E_STATE;
    pthread_mutex_unlock(&s->lock);
    return status;
}

oct_status oct_service_list(oct_service *s, uint32_t parent, oct_object_info *items, size_t capacity, size_t *count)
{
    if (!s || !count || (capacity && !items)) return OCT_E_INVALID_ARGUMENT;
    *count = 0;
    pthread_mutex_lock(&s->lock);
    oct_status status = OCT_E_STATE;
    if (s->store && !s->stopping) {
        oct_handle_list list = { 0 };
        status = oct_store_list(s->store, parent, false, &list);
        for (uint32_t i = 0; status == OCT_OK && i < list.count && i < capacity; i++) {
            status = oct_store_get(s->store, list.handles[i], &items[i]);
        }
        if (status == OCT_OK) *count = list.count;
        oct_handle_list_free(&list);
    }
    pthread_mutex_unlock(&s->lock);
    return status;
}

oct_status oct_service_read(oct_service *s, uint32_t handle, uint64_t offset, void *buffer, size_t length,
                            size_t *read)
{
    if (read) *read = 0;
    if (!s || !read) return OCT_E_INVALID_ARGUMENT;
    pthread_mutex_lock(&s->lock);
    oct_status status = s->store && !s->stopping ? oct_store_read(s->store, handle, offset, buffer, length, read)
                                                 : OCT_E_STATE;
    pthread_mutex_unlock(&s->lock);
    return status;
}

oct_status oct_service_host_attached(oct_service *s)
{
    if (!s) return OCT_E_INVALID_ARGUMENT;
    pthread_mutex_lock(&s->lock);
    oct_status status = OCT_E_STATE;
    if (!s->stopping && atomic_load(&s->state) == OCT_SERVICE_WAITING) {
        status = set_state(s, OCT_SERVICE_CONNECTED);
        emit(s, OCT_EVENT_LOG, 0, false, "Host connected");
    }
    pthread_mutex_unlock(&s->lock);
    return status;
}

oct_status oct_service_host_detached(oct_service *s)
{
    if (!s) return OCT_E_INVALID_ARGUMENT;
    pthread_mutex_lock(&s->lock);
    oct_status status = OCT_E_STATE;
    if (!s->stopping && atomic_load(&s->state) == OCT_SERVICE_CONNECTED) {
        oct_responder_reset(s->responder);
        status = set_state(s, OCT_SERVICE_WAITING);
        emit(s, OCT_EVENT_LOG, 0, false, "Host disconnected");
    }
    pthread_mutex_unlock(&s->lock);
    return status;
}

oct_status oct_service_reset_session(oct_service *s)
{
    if (!s) return OCT_E_INVALID_ARGUMENT;
    pthread_mutex_lock(&s->lock);
    oct_status status = OCT_E_STATE;
    if (!s->stopping && s->responder && atomic_load(&s->state) == OCT_SERVICE_CONNECTED) {
        oct_responder_reset(s->responder);
        emit(s, OCT_EVENT_LOG, 0, false, "Session reset by the host");
        status = OCT_OK;
    }
    pthread_mutex_unlock(&s->lock);
    return status;
}

void oct_service_set_log_component(oct_service *s, const char *component)
{
    if (!s) return;
    pthread_mutex_lock(&s->lock);
    oct_copy_utf8(s->log_component, sizeof s->log_component, component ? component : "");
    pthread_mutex_unlock(&s->lock);
}

const char *oct_transport_state_name(oct_transport_state state)
{
    switch (state) {
    case OCT_TRANSPORT_UNAVAILABLE: return "Unavailable";
    case OCT_TRANSPORT_INITIALIZING: return "Initializing";
    case OCT_TRANSPORT_WAITING: return "Waiting for a connection";
    case OCT_TRANSPORT_HOST_CONNECTED: return "Host connected";
    case OCT_TRANSPORT_SESSION_ACTIVE: return "Session active";
    case OCT_TRANSPORT_ERROR: return "Error";
    case OCT_TRANSPORT_DISCONNECTED: return "Disconnected";
    }
    return "Unknown";
}

oct_status oct_service_receive(oct_service *s, const uint8_t *bytes, size_t length, oct_buf *out)
{
    if (!s || !out) return OCT_E_INVALID_ARGUMENT;
    pthread_mutex_lock(&s->lock);
    oct_status status = OCT_E_STATE;
    if (!s->stopping && s->responder && atomic_load(&s->state) == OCT_SERVICE_CONNECTED) {
        status = oct_responder_handle(s->responder, bytes, length, out);
    }
    pthread_mutex_unlock(&s->lock);
    return status;
}
