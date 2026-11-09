#include "test_support.h"
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>

void client_open(client *c, bool scratch)
{
    memset(c, 0, sizeof *c);
    c->store = open_synthetic(scratch);
    CHECK(c->store != NULL);
    CHECK_OK(oct_store_index_all(c->store));
    oct_responder_identity identity = { "Octopus", "Octopus File Provider", "0.1.0", "TEST-0001", "Test iPhone" };
    CHECK_OK(oct_responder_create(c->store, &identity, &c->responder));
    oct_buf_init(&c->scratch, 0);
}

void client_close(client *c)
{
    oct_responder_destroy(c->responder);
    oct_store_close(c->store);
    oct_buf_free(&c->scratch);
}

static oct_exchange exchange(client *c, uint16_t operation, uint32_t transaction, bool send, const void *data,
                             size_t length, int count, va_list args)
{
    uint32_t params[OCT_PTP_MAX_PARAMS] = { 0 };
    for (int i = 0; i < count && i < (int)OCT_PTP_MAX_PARAMS; i++) params[i] = va_arg(args, uint32_t);
    oct_exchange x;
    oct_status status = oct_exchange_run(c->responder, operation, transaction, params, (uint32_t)count, send, data,
                                         length, &c->scratch, &x);
    CHECK_OK(status);
    if (status != OCT_OK) memset(&x, 0, sizeof x);
    return x;
}

oct_exchange client_call(client *c, uint16_t operation, int count, ...)
{
    va_list args;
    va_start(args, count);
    oct_exchange x = exchange(c, operation, c->session ? c->transaction++ : 0, false, NULL, 0, count, args);
    va_end(args);
    return x;
}

oct_exchange client_send(client *c, uint16_t operation, const void *data, size_t length, int count, ...)
{
    va_list args;
    va_start(args, count);
    oct_exchange x = exchange(c, operation, c->session ? c->transaction++ : 0, true, data, length, count, args);
    va_end(args);
    return x;
}

static oct_exchange raw(client *c, uint16_t operation, uint32_t transaction, int count, ...)
{
    va_list args;
    va_start(args, count);
    oct_exchange x = exchange(c, operation, transaction, false, NULL, 0, count, args);
    va_end(args);
    return x;
}

void client_open_session(client *c)
{
    oct_exchange x = client_call(c, OCT_OP_OPEN_SESSION, 1, 1u);
    CHECK_EQ(x.response_code, OCT_RC_OK);
    c->session = true;
    c->transaction = 1;
}

size_t object_info_dataset(oct_buf *buffer, uint32_t storage, const char *name, uint16_t format, uint32_t size)
{
    oct_object_info info;
    memset(&info, 0, sizeof info);
    info.storage_id = storage;
    info.format = format;
    info.is_directory = format == OCT_FORMAT_ASSOCIATION;
    info.size = size;
    info.created = info.modified = SEED_TIME;
    snprintf(info.name, sizeof info.name, "%s", name);
    oct_buf_clear(buffer);
    oct_dataset_object_info(buffer, &info);
    return buffer->length;
}

uint32_t client_upload(client *c, uint32_t parent, const char *name, bool directory, const void *data, size_t length,
                       uint16_t *code)
{
    oct_buf dataset;
    oct_buf_init(&dataset, 0);
    object_info_dataset(&dataset, OCT_DEFAULT_STORAGE_ID, name,
                        directory ? OCT_FORMAT_ASSOCIATION : oct_format_for_name(name, false), (uint32_t)length);
    oct_exchange x = client_send(c, OCT_OP_SEND_OBJECT_INFO, dataset.data, dataset.length, 2,
                                 OCT_DEFAULT_STORAGE_ID, parent);
    oct_buf_free(&dataset);
    *code = x.response_code;
    if (x.response_code != OCT_RC_OK || x.param_count != 3) return 0;
    uint32_t handle = x.params[2];
    if (!directory) {
        x = client_send(c, OCT_OP_SEND_OBJECT, data, length, 0);
        *code = x.response_code;
    }
    return handle;
}

