#include "test_support.h"

static void test_container_parse(void)
{
    oct_container c;
    const uint8_t open[] = { 0x10, 0, 0, 0, 0x01, 0, 0x02, 0x10, 0, 0, 0, 0, 0x07, 0, 0, 0 };
    CHECK_EQ(oct_container_parse(open, sizeof open, &c), OCT_PARSE_OK);
    CHECK_EQ(c.type, OCT_CONTAINER_COMMAND);
    CHECK_EQ(c.code, OCT_OP_OPEN_SESSION);
    CHECK_EQ(c.transaction_id, 0);
    CHECK_EQ(c.param_count, 1);
    CHECK_EQ(c.params[0], 7);
    CHECK_EQ(c.params[1], 0);
    CHECK_EQ(oct_container_parse(open, 11, &c), OCT_PARSE_TRUNCATED);
    CHECK_EQ(oct_container_parse(NULL, 16, &c), OCT_PARSE_TRUNCATED);
    CHECK_EQ(oct_container_parse(open, 12, &c), OCT_PARSE_LENGTH_MISMATCH);
    CHECK_EQ(c.code, OCT_OP_OPEN_SESSION); /* header still decoded */
    uint8_t response[12] = { 12, 0, 0, 0, 3, 0, 0x01, 0x20, 1, 0, 0, 0 };
    CHECK_EQ(oct_container_parse(response, sizeof response, &c), OCT_PARSE_BAD_TYPE);
    response[4] = 9;
    CHECK_EQ(oct_container_parse(response, sizeof response, &c), OCT_PARSE_BAD_TYPE);
    uint8_t six[36] = { 36, 0, 0, 0, 1, 0, 0x07, 0x10 };
    CHECK_EQ(oct_container_parse(six, sizeof six, &c), OCT_PARSE_BAD_PARAMETERS);
    uint8_t misaligned[14] = { 14, 0, 0, 0, 1, 0, 0x07, 0x10 };
    CHECK_EQ(oct_container_parse(misaligned, sizeof misaligned, &c), OCT_PARSE_BAD_PARAMETERS);
    uint8_t five[32] = { 32, 0, 0, 0, 1, 0, 0x05, 0x98, 3, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0, 4, 0, 0, 0, 5, 0, 0, 0 };
    CHECK_EQ(oct_container_parse(five, sizeof five, &c), OCT_PARSE_OK);
    CHECK_EQ(c.param_count, 5);
    CHECK_EQ(c.params[4], 5);
    CHECK_EQ(c.transaction_id, 3);
    uint8_t data[17] = { 17, 0, 0, 0, 2, 0, 0x0D, 0x10, 4, 0, 0, 0, 'h', 'e', 'l', 'l', 'o' };
    CHECK_EQ(oct_container_parse(data, sizeof data, &c), OCT_PARSE_OK);
    CHECK_EQ(c.payload_length, 5);
    CHECK(memcmp(c.payload, "hello", 5) == 0);
    data[0] = 0xFF, data[1] = 0xFF, data[2] = 0xFF, data[3] = 0xFF; /* > 4 GiB announcement */
    CHECK_EQ(oct_container_parse(data, sizeof data, &c), OCT_PARSE_OK);
    CHECK_EQ(c.payload_length, 5);
}

static void test_builders(void)
{
    oct_buf b;
    oct_buf_init(&b, 0);
    uint32_t params[2] = { 0x00010001u, 0xAABBCCDDu };
    oct_container_response(&b, OCT_RC_OK, 9, params, 2);
    const uint8_t expected[] = { 20, 0, 0, 0, 3, 0, 0x01, 0x20, 9, 0, 0, 0, 0x01, 0, 0x01, 0, 0xDD, 0xCC, 0xBB, 0xAA };
    CHECK_EQ(b.length, sizeof expected);
    CHECK(memcmp(b.data, expected, sizeof expected) == 0);
    oct_buf_clear(&b);
    size_t start = oct_container_data_begin(&b, OCT_OP_GET_OBJECT, 5);
    oct_buf_bytes(&b, "abc", 3);
    oct_container_data_end(&b, start);
    const uint8_t data[] = { 15, 0, 0, 0, 2, 0, 0x09, 0x10, 5, 0, 0, 0, 'a', 'b', 'c' };
    CHECK_EQ(b.length, sizeof data);
    CHECK(memcmp(b.data, data, sizeof data) == 0);
    oct_buf_clear(&b);
    oct_buf_u64(&b, 0x0102030405060708ull);
    const uint8_t le[] = { 8, 7, 6, 5, 4, 3, 2, 1 };
    CHECK(memcmp(b.data, le, 8) == 0);
    oct_buf_free(&b);
    oct_buf small;
    oct_buf_init(&small, 16);
    oct_buf_bytes(&small, "0123456789", 10);
    oct_buf_bytes(&small, "0123456789", 10); /* would exceed the limit */
    CHECK(small.failed);
    CHECK_EQ(small.length, 10);
    oct_buf_u8(&small, 1); /* sticky failure */
    CHECK_EQ(small.length, 10);
    oct_buf_free(&small);
}