static uint32_t u32_at(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static uint32_t array_count(const oct_exchange *x)
{
    if (!x->has_data || x->data_length < 4) return 0xFFFFFFFFu;
    uint32_t count = u32_at(x->data);
    return x->data_length == 4 + 4ull * count ? count : 0xFFFFFFFEu;
}

static uint32_t array_item(const oct_exchange *x, uint32_t index) { return u32_at(x->data + 4 + 4 * index); }

static const char *string_at(const uint8_t *data, size_t length)
{
    static char text[1024];
    oct_reader reader;
    oct_reader_init(&reader, data, length);
    return oct_read_string(&reader, text, sizeof text) ? text : "(invalid)";
}

static void test_sessions(void)
{
    client c;
    client_open(&c, false);
    oct_exchange x = raw(&c, OCT_OP_GET_DEVICE_INFO, 0, 0);
    CHECK_EQ(x.response_code, OCT_RC_OK);
    CHECK(x.has_data && x.data_length > 60);
    CHECK_EQ(raw(&c, OCT_OP_GET_STORAGE_IDS, 0, 0).response_code, OCT_RC_SESSION_NOT_OPEN);
    CHECK_EQ(raw(&c, OCT_OP_OPEN_SESSION, 0, 1, 0u).response_code, OCT_RC_INVALID_PARAMETER);
    CHECK_EQ(raw(&c, OCT_OP_OPEN_SESSION, 0, 1, 7u).response_code, OCT_RC_OK);
    x = raw(&c, OCT_OP_OPEN_SESSION, 0, 1, 8u);
    CHECK_EQ(x.response_code, OCT_RC_SESSION_ALREADY_OPEN);
    CHECK(x.param_count == 1 && x.params[0] == 7);
    uint32_t session = 0;
    CHECK(oct_responder_session(c.responder, &session) && session == 7);
    CHECK_EQ(raw(&c, OCT_OP_GET_STORAGE_IDS, 1, 0).response_code, OCT_RC_OK);
    CHECK_EQ(raw(&c, OCT_OP_GET_STORAGE_IDS, 5, 0).response_code, OCT_RC_INVALID_TRANSACTION_ID);
    CHECK_EQ(raw(&c, OCT_OP_GET_STORAGE_IDS, 2, 0).response_code, OCT_RC_OK);
    CHECK_EQ(raw(&c, OCT_OP_GET_STORAGE_IDS, 2, 0).response_code, OCT_RC_INVALID_TRANSACTION_ID); /* replayed */
    CHECK_EQ(raw(&c, OCT_OP_GET_DEVICE_INFO, 3, 0).response_code, OCT_RC_OK);
    CHECK_EQ(raw(&c, OCT_OP_CLOSE_SESSION, 4, 0).response_code, OCT_RC_OK);
    CHECK(!oct_responder_session(c.responder, NULL));
    CHECK_EQ(raw(&c, OCT_OP_GET_STORAGE_IDS, 5, 0).response_code, OCT_RC_SESSION_NOT_OPEN);
    CHECK_EQ(raw(&c, OCT_OP_OPEN_SESSION, 0xFFFFFFFDu, 1, 9u).response_code, OCT_RC_OK);
    CHECK_EQ(raw(&c, OCT_OP_GET_STORAGE_IDS, 0xFFFFFFFEu, 0).response_code, OCT_RC_OK);
    CHECK_EQ(raw(&c, OCT_OP_GET_STORAGE_IDS, 0xFFFFFFFFu, 0).response_code, OCT_RC_INVALID_TRANSACTION_ID);
    CHECK_EQ(raw(&c, OCT_OP_GET_STORAGE_IDS, 1, 0).response_code, OCT_RC_OK); /* wraps past 0xFFFFFFFF and 0 */
    oct_responder_reset(c.responder);
    CHECK(!oct_responder_session(c.responder, NULL));
    CHECK_EQ(raw(&c, OCT_OP_GET_STORAGE_IDS, 2, 0).response_code, OCT_RC_SESSION_NOT_OPEN);
    oct_responder_stats stats;
    oct_responder_get_stats(c.responder, &stats);
    CHECK_EQ(stats.commands, 17);
    CHECK_EQ(stats.last_response, OCT_RC_SESSION_NOT_OPEN);
    client_close(&c);
}

static void test_enumeration_operations(void)
{
    client c;
    client_open(&c, false);
    client_open_session(&c);
    const uint32_t storage = OCT_DEFAULT_STORAGE_ID;
    oct_exchange x = client_call(&c, OCT_OP_GET_STORAGE_IDS, 0);
    CHECK(array_count(&x) == 1 && array_item(&x, 0) == storage);
    CHECK_EQ(client_call(&c, OCT_OP_GET_STORAGE_INFO, 1, storage).response_code, OCT_RC_OK);
    CHECK_EQ(client_call(&c, OCT_OP_GET_STORAGE_INFO, 1, 2u).response_code, OCT_RC_INVALID_STORAGE_ID);
    x = client_call(&c, OCT_OP_GET_NUM_OBJECTS, 3, 0xFFFFFFFFu, 0u, 0u);
    CHECK(x.response_code == OCT_RC_OK && !x.has_data && x.param_count == 1 && x.params[0] == 6);
    CHECK_EQ(client_call(&c, OCT_OP_GET_NUM_OBJECTS, 3, storage, 0u, 0xFFFFFFFFu).params[0], 3);
    CHECK_EQ(client_call(&c, OCT_OP_GET_NUM_OBJECTS, 3, storage, 0u, 2u).params[0], 2);
    CHECK_EQ(client_call(&c, OCT_OP_GET_NUM_OBJECTS, 3, storage, (uint32_t)OCT_FORMAT_TEXT, 0u).params[0], 3);
    CHECK_EQ(client_call(&c, OCT_OP_GET_NUM_OBJECTS, 3, storage, (uint32_t)OCT_FORMAT_ASSOCIATION, 0u).params[0], 2);
    CHECK_EQ(client_call(&c, OCT_OP_GET_NUM_OBJECTS, 3, storage, 0xFFFFFFFFu, 0u).params[0], 0); /* images only */
    CHECK_EQ(client_call(&c, OCT_OP_GET_NUM_OBJECTS, 3, 5u, 0u, 0u).response_code, OCT_RC_INVALID_STORAGE_ID);
    CHECK_EQ(client_call(&c, OCT_OP_GET_NUM_OBJECTS, 3, storage, 0u, 1u).response_code, OCT_RC_INVALID_PARENT_OBJECT);
    CHECK_EQ(client_call(&c, OCT_OP_GET_NUM_OBJECTS, 3, storage, 0u, 0x99u).response_code, OCT_RC_INVALID_OBJECT_HANDLE);
    CHECK_EQ(client_call(&c, OCT_OP_GET_NUM_OBJECTS, 3, storage, 0x10000u, 0u).response_code,
             OCT_RC_INVALID_OBJECT_FORMAT_CODE);
    x = client_call(&c, OCT_OP_GET_OBJECT_HANDLES, 3, storage, 0u, 0xFFFFFFFFu);
    CHECK(array_count(&x) == 3 && array_item(&x, 0) == 1 && array_item(&x, 1) == 2 && array_item(&x, 2) == 3);
    x = client_call(&c, OCT_OP_GET_OBJECT_HANDLES, 3, storage, 0u, 0u);
    CHECK_EQ(array_count(&x), 6);
    for (uint32_t i = 0; i < 6 && array_count(&x) == 6; i++) CHECK_EQ(array_item(&x, i), i + 1);
    x = client_call(&c, OCT_OP_GET_OBJECT_HANDLES, 3, storage, 0u, 2u);
    CHECK(array_count(&x) == 2 && array_item(&x, 0) == 4 && array_item(&x, 1) == 5);
    oct_object_info_request decoded;
    x = client_call(&c, OCT_OP_GET_OBJECT_INFO, 1, 1u);
    CHECK_OK(oct_dataset_parse_object_info(x.data, x.data_length, &decoded));
    CHECK_STR(decoded.name, "README.txt");
    CHECK_EQ(decoded.parent, 0);
    CHECK_EQ(decoded.format, OCT_FORMAT_TEXT);
    CHECK_EQ(decoded.protection_status, 1);
    x = client_call(&c, OCT_OP_GET_OBJECT_INFO, 1, 2u);
    CHECK_OK(oct_dataset_parse_object_info(x.data, x.data_length, &decoded));
    CHECK_EQ(decoded.association_type, OCT_ASSOCIATION_GENERIC_FOLDER);
    CHECK_EQ(decoded.format, OCT_FORMAT_ASSOCIATION);
    CHECK_EQ(client_call(&c, OCT_OP_GET_OBJECT_INFO, 1, 0x99u).response_code, OCT_RC_INVALID_OBJECT_HANDLE);
    x = client_call(&c, OCT_OP_GET_OBJECT, 1, 5u);
    CHECK(x.response_code == OCT_RC_OK && x.data_length == OCT_SAMPLE_BIN_SIZE);
    bool pattern = x.data_length == OCT_SAMPLE_BIN_SIZE;
    for (size_t i = 0; pattern && i < x.data_length; i++) pattern = x.data[i] == oct_sample_byte(i);
    CHECK(pattern);
    CHECK_EQ(client_call(&c, OCT_OP_GET_OBJECT, 1, 2u).response_code, OCT_RC_INVALID_OBJECT_HANDLE);
    CHECK_EQ(client_call(&c, OCT_OP_GET_OBJECT, 1, 0x99u).response_code, OCT_RC_INVALID_OBJECT_HANDLE);
    x = client_call(&c, OCT_OP_GET_PARTIAL_OBJECT, 3, 5u, 4000u, 200u);
    CHECK(x.response_code == OCT_RC_OK && x.data_length == 96 && x.param_count == 1 && x.params[0] == 96);
    CHECK(x.data_length == 96 && x.data[0] == oct_sample_byte(4000) && x.data[95] == oct_sample_byte(4095));
    x = client_call(&c, OCT_OP_GET_PARTIAL_OBJECT, 3, 5u, 4096u, 10u);
    CHECK(x.response_code == OCT_RC_OK && x.data_length == 0 && x.params[0] == 0);
    CHECK_EQ(client_call(&c, OCT_OP_GET_PARTIAL_OBJECT, 3, 5u, 4097u, 10u).response_code, OCT_RC_INVALID_PARAMETER);
    CHECK_EQ(client_call(&c, OCT_OP_GET_PARTIAL_OBJECT, 3, 5u, 0u, 0xFFFFFFFFu).params[0], OCT_SAMPLE_BIN_SIZE);
    CHECK_EQ(client_call(&c, OCT_OP_GET_PARTIAL_OBJECT, 3, 2u, 0u, 10u).response_code, OCT_RC_INVALID_OBJECT_HANDLE);
    x = client_call(&c, OCT_OP_GET_DEVICE_PROP_DESC, 1, OCT_DEVICE_PROP_FRIENDLY_NAME);
    CHECK(x.response_code == OCT_RC_OK && x.data_length > 6 && x.data[0] == 0x02 && x.data[1] == 0xD4 && x.data[4] == 0);
    CHECK_STR(string_at(x.data + 5, x.data_length - 5), "Test iPhone");
    x = client_call(&c, OCT_OP_GET_DEVICE_PROP_VALUE, 1, OCT_DEVICE_PROP_FRIENDLY_NAME);
    CHECK_STR(string_at(x.data, x.data_length), "Test iPhone");
    CHECK_EQ(client_call(&c, OCT_OP_GET_DEVICE_PROP_VALUE, 1, 0x5001u).response_code, OCT_RC_DEVICE_PROP_NOT_SUPPORTED);
    CHECK_EQ(client_call(&c, OCT_OP_GET_THUMB, 1, 1u).response_code, OCT_RC_OPERATION_NOT_SUPPORTED);
    CHECK_EQ(client_call(&c, OCT_OP_MOVE_OBJECT, 3, 4u, storage, 3u).response_code, OCT_RC_OPERATION_NOT_SUPPORTED);
    CHECK_EQ(client_call(&c, 0x9810, 1, 1u).response_code, OCT_RC_OPERATION_NOT_SUPPORTED);
    client_close(&c);
}

static void test_properties(void)
{
    client c;
    client_open(&c, false);
    client_open_session(&c);
    oct_exchange x = client_call(&c, OCT_OP_GET_OBJECT_PROPS_SUPPORTED, 1, (uint32_t)OCT_FORMAT_UNDEFINED);
    CHECK(x.response_code == OCT_RC_OK && x.data_length == 4 + 2 * 10 && u32_at(x.data) == 10);
    CHECK_EQ(client_call(&c, OCT_OP_GET_OBJECT_PROPS_SUPPORTED, 1, 0x3801u).response_code,
             OCT_RC_INVALID_OBJECT_FORMAT_CODE);
    x = client_call(&c, OCT_OP_GET_OBJECT_PROP_DESC, 2, (uint32_t)OCT_PROP_OBJECT_FILE_NAME, (uint32_t)OCT_FORMAT_UNDEFINED);
    CHECK(x.response_code == OCT_RC_OK && x.data_length > 4 && x.data[4] == 0x00); /* read-only storage */
    CHECK_EQ(client_call(&c, OCT_OP_GET_OBJECT_PROP_DESC, 2, 0xDC99u, (uint32_t)OCT_FORMAT_UNDEFINED).response_code,
             OCT_RC_INVALID_OBJECT_PROP_CODE);
    CHECK_EQ(client_call(&c, OCT_OP_GET_OBJECT_PROP_DESC, 2, (uint32_t)OCT_PROP_OBJECT_FILE_NAME, 0x3801u).response_code,
             OCT_RC_INVALID_OBJECT_FORMAT_CODE);
    x = client_call(&c, OCT_OP_GET_OBJECT_PROP_VALUE, 2, 4u, (uint32_t)OCT_PROP_OBJECT_FILE_NAME);
    CHECK_STR(string_at(x.data, x.data_length), "example.txt");
    oct_object_info example;
    CHECK_OK(oct_store_get(c.store, 4, &example));
    x = client_call(&c, OCT_OP_GET_OBJECT_PROP_VALUE, 2, 4u, (uint32_t)OCT_PROP_OBJECT_SIZE);
    CHECK(x.data_length == 8 && u32_at(x.data) == example.size && u32_at(x.data + 4) == 0);
    x = client_call(&c, OCT_OP_GET_OBJECT_PROP_VALUE, 2, 4u, (uint32_t)OCT_PROP_PARENT_OBJECT);
    CHECK(x.data_length == 4 && u32_at(x.data) == 2);
    x = client_call(&c, OCT_OP_GET_OBJECT_PROP_VALUE, 2, 4u, (uint32_t)OCT_PROP_PROTECTION_STATUS);
    CHECK(x.data_length == 2 && x.data[0] == 1 && x.data[1] == 0);
    x = client_call(&c, OCT_OP_GET_OBJECT_PROP_VALUE, 2, 4u, (uint32_t)OCT_PROP_PERSISTENT_UID);
    CHECK_EQ(x.data_length, 16);
    x = client_call(&c, OCT_OP_GET_OBJECT_PROP_VALUE, 2, 4u, (uint32_t)OCT_PROP_DATE_CREATED);
    CHECK_STR(string_at(x.data, x.data_length), "20261107T000000");
    CHECK_EQ(client_call(&c, OCT_OP_GET_OBJECT_PROP_VALUE, 2, 0x99u, (uint32_t)OCT_PROP_OBJECT_FILE_NAME).response_code,
             OCT_RC_INVALID_OBJECT_HANDLE);
    CHECK_EQ(client_call(&c, OCT_OP_GET_OBJECT_PROP_VALUE, 2, 4u, 0xDC99u).response_code, OCT_RC_INVALID_OBJECT_PROP_CODE);
    x = client_call(&c, OCT_OP_GET_OBJECT_PROP_LIST, 5, 0u, 0u, 0xFFFFFFFFu, 0u, 1u);
    CHECK(x.response_code == OCT_RC_OK && x.data_length > 16 && u32_at(x.data) == 30);
    CHECK(x.data_length > 16 && u32_at(x.data + 4) == 1 && x.data[8] == 0x01 && x.data[9] == 0xDC && x.data[10] == 0x06 &&
          u32_at(x.data + 12) == OCT_DEFAULT_STORAGE_ID);
    x = client_call(&c, OCT_OP_GET_OBJECT_PROP_LIST, 5, 2u, 0u, (uint32_t)OCT_PROP_OBJECT_FILE_NAME, 0u, 1u);
    CHECK(x.response_code == OCT_RC_OK && x.data_length > 12 && u32_at(x.data) == 2 && u32_at(x.data + 4) == 4);
    if (x.data_length > 12) CHECK_STR(string_at(x.data + 12, x.data_length - 12), "example.txt");
    CHECK_EQ(u32_at(client_call(&c, OCT_OP_GET_OBJECT_PROP_LIST, 5, 1u, 0u, (uint32_t)OCT_PROP_OBJECT_SIZE, 0u, 0u).data), 1);
    CHECK_EQ(u32_at(client_call(&c, OCT_OP_GET_OBJECT_PROP_LIST, 5, 0xFFFFFFFFu, 0u, (uint32_t)OCT_PROP_OBJECT_FILE_NAME, 0u, 0u).data), 6);
    CHECK_EQ(u32_at(client_call(&c, OCT_OP_GET_OBJECT_PROP_LIST, 5, 1u, 0u, (uint32_t)OCT_PROP_OBJECT_FILE_NAME, 0u, 1u).data), 0);
    CHECK_EQ(u32_at(client_call(&c, OCT_OP_GET_OBJECT_PROP_LIST, 5, 0u, (uint32_t)OCT_FORMAT_ASSOCIATION, (uint32_t)OCT_PROP_OBJECT_FILE_NAME, 0u, 1u).data), 2);
    CHECK_EQ(u32_at(client_call(&c, OCT_OP_GET_OBJECT_PROP_LIST, 5, 0u, 0u, (uint32_t)OCT_PROP_OBJECT_FILE_NAME, 0u, 0u).data), 0);
    CHECK_EQ(client_call(&c, OCT_OP_GET_OBJECT_PROP_LIST, 5, 0u, 0u, 0u, 5u, 0u).response_code,
             OCT_RC_SPECIFICATION_BY_GROUP_UNSUPPORTED);
    CHECK_EQ(client_call(&c, OCT_OP_GET_OBJECT_PROP_LIST, 5, 1u, 0u, (uint32_t)OCT_PROP_OBJECT_FILE_NAME, 0u, 2u).response_code,
             OCT_RC_SPECIFICATION_BY_DEPTH_UNSUPPORTED);
    CHECK_EQ(client_call(&c, OCT_OP_GET_OBJECT_PROP_LIST, 5, 0x99u, 0u, (uint32_t)OCT_PROP_OBJECT_FILE_NAME, 0u, 0u).response_code,
             OCT_RC_INVALID_OBJECT_HANDLE);
    CHECK_EQ(client_call(&c, OCT_OP_GET_OBJECT_PROP_LIST, 5, 1u, 0u, 0xDC99u, 0u, 0u).response_code,
             OCT_RC_INVALID_OBJECT_PROP_CODE);
    client_close(&c);
    client_open(&c, true);
    client_open_session(&c);
    x = client_call(&c, OCT_OP_GET_OBJECT_PROP_DESC, 2, (uint32_t)OCT_PROP_OBJECT_FILE_NAME, (uint32_t)OCT_FORMAT_TEXT);
    CHECK(x.data_length > 4 && x.data[4] == 0x01); /* settable where a writable area exists */
    x = client_call(&c, OCT_OP_GET_OBJECT_PROP_DESC, 2, (uint32_t)OCT_PROP_STORAGE_ID, (uint32_t)OCT_FORMAT_TEXT);
    CHECK(x.data_length > 4 && x.data[4] == 0x00);
    client_close(&c);
}

static void raw_command(client *c, uint16_t operation)
{
    oct_buf request;
    oct_buf_init(&request, 0);
    oct_container_command(&request, operation, c->transaction++, NULL, 0);
    oct_buf_clear(&c->scratch);
    CHECK_OK(oct_responder_handle(c->responder, request.data, request.length, &c->scratch));
    CHECK_EQ(c->scratch.length, 0);
    oct_buf_free(&request);
}

static void test_uploads(void)
{
    client c;
    client_open(&c, true);
    client_open_session(&c);
    const uint32_t storage = OCT_DEFAULT_STORAGE_ID, scratch = 4;
    uint16_t code = 0;
    oct_buf dataset;
    oct_buf_init(&dataset, 0);
    object_info_dataset(&dataset, storage, "upload.bin", OCT_FORMAT_UNDEFINED, 10);
    oct_exchange x = client_send(&c, OCT_OP_SEND_OBJECT_INFO, dataset.data, dataset.length, 2, storage, scratch);
    CHECK(x.response_code == OCT_RC_OK && x.param_count == 3 && x.params[0] == storage && x.params[1] == scratch &&
          x.params[2] == 8);
    x = client_send(&c, OCT_OP_SEND_OBJECT, "0123456789", 10, 0);
    CHECK_EQ(x.response_code, OCT_RC_OK);
    x = client_call(&c, OCT_OP_GET_OBJECT, 1, 8u);
    CHECK(x.data_length == 10 && memcmp(x.data, "0123456789", 10) == 0);
    oct_object_info_request decoded;
    x = client_call(&c, OCT_OP_GET_OBJECT_INFO, 1, 8u);
    CHECK_OK(oct_dataset_parse_object_info(x.data, x.data_length, &decoded));
    CHECK(decoded.protection_status == 0 && decoded.parent == scratch && decoded.compressed_size == 10);
    uint32_t empty = client_upload(&c, scratch, "empty.txt", false, "", 0, &code);
    CHECK_EQ(code, OCT_RC_OK);
    x = client_call(&c, OCT_OP_GET_OBJECT, 1, empty);
    CHECK(x.response_code == OCT_RC_OK && x.has_data && x.data_length == 0);
    uint32_t folder = client_upload(&c, scratch, "Folder", true, NULL, 0, &code);
    CHECK_EQ(code, OCT_RC_OK);
    CHECK_EQ(array_count(&(oct_exchange){ 0 }), 0xFFFFFFFFu);
    x = client_call(&c, OCT_OP_GET_OBJECT_HANDLES, 3, storage, 0u, folder);
    CHECK_EQ(array_count(&x), 0);
    uint32_t inner = client_upload(&c, folder, "inner.txt", false, "abc", 3, &code);
    CHECK(code == OCT_RC_OK && inner != 0);
    x = client_call(&c, OCT_OP_GET_OBJECT_HANDLES, 3, storage, 0u, folder);
    CHECK(array_count(&x) == 1 && array_item(&x, 0) == inner);
    client_upload(&c, scratch, "upload.bin", false, "x", 1, &code);
    CHECK_EQ(code, OCT_RC_GENERAL_ERROR); /* name exists */
    client_upload(&c, scratch, "a/b", false, "x", 1, &code);
    CHECK_EQ(code, OCT_RC_INVALID_DATASET);
    client_upload(&c, 2u, "x.txt", false, "x", 1, &code);
    CHECK_EQ(code, OCT_RC_ACCESS_DENIED); /* Documents is read-only */
    client_upload(&c, 0xFFFFFFFFu, "x.txt", false, "x", 1, &code);
    CHECK_EQ(code, OCT_RC_ACCESS_DENIED); /* so is the root */
    client_upload(&c, 1u, "x.txt", false, "x", 1, &code);
    CHECK_EQ(code, OCT_RC_INVALID_PARENT_OBJECT);
    client_upload(&c, 0x99u, "x.txt", false, "x", 1, &code);
    CHECK_EQ(code, OCT_RC_INVALID_PARENT_OBJECT);
    object_info_dataset(&dataset, storage, "x.txt", OCT_FORMAT_TEXT, 1);
    CHECK_EQ(client_send(&c, OCT_OP_SEND_OBJECT_INFO, dataset.data, dataset.length, 2, 5u, scratch).response_code,
             OCT_RC_INVALID_STORAGE_ID);
    CHECK_EQ(client_send(&c, OCT_OP_SEND_OBJECT_INFO, dataset.data, 5, 2, storage, scratch).response_code,
             OCT_RC_INVALID_DATASET);
    object_info_dataset(&dataset, storage, "short.bin", OCT_FORMAT_UNDEFINED, 10);
    x = client_send(&c, OCT_OP_SEND_OBJECT_INFO, dataset.data, dataset.length, 2, storage, scratch);
    uint32_t short_handle = x.params[2];
    CHECK_EQ(client_send(&c, OCT_OP_SEND_OBJECT, "1234", 4, 0).response_code, OCT_RC_INCOMPLETE_TRANSFER);
    CHECK_EQ(client_call(&c, OCT_OP_GET_OBJECT_INFO, 1, short_handle).response_code, OCT_RC_INVALID_OBJECT_HANDLE);
    CHECK_EQ(client_send(&c, OCT_OP_SEND_OBJECT, "1234", 4, 0).response_code, OCT_RC_NO_VALID_OBJECT_INFO);
    object_info_dataset(&dataset, storage, "huge.bin", OCT_FORMAT_UNDEFINED, 0xFFFFFFFFu);
    CHECK_EQ(client_send(&c, OCT_OP_SEND_OBJECT_INFO, dataset.data, dataset.length, 2, storage, scratch).response_code,
             OCT_RC_OBJECT_TOO_LARGE);
    object_info_dataset(&dataset, storage, "huge.bin", OCT_FORMAT_UNDEFINED, OCT_PTP_MAX_TRANSFER + 1u);
    CHECK_EQ(client_send(&c, OCT_OP_SEND_OBJECT_INFO, dataset.data, dataset.length, 2, storage, scratch).response_code,
             OCT_RC_OBJECT_TOO_LARGE);
    object_info_dataset(&dataset, storage, "huge.bin", OCT_FORMAT_UNDEFINED, 20u * 1024u * 1024u);
    CHECK_EQ(client_send(&c, OCT_OP_SEND_OBJECT_INFO, dataset.data, dataset.length, 2, storage, scratch).response_code,
             OCT_RC_STORE_FULL);
    /* A reservation survives unrelated commands, as MTP hosts expect. */
    object_info_dataset(&dataset, storage, "pending.txt", OCT_FORMAT_TEXT, 5);
    uint32_t pending = client_send(&c, OCT_OP_SEND_OBJECT_INFO, dataset.data, dataset.length, 2, storage, scratch).params[2];
    CHECK_EQ(client_call(&c, OCT_OP_GET_STORAGE_IDS, 0).response_code, OCT_RC_OK);
    CHECK_EQ(client_send(&c, OCT_OP_SEND_OBJECT, "12345", 5, 0).response_code, OCT_RC_OK);
    CHECK_EQ(client_call(&c, OCT_OP_GET_OBJECT, 1, pending).data_length, 5);
    /* A SendObject whose data phase never arrives releases the reservation. */
    object_info_dataset(&dataset, storage, "abandoned.txt", OCT_FORMAT_TEXT, 5);
    uint32_t abandoned = client_send(&c, OCT_OP_SEND_OBJECT_INFO, dataset.data, dataset.length, 2, storage, scratch).params[2];
    raw_command(&c, OCT_OP_SEND_OBJECT);
    CHECK(oct_responder_awaiting_data(c.responder));
    CHECK_EQ(client_call(&c, OCT_OP_GET_STORAGE_IDS, 0).response_code, OCT_RC_OK);
    CHECK(!oct_responder_awaiting_data(c.responder));
    CHECK_EQ(client_call(&c, OCT_OP_GET_OBJECT_INFO, 1, abandoned).response_code, OCT_RC_INVALID_OBJECT_HANDLE);
    oct_responder_stats stats;
    oct_responder_get_stats(c.responder, &stats);
    CHECK_EQ(stats.abandoned, 1);
    /* A second SendObjectInfo replaces an unfinished one. */
    object_info_dataset(&dataset, storage, "first.txt", OCT_FORMAT_TEXT, 3);
    uint32_t first = client_send(&c, OCT_OP_SEND_OBJECT_INFO, dataset.data, dataset.length, 2, storage, scratch).params[2];
    object_info_dataset(&dataset, storage, "second.txt", OCT_FORMAT_TEXT, 3);
    uint32_t second = client_send(&c, OCT_OP_SEND_OBJECT_INFO, dataset.data, dataset.length, 2, storage, scratch).params[2];
    CHECK_EQ(client_call(&c, OCT_OP_GET_OBJECT_INFO, 1, first).response_code, OCT_RC_INVALID_OBJECT_HANDLE);
    CHECK_EQ(client_send(&c, OCT_OP_SEND_OBJECT, "abc", 3, 0).response_code, OCT_RC_OK);
    CHECK_EQ(client_call(&c, OCT_OP_GET_OBJECT, 1, second).data_length, 3);
    /* Closing the session discards a reservation. */
    object_info_dataset(&dataset, storage, "third.txt", OCT_FORMAT_TEXT, 3);
    uint32_t third = client_send(&c, OCT_OP_SEND_OBJECT_INFO, dataset.data, dataset.length, 2, storage, scratch).params[2];
    CHECK_EQ(client_call(&c, OCT_OP_CLOSE_SESSION, 0).response_code, OCT_RC_OK);
    c.session = false;
    client_open_session(&c);
    CHECK_EQ(client_call(&c, OCT_OP_GET_OBJECT_INFO, 1, third).response_code, OCT_RC_INVALID_OBJECT_HANDLE);
    oct_buf_free(&dataset);
    client_close(&c);
}

static oct_exchange set_name(client *c, uint32_t handle, uint32_t property, const char *name)
{
    oct_buf value;
    oct_buf_init(&value, 0);
    oct_buf_string(&value, name);
    oct_exchange x = client_send(c, OCT_OP_SET_OBJECT_PROP_VALUE, value.data, value.length, 2, handle, property);
    oct_buf_free(&value);
    return x;
}

static void test_rename_and_delete(void)
{
    client c;
    client_open(&c, true);
    client_open_session(&c);
    const uint32_t scratch = 4;
    uint16_t code = 0;
    uint32_t file = client_upload(&c, scratch, "r.txt", false, "data", 4, &code);
    uint32_t other = client_upload(&c, scratch, "other.txt", false, "x", 1, &code);
    CHECK(file && other);
    CHECK_EQ(set_name(&c, file, OCT_PROP_OBJECT_FILE_NAME, "s.txt").response_code, OCT_RC_OK);
    oct_exchange x = client_call(&c, OCT_OP_GET_OBJECT_PROP_VALUE, 2, file, (uint32_t)OCT_PROP_OBJECT_FILE_NAME);
    CHECK_STR(string_at(x.data, x.data_length), "s.txt");
    CHECK_EQ(set_name(&c, file, OCT_PROP_NAME, "t.txt").response_code, OCT_RC_OK);
    oct_object_info info;
    CHECK_OK(oct_store_get(c.store, file, &info));
    CHECK_STR(info.name, "t.txt");
    CHECK_EQ(set_name(&c, file, OCT_PROP_OBJECT_FILE_NAME, "u/v").response_code, OCT_RC_INVALID_OBJECT_PROP_VALUE);
    CHECK_EQ(set_name(&c, file, OCT_PROP_OBJECT_FILE_NAME, "other.txt").response_code, OCT_RC_INVALID_OBJECT_PROP_VALUE);
    CHECK_EQ(set_name(&c, 1, OCT_PROP_OBJECT_FILE_NAME, "x.txt").response_code, OCT_RC_OBJECT_WRITE_PROTECTED);
    CHECK_EQ(set_name(&c, file, OCT_PROP_OBJECT_SIZE, "1").response_code, OCT_RC_ACCESS_DENIED);
    CHECK_EQ(set_name(&c, file, 0xDC99u, "1").response_code, OCT_RC_INVALID_OBJECT_PROP_CODE);
    CHECK_EQ(set_name(&c, 0x99u, OCT_PROP_OBJECT_FILE_NAME, "1").response_code, OCT_RC_INVALID_OBJECT_HANDLE);
    const uint8_t broken[] = { 3, 'a', 0 };
    CHECK_EQ(client_send(&c, OCT_OP_SET_OBJECT_PROP_VALUE, broken, sizeof broken, 2, file,
                         (uint32_t)OCT_PROP_OBJECT_FILE_NAME).response_code, OCT_RC_INVALID_OBJECT_PROP_FORMAT);
    const uint8_t trailing[] = { 2, 'z', 0, 0, 0, 0xEE };
    CHECK_EQ(client_send(&c, OCT_OP_SET_OBJECT_PROP_VALUE, trailing, sizeof trailing, 2, file,
                         (uint32_t)OCT_PROP_OBJECT_FILE_NAME).response_code, OCT_RC_INVALID_OBJECT_PROP_FORMAT);
    CHECK_EQ(client_call(&c, OCT_OP_DELETE_OBJECT, 2, file, 0u).response_code, OCT_RC_OK);
    CHECK_EQ(client_call(&c, OCT_OP_DELETE_OBJECT, 2, file, 0u).response_code, OCT_RC_INVALID_OBJECT_HANDLE);
    CHECK_EQ(client_call(&c, OCT_OP_DELETE_OBJECT, 2, 1u, 0u).response_code, OCT_RC_OBJECT_WRITE_PROTECTED);
    CHECK_EQ(client_call(&c, OCT_OP_DELETE_OBJECT, 2, 2u, 0u).response_code, OCT_RC_OBJECT_WRITE_PROTECTED);
    uint32_t tree = client_upload(&c, scratch, "Tree", true, NULL, 0, &code);
    uint32_t leaf = client_upload(&c, tree, "leaf.txt", false, "leaf", 4, &code);
    CHECK_EQ(client_call(&c, OCT_OP_DELETE_OBJECT, 2, tree, 0u).response_code, OCT_RC_OK);
    CHECK_EQ(client_call(&c, OCT_OP_GET_OBJECT_INFO, 1, leaf).response_code, OCT_RC_INVALID_OBJECT_HANDLE);
    CHECK_EQ(client_call(&c, OCT_OP_DELETE_OBJECT, 2, 0xFFFFFFFFu, (uint32_t)OCT_FORMAT_UNDEFINED).response_code,
             OCT_RC_SPECIFICATION_BY_FORMAT_UNSUPPORTED);
    CHECK_EQ(client_call(&c, OCT_OP_DELETE_OBJECT, 2, 0xFFFFFFFFu, 0u).response_code, OCT_RC_PARTIAL_DELETION);
    x = client_call(&c, OCT_OP_GET_OBJECT_HANDLES, 3, OCT_DEFAULT_STORAGE_ID, 0u, 0xFFFFFFFFu);
    CHECK_EQ(array_count(&x), 3); /* only the protected seed objects remain */
    client_close(&c);

    client_open(&c, false);
    client_open_session(&c);
    CHECK_EQ(client_call(&c, OCT_OP_DELETE_OBJECT, 2, 1u, 0u).response_code, OCT_RC_STORE_READ_ONLY);
    client_upload(&c, 0xFFFFFFFFu, "x.txt", false, "x", 1, &code);
    CHECK_EQ(code, OCT_RC_STORE_READ_ONLY);
    CHECK_EQ(set_name(&c, 1, OCT_PROP_OBJECT_FILE_NAME, "x.txt").response_code, OCT_RC_OBJECT_WRITE_PROTECTED);
    client_close(&c);
}

static void check_output_framing(const oct_buf *out)
{
    size_t position = 0;
    while (position < out->length) {
        CHECK(out->length - position >= 12);
        if (out->length - position < 12) return;
        uint32_t length = u32_at(out->data + position);
        uint16_t type = (uint16_t)(out->data[position + 4] | out->data[position + 5] << 8);
        CHECK(length >= 12 && length <= out->length - position);
        CHECK(type == OCT_CONTAINER_DATA || type == OCT_CONTAINER_RESPONSE);
        if (length < 12 || length > out->length - position) return;
        position += length;
    }
}

static oct_status feed(client *c, const void *bytes, size_t length)
{
    oct_buf_clear(&c->scratch);
    oct_status status = oct_responder_handle(c->responder, bytes, length, &c->scratch);
    check_output_framing(&c->scratch);
    return status;
}

static void test_malformed(void)
{
    client c;
    client_open(&c, true);
    client_open_session(&c);
    const uint8_t tiny[5] = { 5, 0, 0, 0, 1 };
    CHECK_EQ(feed(&c, tiny, sizeof tiny), OCT_E_MALFORMED);
    CHECK_EQ(c.scratch.length, 0);
    const uint8_t lying[12] = { 16, 0, 0, 0, 1, 0, 0x04, 0x10, 1, 0, 0, 0 };
    CHECK_EQ(feed(&c, lying, sizeof lying), OCT_E_MALFORMED);
    CHECK(c.scratch.length == 12 && c.scratch.data[6] == 0x02 && c.scratch.data[7] == 0x20 && c.scratch.data[8] == 1);
    const uint8_t reply[12] = { 12, 0, 0, 0, 3, 0, 0x01, 0x20, 1, 0, 0, 0 };
    CHECK_EQ(feed(&c, reply, sizeof reply), OCT_E_MALFORMED);
    CHECK_EQ(c.scratch.length, 0);
    uint8_t six[36] = { 36, 0, 0, 0, 1, 0, 0x07, 0x10, 1, 0, 0, 0 };
    CHECK_EQ(feed(&c, six, sizeof six), OCT_E_MALFORMED);
    CHECK_EQ(c.scratch.length, 12);
    const uint8_t stray[14] = { 14, 0, 0, 0, 2, 0, 0x0D, 0x10, 1, 0, 0, 0, 'h', 'i' };
    CHECK_EQ(feed(&c, stray, sizeof stray), OCT_E_STATE);
    CHECK_EQ(c.scratch.length, 0);
    /* Data containers must match the pending command's code and transaction. */
    oct_buf request, dataset;
    oct_buf_init(&request, 0);
    oct_buf_init(&dataset, 0);
    uint32_t params[2] = { OCT_DEFAULT_STORAGE_ID, 4 };
    uint32_t transaction = c.transaction++;
    oct_container_command(&request, OCT_OP_SEND_OBJECT_INFO, transaction, params, 2);
    CHECK_OK(feed(&c, request.data, request.length));
    object_info_dataset(&dataset, OCT_DEFAULT_STORAGE_ID, "framed.txt", OCT_FORMAT_TEXT, 2);
    oct_buf_clear(&request);
    size_t start = oct_container_data_begin(&request, OCT_OP_SEND_OBJECT_INFO, transaction + 1);
    oct_buf_bytes(&request, dataset.data, dataset.length);
    oct_container_data_end(&request, start);
    CHECK_EQ(feed(&c, request.data, request.length), OCT_E_STATE);
    CHECK(oct_responder_awaiting_data(c.responder));
    request.data[6] = 0x0D; /* SendObject code, correct transaction */
    request.data[8] = (uint8_t)transaction;
    CHECK_EQ(feed(&c, request.data, request.length), OCT_E_STATE);
    request.data[6] = 0x0C;
    CHECK_OK(feed(&c, request.data, request.length));
    CHECK(c.scratch.length == 24 && c.scratch.data[6] == 0x01 && c.scratch.data[7] == 0x20);
    uint32_t framed = u32_at(c.scratch.data + 20);
    /* A damaged SendObject data phase fails the transfer and frees the reservation. */
    transaction = c.transaction++;
    oct_buf_clear(&request);
    oct_container_command(&request, OCT_OP_SEND_OBJECT, transaction, NULL, 0);
    CHECK_OK(feed(&c, request.data, request.length));
    const uint8_t damaged[14] = { 20, 0, 0, 0, 2, 0, 0x0D, 0x10, (uint8_t)transaction, 0, 0, 0, 'o', 'k' };
    CHECK_EQ(feed(&c, damaged, sizeof damaged), OCT_E_MALFORMED);
    CHECK(c.scratch.length == 12 && c.scratch.data[6] == 0x07 && c.scratch.data[7] == 0x20);
    CHECK_EQ(client_call(&c, OCT_OP_GET_OBJECT_INFO, 1, framed).response_code, OCT_RC_INVALID_OBJECT_HANDLE);
    /* Every prefix of a valid data phase is handled without leaving bounds. */
    object_info_dataset(&dataset, OCT_DEFAULT_STORAGE_ID, "prefix.txt", OCT_FORMAT_TEXT, 1);
    for (size_t cut = 0; cut <= dataset.length + 12; cut++) {
        transaction = c.transaction++;
        oct_buf_clear(&request);
        oct_container_command(&request, OCT_OP_SEND_OBJECT_INFO, transaction, params, 2);
        feed(&c, request.data, request.length);
        oct_buf_clear(&request);
        start = oct_container_data_begin(&request, OCT_OP_SEND_OBJECT_INFO, transaction);
        oct_buf_bytes(&request, dataset.data, dataset.length);
        oct_container_data_end(&request, start);
        oct_status status = feed(&c, request.data, cut);
        if (cut == request.length) CHECK_OK(status);
        if (cut >= 12 && cut < request.length) CHECK_EQ(status, OCT_E_MALFORMED);
    }
    oct_responder_stats stats;
    oct_responder_get_stats(c.responder, &stats);
    CHECK(stats.malformed >= 5);
    oct_buf_free(&request);
    oct_buf_free(&dataset);
    client_close(&c);
}

static size_t parse_hex(const char *text, uint8_t *out, size_t capacity)
{
    size_t count = 0;
    int high = -1;
    for (const char *p = text; *p && *p != '#'; p++) {
        int value;
        if (*p >= '0' && *p <= '9') value = *p - '0';
        else if (*p >= 'a' && *p <= 'f') value = *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') value = *p - 'A' + 10;
        else continue;
        if (high < 0) {
            high = value;
        } else if (count < capacity) {
            out[count++] = (uint8_t)(high << 4 | value);
            high = -1;
        }
    }
    return count;
}

static void compare_exchange(int index, const oct_buf *actual, const uint8_t *expected, size_t length)
{
    bool same = actual->length == length && (length == 0 || memcmp(actual->data, expected, length) == 0);
    CHECK(same);
    if (!same) {
        size_t at = 0;
        while (at < length && at < actual->length && actual->data[at] == expected[at]) at++;
        fprintf(stderr, "  transcript exchange %d: %zu bytes, expected %zu; first difference at %zu\n", index,
                actual->length, length, at);
    }
}

static void test_transcript(void)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/explorer-enumeration.ptp", fixture_directory);
    FILE *file = fopen(path, "r");
    CHECK(file != NULL);
    if (!file) return;
    client c;
    client_open(&c, false);
    oct_buf actual;
    oct_buf_init(&actual, 0);
    size_t capacity = 1u << 20, expected_length = 0;
    uint8_t *expected = malloc(capacity), *request = malloc(capacity);
    char *line = NULL;
    size_t line_capacity = 0;
    int exchanges = 0;
    while (getline(&line, &line_capacity, file) > 0) {
        if (line[0] == '>') {
            if (exchanges) compare_exchange(exchanges, &actual, expected, expected_length);
            size_t length = parse_hex(line + 1, request, capacity);
            oct_buf_clear(&actual);
            oct_responder_handle(c.responder, request, length, &actual);
            expected_length = 0;
            exchanges++;
        } else if (line[0] == '<') {
            expected_length += parse_hex(line + 1, expected + expected_length, capacity - expected_length);
        }
    }
    if (exchanges) compare_exchange(exchanges, &actual, expected, expected_length);
    CHECK(exchanges >= 20);
    free(line);
    free(expected);
    free(request);
    fclose(file);
    oct_buf_free(&actual);
    client_close(&c);
}

/* A small file at `path`, for the directory-backed tests below. */
static void write_file(const char *path, const char *text)
{
    FILE *file = fopen(path, "w");
    CHECK(file != NULL);
    if (file) {
        CHECK(fputs(text, file) >= 0);
        CHECK_EQ(fclose(file), 0);
    }
}

/*
 * A host asking for the whole hierarchy over a store whose budget can't cover it must be told
 * that specification by depth is unsupported, and must still be able to enumerate one directory
 * at a time. Without this the responder would walk an entire filesystem inside one USB
 * transaction. See oct_store_options.max_objects.
 */
static void test_whole_hierarchy_over_budget(void)
{
    char root[] = "/tmp/octopus-depth-XXXXXX", path[512];
    CHECK(mkdtemp(root) != NULL);
    snprintf(path, sizeof path, "%s/%s", root, OCT_DISPOSABLE_MARKER);
    write_file(path, "disposable\n");
    for (int d = 0; d < 4; d++) {
        snprintf(path, sizeof path, "%s/dir%d", root, d);
        CHECK_EQ(mkdir(path, 0755), 0);
        for (int f = 0; f < 5; f++) {
            snprintf(path, sizeof path, "%s/dir%d/f%d.txt", root, d, f);
            write_file(path, "y");
        }
    }
    oct_backend backend;
    oct_store *store = NULL;
    oct_responder *r = NULL;
    oct_store_options options = { .max_objects = 12 };
    CHECK_OK(oct_directory_backend_create(root, &backend));
    CHECK_OK(oct_store_open(&options, backend, &store));
    CHECK_OK(oct_responder_create(store, NULL, &r));
    oct_buf out;
    oct_buf_init(&out, 0);
    oct_exchange exchange = { 0 };
    uint32_t session = 1;
    CHECK_OK(oct_exchange_run(r, OCT_OP_OPEN_SESSION, 0, &session, 1, false, NULL, 0, &out, &exchange));

    uint32_t whole[3] = { 0xFFFFFFFFu, 0, 0 };          /* association 0: everything */
    CHECK_OK(oct_exchange_run(r, OCT_OP_GET_OBJECT_HANDLES, 1, whole, 3, false, NULL, 0, &out, &exchange));
    CHECK_EQ(exchange.response_code, OCT_RC_SPECIFICATION_BY_DEPTH_UNSUPPORTED);
    CHECK(!exchange.has_data);

    uint32_t top[3] = { 0xFFFFFFFFu, 0, 0xFFFFFFFFu };  /* association 0xFFFFFFFF: the root level */
    CHECK_OK(oct_exchange_run(r, OCT_OP_GET_OBJECT_HANDLES, 2, top, 3, false, NULL, 0, &out, &exchange));
    CHECK_EQ(exchange.response_code, OCT_RC_OK);
    CHECK(exchange.has_data);
    CHECK_EQ(exchange.data_length, 4u + 4u * 4u);       /* a count and the four directories */

    /* GetNumObjects takes the same path, so it answers the same way. */
    CHECK_OK(oct_exchange_run(r, OCT_OP_GET_NUM_OBJECTS, 3, whole, 3, false, NULL, 0, &out, &exchange));
    CHECK_EQ(exchange.response_code, OCT_RC_SPECIFICATION_BY_DEPTH_UNSUPPORTED);

    oct_buf_free(&out);
    oct_responder_destroy(r);
    oct_store_close(store);
}

void run_responder_tests(void)
{
    test_whole_hierarchy_over_budget();
    test_sessions();
    test_enumeration_operations();
    test_properties();
    test_uploads();
    test_rename_and_delete();
    test_malformed();
    test_transcript();
}