static void check_string(const char *utf8, const uint8_t *wire, size_t wire_length)
{
    oct_buf b;
    oct_buf_init(&b, 0);
    oct_buf_string(&b, utf8);
    CHECK(!b.failed);
    CHECK_EQ(b.length, wire_length);
    CHECK(b.length == wire_length && memcmp(b.data, wire, wire_length) == 0);
    oct_reader reader;
    char decoded[1024];
    oct_reader_init(&reader, wire, wire_length);
    CHECK(oct_read_string(&reader, decoded, sizeof decoded));
    CHECK_STR(decoded, utf8);
    CHECK_EQ(oct_reader_remaining(&reader), 0);
    oct_buf_free(&b);
}

static bool decodes(const uint8_t *wire, size_t length, size_t capacity)
{
    oct_reader reader;
    char decoded[1024];
    oct_reader_init(&reader, wire, length);
    return oct_read_string(&reader, decoded, capacity);
}

static void test_strings(void)
{
    const uint8_t hi[] = { 3, 'H', 0, 'i', 0, 0, 0 };
    check_string("Hi", hi, sizeof hi);
    const uint8_t empty[] = { 0 };
    check_string("", empty, sizeof empty);
    /* U+00E9 and U+1F419 (a surrogate pair) */
    const uint8_t unicode[] = { 4, 0xE9, 0x00, 0x3D, 0xD8, 0x19, 0xDC, 0, 0 };
    check_string("\xC3\xA9\xF0\x9F\x90\x99", unicode, sizeof unicode);
    char longest[256];
    memset(longest, 'a', 254);
    longest[254] = '\0';
    oct_buf b;
    oct_buf_init(&b, 0);
    oct_buf_string(&b, longest);
    CHECK(!b.failed && b.length == 1 + 255 * 2 && b.data[0] == 255);
    oct_buf_clear(&b);
    longest[254] = 'a';
    longest[255] = '\0';
    oct_buf_string(&b, longest);
    CHECK(b.failed); /* 255 characters cannot be represented */
    oct_buf_clear(&b);
    oct_buf_string(&b, "\xC3");
    CHECK(b.failed);
    oct_buf_free(&b);
    const uint8_t unterminated[] = { 2, 'a', 0, 'b', 0 };
    CHECK(!decodes(unterminated, sizeof unterminated, 64));
    const uint8_t embedded[] = { 3, 'a', 0, 0, 0, 0, 0 };
    CHECK(!decodes(embedded, sizeof embedded, 64));
    const uint8_t lone_low[] = { 2, 0x00, 0xDC, 0, 0 };
    CHECK(!decodes(lone_low, sizeof lone_low, 64));
    const uint8_t lone_high[] = { 2, 0x3D, 0xD8, 0, 0 };
    CHECK(!decodes(lone_high, sizeof lone_high, 64));
    const uint8_t swapped[] = { 3, 0x19, 0xDC, 0x3D, 0xD8, 0, 0 };
    CHECK(!decodes(swapped, sizeof swapped, 64));
    const uint8_t truncated[] = { 5, 'a', 0 };
    CHECK(!decodes(truncated, sizeof truncated, 64));
    CHECK(!decodes(hi, sizeof hi, 2)); /* "Hi" needs 3 bytes with its NUL */
    CHECK(decodes(hi, sizeof hi, 3));
}

static void put_ascii(uint8_t **p, const char *text)
{
    size_t n = strlen(text);
    *(*p)++ = n ? (uint8_t)(n + 1) : 0;
    for (size_t i = 0; i < n; i++) {
        *(*p)++ = (uint8_t)text[i];
        *(*p)++ = 0;
    }
    if (n) {
        *(*p)++ = 0;
        *(*p)++ = 0;
    }
}

static void put_le(uint8_t **p, uint64_t value, int bytes)
{
    for (int i = 0; i < bytes; i++) *(*p)++ = (uint8_t)(value >> (8 * i));
}

static void test_datasets(void)
{
    char date[OCT_DATETIME_LENGTH + 1];
    CHECK(oct_format_datetime(SEED_TIME, date));
    CHECK_STR(date, "20261107T000000");
    CHECK(oct_format_datetime(SEED_TIME + 3723, date));
    CHECK_STR(date, "20261107T010203");

    oct_buf b;
    oct_buf_init(&b, 0);
    oct_storage_info storage = { 0x00010001u, 0x0003, 0x0002, 0x0001, 0x1000, 0x800, 6, "Oct", "" };
    oct_dataset_storage_info(&b, &storage);
    uint8_t expected[256], *p = expected;
    put_le(&p, 3, 2), put_le(&p, 2, 2), put_le(&p, 1, 2);
    put_le(&p, 0x1000, 8), put_le(&p, 0x800, 8), put_le(&p, 0xFFFFFFFFu, 4);
    put_ascii(&p, "Oct"), put_ascii(&p, "");
    CHECK_EQ(b.length, (size_t)(p - expected));
    CHECK(memcmp(b.data, expected, b.length) == 0);

    oct_buf_clear(&b);
    oct_object_info info;
    memset(&info, 0, sizeof info);
    info.handle = 4, info.parent = 2, info.storage_id = 0x00010001u, info.format = OCT_FORMAT_TEXT;
    info.write_protected = true, info.size = 26, info.created = SEED_TIME, info.modified = SEED_TIME + 60;
    strcpy(info.name, "a.txt");
    oct_dataset_object_info(&b, &info);
    p = expected;
    put_le(&p, 0x00010001u, 4), put_le(&p, 0x3004, 2), put_le(&p, 1, 2), put_le(&p, 26, 4);
    put_le(&p, 0, 2);
    for (int i = 0; i < 6; i++) put_le(&p, 0, 4);
    put_le(&p, 2, 4), put_le(&p, 0, 2), put_le(&p, 0, 4), put_le(&p, 0, 4);
    put_ascii(&p, "a.txt"), put_ascii(&p, "20261107T000000"), put_ascii(&p, "20261107T000100"), put_ascii(&p, "");
    CHECK_EQ(b.length, 132);
    CHECK_EQ(b.length, (size_t)(p - expected));
    CHECK(memcmp(b.data, expected, b.length) == 0);

    oct_object_info_request request;
    CHECK_OK(oct_dataset_parse_object_info(b.data, b.length, &request));
    CHECK_EQ(request.storage_id, 0x00010001u);
    CHECK_EQ(request.format, OCT_FORMAT_TEXT);
    CHECK_EQ(request.compressed_size, 26);
    CHECK_EQ(request.parent, 2);
    CHECK_STR(request.name, "a.txt");
    /* Every truncation before the filename terminator is rejected; after it, the
     * optional trailing strings must still be well formed. */
    for (size_t cut = 0; cut < b.length; cut++) {
        oct_status status = oct_dataset_parse_object_info(b.data, cut, &request);
        if (cut < 65) CHECK_EQ(status, OCT_E_MALFORMED);
    }
    CHECK_OK(oct_dataset_parse_object_info(b.data, 65, &request)); /* dates omitted */

    oct_buf_clear(&b);
    info.is_directory = true, info.format = OCT_FORMAT_ASSOCIATION, info.size = 77;
    oct_dataset_object_info(&b, &info);
    CHECK_OK(oct_dataset_parse_object_info(b.data, b.length, &request));
    CHECK_EQ(request.association_type, OCT_ASSOCIATION_GENERIC_FOLDER);
    CHECK_EQ(request.compressed_size, 0);

    oct_buf_clear(&b);
    info.is_directory = false, info.size = 0x100000000ull;
    oct_dataset_object_info(&b, &info);
    CHECK_OK(oct_dataset_parse_object_info(b.data, b.length, &request));
    CHECK_EQ(request.compressed_size, 0xFFFFFFFFu); /* > 4 GiB: see ObjectSize */

    uint8_t uid[16];
    info.handle = 1;
    oct_persistent_uid(&info, uid);
    const uint8_t uid_expected[16] = { 1, 0, 0, 0, 1, 0, 1, 0, 'S', 'U', 'P', 'O', 'T', 'C', 'O', '!' };
    CHECK(memcmp(uid, uid_expected, 16) == 0);

    oct_buf_clear(&b);
    oct_dataset_object_prop_desc(&b, OCT_PROP_OBJECT_FILE_NAME, true);
    const uint8_t desc[] = { 0x07, 0xDC, 0xFF, 0xFF, 0x01, 0x00, 0, 0, 0, 0, 0x00 };
    CHECK_EQ(b.length, sizeof desc);
    CHECK(memcmp(b.data, desc, sizeof desc) == 0);
    oct_buf_clear(&b);
    oct_dataset_object_prop_desc(&b, OCT_PROP_PROTECTION_STATUS, false);
    const uint8_t protection[] = { 0x03, 0xDC, 0x04, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0x02, 2, 0, 0, 0, 1, 0 };
    CHECK_EQ(b.length, sizeof protection);
    CHECK(memcmp(b.data, protection, sizeof protection) == 0);
    CHECK(oct_object_property_supported(OCT_PROP_PERSISTENT_UID));
    CHECK(!oct_object_property_supported(0xDC99));
    CHECK_EQ(oct_object_property_type(OCT_PROP_OBJECT_SIZE), OCT_TYPE_UINT64);
    oct_buf_free(&b);
}

void run_protocol_tests(void)
{
    test_container_parse();
    test_builders();
    test_strings();
    test_datasets();
}
