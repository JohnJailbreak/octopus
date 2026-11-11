#include "test_support.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

oct_store *open_synthetic(bool scratch)
{
    oct_synthetic_options options = { .time = SEED_TIME, .scratch = scratch };
    oct_backend backend;
    oct_store *store = NULL;
    if (oct_synthetic_backend_create(&options, &backend) != OCT_OK) return NULL;
    if (oct_store_open(NULL, backend, &store) != OCT_OK) return NULL;
    return store;
}

static uint32_t find(oct_store *store, uint32_t parent, const char *name)
{
    uint32_t handle = 0;
    return oct_store_find(store, parent, name, &handle) == OCT_OK ? handle : 0;
}

static const char *name_of(oct_store *store, uint32_t handle)
{
    static oct_object_info info;
    return oct_store_get(store, handle, &info) == OCT_OK ? info.name : "(invalid)";
}

static void test_enumeration(void)
{
    oct_store *store = open_synthetic(false);
    CHECK(store != NULL);
    oct_handle_list list = { 0 };
    CHECK_OK(oct_store_list(store, OCT_ROOT, false, &list));
    CHECK_EQ(list.count, 3);
    CHECK_STR(name_of(store, list.handles[0]), "README.txt");
    CHECK_STR(name_of(store, list.handles[1]), "Documents");
    CHECK_STR(name_of(store, list.handles[2]), "Information");
    uint32_t documents = find(store, OCT_ROOT, "Documents");
    CHECK_OK(oct_store_list(store, documents, false, &list));
    CHECK_EQ(list.count, 2);
    CHECK_STR(name_of(store, list.handles[0]), "example.txt");
    CHECK_STR(name_of(store, list.handles[1]), "sample.bin");
    CHECK_OK(oct_store_list(store, find(store, OCT_ROOT, "Information"), false, &list));
    CHECK_EQ(list.count, 1);
    CHECK_STR(name_of(store, list.handles[0]), "version.txt");
    CHECK_OK(oct_store_list(store, OCT_ROOT, true, &list));
    CHECK_EQ(list.count, 6);
    CHECK_EQ(oct_store_object_count(store), 6);
    oct_handle_list_free(&list);
    oct_store_close(store);
}

static void test_relationships(void)
{
    oct_store *store = open_synthetic(false);
    oct_object_info info;
    uint32_t documents = find(store, OCT_ROOT, "Documents");
    uint32_t information = find(store, OCT_ROOT, "Information");
    uint32_t example = find(store, documents, "example.txt");
    uint32_t sample = find(store, documents, "sample.bin");
    uint32_t version = find(store, information, "version.txt");
    uint32_t readme = find(store, OCT_ROOT, "README.txt");
    CHECK(documents && information && example && sample && version && readme);
    CHECK_OK(oct_store_get(store, example, &info));
    CHECK_EQ(info.parent, documents);
    CHECK_EQ(info.format, OCT_FORMAT_TEXT);
    CHECK(!info.is_directory);
    CHECK_OK(oct_store_get(store, sample, &info));
    CHECK_EQ(info.parent, documents);
    CHECK_EQ(info.format, OCT_FORMAT_UNDEFINED);
    CHECK_OK(oct_store_get(store, version, &info));
    CHECK_EQ(info.parent, information);
    CHECK_OK(oct_store_get(store, documents, &info));
    CHECK_EQ(info.parent, OCT_ROOT);
    CHECK(info.is_directory);
    CHECK_EQ(info.format, OCT_FORMAT_ASSOCIATION);
    CHECK_EQ(info.size, 0);
    CHECK_EQ(info.storage_id, OCT_DEFAULT_STORAGE_ID);
    CHECK_EQ(find(store, documents, "README.txt"), 0); /* names are per directory */
    oct_handle_list list = { 0 };
    CHECK_EQ(oct_store_list(store, example, false, &list), OCT_E_NOT_DIRECTORY);
    CHECK_EQ(oct_store_list(store, 0x7777u, false, &list), OCT_E_INVALID_HANDLE);
    CHECK_EQ(oct_store_get(store, 0, &info), OCT_E_INVALID_HANDLE);
    CHECK_EQ(oct_store_get(store, 0xFFFFFFFFu, &info), OCT_E_INVALID_HANDLE);
    uint32_t missing = 0;
    CHECK_EQ(oct_store_find(store, documents, "missing", &missing), OCT_E_INVALID_HANDLE);
    CHECK_EQ(oct_store_find(store, readme, "x", &missing), OCT_E_NOT_DIRECTORY);
    oct_handle_list_free(&list);
    oct_store_close(store);
}

static void test_stable_handles(void)
{
    oct_store *store = open_synthetic(true);
    oct_handle_list first = { 0 }, second = { 0 };
    CHECK_OK(oct_store_list(store, OCT_ROOT, true, &first));
    CHECK_EQ(first.count, 7);
    CHECK_OK(oct_store_list(store, OCT_ROOT, true, &second));
    CHECK_EQ(second.count, first.count);
    CHECK(memcmp(first.handles, second.handles, first.count * sizeof(uint32_t)) == 0);
    uint32_t highest = 0;
    for (uint32_t i = 0; i < first.count; i++) highest = first.handles[i] > highest ? first.handles[i] : highest;
    uint32_t scratch = find(store, OCT_ROOT, "Scratch"), created = 0, again = 0;
    CHECK_OK(oct_store_create(store, scratch, "temp.txt", false, &created));
    CHECK(created > highest);
    uint32_t deleted = 0;
    CHECK_OK(oct_store_delete(store, created, &deleted));
    CHECK_EQ(deleted, 1);
    CHECK_OK(oct_store_create(store, scratch, "temp.txt", false, &again));
    CHECK(again != created && again > created); /* handles are never reused */
    CHECK_OK(oct_store_delete(store, again, &deleted));
    CHECK_OK(oct_store_list(store, OCT_ROOT, true, &second));
    CHECK_EQ(second.count, first.count);
    CHECK(memcmp(first.handles, second.handles, first.count * sizeof(uint32_t)) == 0);
    oct_handle_list_free(&first);
    oct_handle_list_free(&second);
    oct_store_close(store);
}

static void test_metadata(void)
{
    oct_store *store = open_synthetic(false);
    oct_object_info info;
    oct_storage_info storage;
    uint32_t readme = find(store, OCT_ROOT, "README.txt");
    CHECK_OK(oct_store_get(store, readme, &info));
    CHECK(info.size > 100);
    CHECK_EQ(info.created, SEED_TIME);
    CHECK_EQ(info.modified, SEED_TIME);
    CHECK(info.write_protected);
    char text[32] = { 0 };
    size_t read = 0;
    CHECK_OK(oct_store_read(store, readme, 0, text, 25, &read));
    CHECK_EQ(read, 25);
    CHECK_STR(text, "Octopus synthetic storage");
    uint32_t sample = find(store, find(store, OCT_ROOT, "Documents"), "sample.bin");
    CHECK_OK(oct_store_get(store, sample, &info));
    CHECK_EQ(info.size, OCT_SAMPLE_BIN_SIZE);
    CHECK_OK(oct_store_index_all(store));
    CHECK_OK(oct_store_storage_info(store, &storage));
    CHECK_EQ(storage.storage_id, 0x00010001u);
    CHECK_EQ(storage.storage_type, 0x0003);
    CHECK_EQ(storage.filesystem_type, 0x0002);
    CHECK_EQ(storage.access_capability, 0x0001); /* no writable area */
    CHECK_EQ(storage.max_capacity, 16u * 1024u * 1024u);
    uint64_t used = 0;
    oct_handle_list list = { 0 };
    CHECK_OK(oct_store_list(store, OCT_ROOT, true, &list));
    for (uint32_t i = 0; i < list.count; i++) {
        CHECK_OK(oct_store_get(store, list.handles[i], &info));
        used += info.size;
    }
    CHECK_EQ(storage.free_space, storage.max_capacity - used);
    CHECK_EQ(storage.object_count, 6);
    CHECK_STR(storage.description, "Octopus");
    CHECK_STR(oct_store_backend_kind(store), "memory");
    oct_handle_list_free(&list);
    oct_store_close(store);
    store = open_synthetic(true);
    CHECK_OK(oct_store_storage_info(store, &storage));
    CHECK_EQ(storage.access_capability, 0x0000);
    oct_store_close(store);
}

static void test_reads(void)
{
    oct_store *store = open_synthetic(false);
    uint32_t documents = find(store, OCT_ROOT, "Documents");
    uint32_t sample = find(store, documents, "sample.bin");
    uint8_t buffer[OCT_SAMPLE_BIN_SIZE + 64];
    size_t read = 0;
    CHECK_OK(oct_store_read(store, sample, 0, buffer, sizeof buffer, &read));
    CHECK_EQ(read, OCT_SAMPLE_BIN_SIZE); /* clamped to the object size */
    bool pattern = true;
    for (size_t i = 0; i < read; i++) pattern = pattern && buffer[i] == oct_sample_byte(i);
    CHECK(pattern);
    CHECK_OK(oct_store_read(store, sample, 1000, buffer, 100, &read));
    CHECK_EQ(read, 100);
    CHECK(buffer[0] == oct_sample_byte(1000) && buffer[99] == oct_sample_byte(1099));
    CHECK_OK(oct_store_read(store, sample, OCT_SAMPLE_BIN_SIZE, buffer, 10, &read));
    CHECK_EQ(read, 0);
    CHECK_EQ(oct_store_read(store, sample, OCT_SAMPLE_BIN_SIZE + 1, buffer, 10, &read), OCT_E_OUT_OF_RANGE);
    CHECK_EQ(oct_store_read(store, documents, 0, buffer, 10, &read), OCT_E_IS_DIRECTORY);
    CHECK_EQ(oct_store_read(store, 0x5555u, 0, buffer, 10, &read), OCT_E_INVALID_HANDLE);
    CHECK_EQ(oct_store_read(store, sample, 0, NULL, 10, &read), OCT_E_INVALID_ARGUMENT);
    oct_store_close(store);
}

static void check_consistency(oct_store *store)
{
    oct_handle_list all = { 0 }, children = { 0 };
    CHECK_OK(oct_store_list(store, OCT_ROOT, true, &all));
    CHECK_EQ(all.count, oct_store_object_count(store));
    for (uint32_t i = 0; i < all.count; i++) {
        oct_object_info info;
        CHECK_OK(oct_store_get(store, all.handles[i], &info));
        CHECK_OK(oct_store_list(store, info.parent, false, &children));
        bool listed = false;
        for (uint32_t k = 0; k < children.count; k++) listed = listed || children.handles[k] == info.handle;
        CHECK(listed);
    }
    oct_handle_list_free(&all);
    oct_handle_list_free(&children);
}

static void test_writes_in_test_directory(void)
{
    oct_store *store = open_synthetic(true);
    uint32_t scratch = find(store, OCT_ROOT, "Scratch");
    uint32_t documents = find(store, OCT_ROOT, "Documents");
    uint32_t readme = find(store, OCT_ROOT, "README.txt");
    uint32_t sample = find(store, documents, "sample.bin");
    uint32_t handle = 0, other = 0, folder = 0, inner = 0, deleted = 0;
    CHECK_EQ(oct_store_create(store, OCT_ROOT, "x.txt", false, &handle), OCT_E_WRITE_PROTECTED);
    CHECK_EQ(oct_store_create(store, documents, "x.txt", false, &handle), OCT_E_WRITE_PROTECTED);
    CHECK_EQ(oct_store_create(store, readme, "x.txt", false, &handle), OCT_E_NOT_DIRECTORY);
    CHECK_EQ(oct_store_create(store, 0x4444u, "x.txt", false, &handle), OCT_E_INVALID_PARENT);
    CHECK_OK(oct_store_create(store, scratch, "notes.txt", false, &handle));
    CHECK_OK(oct_store_write(store, handle, 0, "hello", 5));
    oct_object_info info;
    CHECK_OK(oct_store_get(store, handle, &info));
    CHECK_EQ(info.size, 5);
    CHECK(!info.write_protected);
    CHECK_EQ(info.format, OCT_FORMAT_TEXT);
    CHECK_OK(oct_store_write(store, handle, 10, "abc", 3));
    char buffer[16] = { 0 };
    size_t read = 0;
    CHECK_OK(oct_store_read(store, handle, 0, buffer, sizeof buffer, &read));
    CHECK_EQ(read, 13);
    CHECK(memcmp(buffer, "hello\0\0\0\0\0abc", 13) == 0);
    CHECK_OK(oct_store_truncate(store, handle, 3));
    CHECK_OK(oct_store_get(store, handle, &info));
    CHECK_EQ(info.size, 3);
    CHECK_EQ(oct_store_create(store, scratch, "notes.txt", false, &other), OCT_E_EXISTS);
    CHECK_EQ(oct_store_create(store, scratch, "bad/name", false, &other), OCT_E_INVALID_NAME);
    CHECK_EQ(oct_store_create(store, scratch, "", false, &other), OCT_E_INVALID_NAME);
    CHECK_EQ(oct_store_create(store, scratch, "..", false, &other), OCT_E_INVALID_NAME);
    CHECK_EQ(oct_store_write(store, sample, 0, "x", 1), OCT_E_WRITE_PROTECTED);
    CHECK_EQ(oct_store_write(store, scratch, 0, "x", 1), OCT_E_IS_DIRECTORY);
    CHECK_OK(oct_store_create(store, scratch, "folder", true, &folder));
    CHECK_OK(oct_store_create(store, folder, "inner.bin", false, &inner));
    CHECK_OK(oct_store_write(store, inner, 0, "1234", 4));
    CHECK_OK(oct_store_rename(store, handle, "renamed.txt"));
    CHECK_EQ(find(store, scratch, "renamed.txt"), handle);
    CHECK_EQ(find(store, scratch, "notes.txt"), 0);
    CHECK_EQ(oct_store_rename(store, handle, "folder"), OCT_E_EXISTS);
    CHECK_EQ(oct_store_rename(store, handle, "a/b"), OCT_E_INVALID_NAME);
    CHECK_OK(oct_store_rename(store, handle, "renamed.txt")); /* same name is a no-op */
    CHECK_EQ(oct_store_rename(store, readme, "x.txt"), OCT_E_WRITE_PROTECTED);
    CHECK_EQ(oct_store_delete(store, readme, &deleted), OCT_E_WRITE_PROTECTED);
    CHECK_EQ(oct_store_delete(store, documents, &deleted), OCT_E_WRITE_PROTECTED);
    check_consistency(store);
    CHECK_OK(oct_store_delete(store, folder, &deleted));
    CHECK_EQ(deleted, 2);
    CHECK_EQ(oct_store_get(store, inner, &info), OCT_E_INVALID_HANDLE);
    CHECK_EQ(oct_store_get(store, folder, &info), OCT_E_INVALID_HANDLE);
    CHECK_EQ(oct_store_delete(store, folder, &deleted), OCT_E_INVALID_HANDLE);
    CHECK_OK(oct_store_delete(store, handle, &deleted));
    oct_handle_list list = { 0 };
    CHECK_OK(oct_store_list(store, scratch, false, &list));
    CHECK_EQ(list.count, 0);
    check_consistency(store);
    oct_handle_list_free(&list);
    oct_store_close(store);
}

static void test_partial_deletion_and_capacity(void)
{
    oct_backend backend;
    CHECK_OK(oct_memory_backend_create(10, &backend));
    uint64_t area = 0;
    CHECK_OK(oct_memory_backend_add(&backend, 0, "area", true, NULL, 0, true, SEED_TIME, &area));
    CHECK_OK(oct_memory_backend_add(&backend, area, "keep.txt", false, "k", 1, false, SEED_TIME, NULL));
    CHECK_OK(oct_memory_backend_add(&backend, area, "drop.txt", false, "d", 1, true, SEED_TIME, NULL));
    CHECK_EQ(oct_memory_backend_add(&backend, area, "drop.txt", false, "d", 1, true, SEED_TIME, NULL), OCT_E_EXISTS);
    CHECK_EQ(oct_memory_backend_add(&backend, area, "huge.bin", false, "0123456789", 10, true, SEED_TIME, NULL),
             OCT_E_NO_SPACE);
    oct_store *store = NULL;
    oct_store_options options = { .storage_id = 0x00020001u, .first_handle = 100,
                                  .description = "Test store", .volume_label = "Label" };
    CHECK_OK(oct_store_open(&options, backend, &store));
    uint32_t handle = find(store, OCT_ROOT, "area"), file = 0, deleted = 0;
    CHECK_EQ(handle, 100); /* first_handle honoured */
    CHECK_EQ(oct_store_storage_id(store), 0x00020001u);
    CHECK_OK(oct_store_create(store, handle, "big.bin", false, &file));
    CHECK_OK(oct_store_write(store, file, 0, "12345678", 8));
    CHECK_EQ(oct_store_write(store, file, 8, "9", 1), OCT_E_NO_SPACE); /* 2 + 8 bytes used of 10 */
    CHECK_EQ(oct_store_delete(store, handle, &deleted), OCT_E_PARTIAL_DELETION);
    CHECK_EQ(deleted, 2); /* drop.txt and big.bin */
    oct_handle_list list = { 0 };
    CHECK_OK(oct_store_list(store, handle, false, &list));
    CHECK_EQ(list.count, 1);
    CHECK_STR(name_of(store, list.handles[0]), "keep.txt");
    oct_storage_info storage;
    CHECK_OK(oct_store_storage_info(store, &storage));
    CHECK_STR(storage.description, "Test store");
    CHECK_STR(storage.volume_label, "Label");
    CHECK_EQ(storage.free_space, 9);
    oct_handle_list_free(&list);
    oct_store_close(store);
    oct_backend invalid = { NULL, NULL };
    CHECK_EQ(oct_store_open(NULL, invalid, &store), OCT_E_INVALID_ARGUMENT);
}

static void test_names(void)
{
    char long_name[300];
    memset(long_name, 'a', sizeof long_name);
    long_name[254] = '\0';
    CHECK_OK(oct_name_validate("a"));
    CHECK_OK(oct_name_validate("caf\xC3\xA9 notes.txt"));
    CHECK_OK(oct_name_validate(long_name)); /* 254 UTF-16 units: the PTP maximum */
    long_name[254] = 'a';
    long_name[255] = '\0';
    CHECK_EQ(oct_name_validate(long_name), OCT_E_INVALID_NAME); /* 255 units */
    char bytes[300];
    memset(bytes, 0, sizeof bytes);
    for (int i = 0; i < 127; i++) memcpy(bytes + 2 * i, "\xC3\xA9", 2);
    bytes[254] = 'a'; /* 255 bytes, 128 units: within both limits */
    CHECK_OK(oct_name_validate(bytes));
    bytes[255] = 'a'; /* 256 bytes exceeds the POSIX component limit */
    CHECK_EQ(oct_name_validate(bytes), OCT_E_INVALID_NAME);
    CHECK_EQ(oct_name_validate(""), OCT_E_INVALID_NAME);
    CHECK_EQ(oct_name_validate("."), OCT_E_INVALID_NAME);
    CHECK_EQ(oct_name_validate(".."), OCT_E_INVALID_NAME);
    CHECK_EQ(oct_name_validate("a/b"), OCT_E_INVALID_NAME);
    CHECK_EQ(oct_name_validate("tab\there"), OCT_E_INVALID_NAME);
    CHECK_EQ(oct_name_validate("\xC0\xAF"), OCT_E_INVALID_NAME);     /* overlong '/' */
    CHECK_EQ(oct_name_validate("\xED\xA0\x80"), OCT_E_INVALID_NAME); /* encoded surrogate */
    CHECK_EQ(oct_name_validate("\xE2\x82"), OCT_E_INVALID_NAME);     /* truncated sequence */
    CHECK_EQ(oct_name_validate(NULL), OCT_E_INVALID_NAME);
    char emoji[300] = "";
    for (int i = 0; i < 63; i++) strcat(emoji, "\xF0\x9F\x90\x99"); /* 252 bytes, 126 units */
    CHECK_OK(oct_name_validate(emoji));
    CHECK_EQ(oct_utf8_utf16_length("A\xF0\x9F\x90\x99", 5), 3);
    CHECK_EQ(oct_utf8_utf16_length("\xFF", 1), -1);
    char units[300];
    memset(units, 0, sizeof units);
    for (int i = 0; i < 127; i++) memcpy(units + 2 * i, "\xC3\xA9", 2); /* 254 bytes, 127 units */
    CHECK_OK(oct_name_validate(units));
    CHECK_EQ(oct_format_for_name("a.TXT", false), OCT_FORMAT_TEXT);
    CHECK_EQ(oct_format_for_name("a.text", false), OCT_FORMAT_TEXT);
    CHECK_EQ(oct_format_for_name("a.bin", false), OCT_FORMAT_UNDEFINED);
    CHECK_EQ(oct_format_for_name(".txt", false), OCT_FORMAT_UNDEFINED);
    CHECK_EQ(oct_format_for_name("folder.txt", true), OCT_FORMAT_ASSOCIATION);
    char copy[6];
    oct_copy_utf8(copy, sizeof copy, "ab\xC3\xA9\xC3\xA9");
    CHECK_STR(copy, "ab\xC3\xA9"); /* never splits a sequence */
}

static void write_file(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    if (!file) return;
    fwrite(text, 1, strlen(text), file);
    fclose(file);
}

/*
 * The enumeration budget. PTP asks for the whole hierarchy when the host passes association 0,
 * which over a real filesystem walks everything below the root. These checks prove a store that
 * exceeds its budget says so, bounded, instead of growing until the allocator fails, and that
 * per-directory listing keeps working afterwards so a host can still browse.
 */
/*
 * The contract the native data source relies on: Apple's writer asks for a download one chunk at
 * a time through -read:length:offset:, which becomes oct_store_read at an offset. These checks
 * cover a file past the responder's buffered-transfer ceiling, so a download neither holds the
 * whole file in memory nor is capped by OCT_PTP_MAX_TRANSFER, plus the boundaries the writer
 * depends on: the end of the file reads nothing and succeeds, past it is an error rather than
 * silent zeros, and an oversized request is clamped to what remains.
 */
static void test_chunked_streaming_reads(void)
{
    char root[] = "/tmp/octopus-stream-XXXXXX", path[512];
    CHECK(mkdtemp(root) != NULL);
    snprintf(path, sizeof path, "%s/%s", root, OCT_DISPOSABLE_MARKER);
    write_file(path, "disposable\n");
    /* Larger than OCT_PTP_MAX_TRANSFER so the buffered path could not serve it. */
    const uint64_t size = (uint64_t)OCT_PTP_MAX_TRANSFER + 1024u;
    snprintf(path, sizeof path, "%s/big.bin", root);
    FILE *file = fopen(path, "wb");
    CHECK(file != NULL);
    if (file) {
        /* One check for the whole write, not one per byte: a check per byte would add tens of
         * millions of assertions to the suite and tell us nothing more. */
        bool written = true;
        for (uint64_t i = 0; i < size; i++) {
            if (fputc((int)((i * 31u + (i >> 13)) & 0xFF), file) == EOF) { written = false; break; }
        }
        CHECK(written);
        CHECK_EQ(fclose(file), 0);
    }
    snprintf(path, sizeof path, "%s/folder", root);
    CHECK_EQ(mkdir(path, 0755), 0);

    oct_backend backend;
    oct_store *store = NULL;
    CHECK_OK(oct_directory_backend_create(root, &backend));
    CHECK_OK(oct_store_open(NULL, backend, &store));
    uint32_t handle = find(store, OCT_ROOT, "big.bin");
    CHECK(handle != 0);
    oct_object_info info;
    CHECK_OK(oct_store_get(store, handle, &info));
    CHECK_EQ(info.size, size);
    CHECK(!info.is_directory);

    const size_t chunk = 64u * 1024u;
    uint8_t *buffer = malloc(chunk);
    CHECK(buffer != NULL);
    uint64_t offset = 0;
    uint32_t wrong = 0;
    while (buffer && offset < size) {
        size_t got = 0;
        CHECK_OK(oct_store_read(store, handle, offset, buffer, chunk, &got));
        CHECK(got > 0);
        if (!got) break;
        for (size_t i = 0; i < got; i++) {
            uint64_t index = offset + i;
            if (buffer[i] != (uint8_t)((index * 31u + (index >> 13)) & 0xFF)) wrong++;
        }
        offset += got;
    }
    CHECK_EQ(wrong, 0u);
    CHECK_EQ(offset, size);

    if (buffer) {
        /* A request past the end of the file is clamped to what remains, never over-read. */
        size_t got = 0;
        CHECK_OK(oct_store_read(store, handle, size - 10, buffer, chunk, &got));
        CHECK_EQ(got, 10u);
        /* Exactly at the end: no bytes, and no error, so the writer finishes cleanly. */
        got = 1;
        CHECK_OK(oct_store_read(store, handle, size, buffer, chunk, &got));
        CHECK_EQ(got, 0u);
        /* Past the end is refused. */
        CHECK_EQ(oct_store_read(store, handle, size + 1, buffer, chunk, &got), OCT_E_OUT_OF_RANGE);
        /* A directory has no bytes to stream, and an unknown handle is refused. */
        uint32_t folder = find(store, OCT_ROOT, "folder");
        CHECK(folder != 0);
        CHECK_EQ(oct_store_read(store, folder, 0, buffer, chunk, &got), OCT_E_IS_DIRECTORY);
        CHECK_EQ(oct_store_read(store, 0, 0, buffer, chunk, &got), OCT_E_INVALID_HANDLE);
    }
    free(buffer);
    oct_store_close(store);
}

/* The probe objects: absent unless asked for, so the seed tree keeps its six, and when asked for,
 * one per extension in Documents, every one carrying identical bytes. Identical content is the
 * whole point: a host that lists some and not others is then deciding on the extension alone. */
static void test_probe_objects(void)
{
    oct_backend backend;
    oct_store *store = NULL;
    oct_synthetic_options plain = { .time = 1 };
    CHECK_OK(oct_synthetic_backend_create(&plain, &backend));
    CHECK_OK(oct_store_open(NULL, backend, &store));
    CHECK_OK(oct_store_index_all(store));
    CHECK_EQ(oct_store_object_count(store), OCT_SYNTHETIC_SEED_OBJECTS);
    uint32_t documents = find(store, OCT_ROOT, "Documents");
    CHECK(documents != 0);
    CHECK_EQ(find(store, documents, "probe.dat"), 0u);
    CHECK_EQ(find(store, documents, "probe.txt"), 0u);
    oct_store_close(store);

    oct_synthetic_options probed = { .time = 1, .probes = true };
    store = NULL;
    CHECK_OK(oct_synthetic_backend_create(&probed, &backend));
    CHECK_OK(oct_store_open(NULL, backend, &store));
    CHECK_OK(oct_store_index_all(store));
    CHECK_EQ(oct_store_object_count(store), OCT_SYNTHETIC_SEED_OBJECTS + OCT_SYNTHETIC_PROBE_OBJECTS);
    documents = find(store, OCT_ROOT, "Documents");
    CHECK(documents != 0);

    static const char *const names[] = {
        "probe.dat", "probe.txt", "probe.jpg", "probe.png", "probe.pdf",
        "probe.json", "probe.log", "probe.mov", "probe.plist", "probe",
    };
    const size_t count = sizeof names / sizeof names[0];
    CHECK_EQ(count, (size_t)OCT_SYNTHETIC_PROBE_OBJECTS); /* the constant matches the list */

    /* Every probe exists, and every one is byte-identical in size to example.txt, so nothing but
     * the name distinguishes them. A difference here would make the experiment meaningless. */
    uint32_t example = find(store, documents, "example.txt");
    oct_object_info reference;
    CHECK(example != 0);
    if (example) CHECK_OK(oct_store_get(store, example, &reference));
    for (size_t i = 0; i < count; i++) {
        uint32_t handle = find(store, documents, names[i]);
        CHECK(handle != 0);
        if (!handle || !example) continue;
        oct_object_info info;
        CHECK_OK(oct_store_get(store, handle, &info));
        CHECK_EQ(info.size, reference.size);
        CHECK(!info.is_directory);
        CHECK_EQ(info.parent, documents);
        /* The bytes match too, not merely the length. */
        char mine[128], theirs[128];
        size_t got = 0, want = 0;
        CHECK_OK(oct_store_read(store, handle, 0, mine, sizeof mine, &got));
        CHECK_OK(oct_store_read(store, example, 0, theirs, sizeof theirs, &want));
        CHECK_EQ(got, want);
        if (got == want) CHECK(memcmp(mine, theirs, got) == 0);
    }
    /* The formats differ exactly as the extensions imply, which is what the host is reacting to. */
    uint32_t text_probe = find(store, documents, "probe.txt");
    uint32_t data_probe = find(store, documents, "probe.dat");
    if (text_probe && data_probe) {
        oct_object_info text_info, data_info;
        CHECK_OK(oct_store_get(store, text_probe, &text_info));
        CHECK_OK(oct_store_get(store, data_probe, &data_info));
        CHECK_EQ(text_info.format, OCT_FORMAT_TEXT);
        CHECK_EQ(data_info.format, OCT_FORMAT_UNDEFINED);
    }
    oct_store_close(store);
}

static void test_enumeration_budget(void)
{
    char root[] = "/tmp/octopus-budget-XXXXXX", path[512];
    CHECK(mkdtemp(root) != NULL);
    snprintf(path, sizeof path, "%s/%s", root, OCT_DISPOSABLE_MARKER);
    write_file(path, "disposable\n");
    /* A tree that is small but deeper and wider than the budget set below: 4 directories of 5
     * files each, so a recursive walk sees 24 objects and a single directory sees at most 5. */
    for (int d = 0; d < 4; d++) {
        snprintf(path, sizeof path, "%s/dir%d", root, d);
        CHECK_EQ(mkdir(path, 0755), 0);
        for (int f = 0; f < 5; f++) {
            snprintf(path, sizeof path, "%s/dir%d/file%d.txt", root, d, f);
            write_file(path, "x");
        }
    }

    oct_backend backend;
    oct_store *store = NULL;
    /* 12 fits the four directories and one of them expanded (4 + 5 = 9), but not the whole
     * hierarchy (24) and not a second directory (14). */
    oct_store_options options = { .max_objects = 12 };
    CHECK_OK(oct_directory_backend_create(root, &backend));
    CHECK_OK(oct_store_open(&options, backend, &store));
    CHECK_EQ(oct_store_max_objects(store), 12u);

    /* The whole hierarchy exceeds the budget: refused, with nothing returned. */
    oct_handle_list list = { 0 };
    CHECK_EQ(oct_store_list(store, OCT_ROOT, true, &list), OCT_E_TOO_LARGE);
    CHECK_EQ(list.count, 0u);
    oct_handle_list_free(&list);

    /* One directory at a time still works, which is how a host browses after being told that
     * specification by depth is unsupported. */
    CHECK_OK(oct_store_list(store, OCT_ROOT, false, &list));
    CHECK_EQ(list.count, 4u);
    uint32_t first = list.handles[0];
    oct_handle_list_free(&list);
    CHECK_OK(oct_store_list(store, first, false, &list));
    CHECK_EQ(list.count, 5u);
    oct_handle_list_free(&list);

    /* Browsing more directories eventually reaches the ceiling on tracked objects. The store
     * reports it rather than growing without limit for the length of a session. */
    oct_handle_list top = { 0 };
    CHECK_OK(oct_store_list(store, OCT_ROOT, false, &top));
    oct_status status = OCT_OK;
    for (uint32_t i = 1; i < top.count && status == OCT_OK; i++) {
        status = oct_store_list(store, top.handles[i], false, &list);
        oct_handle_list_free(&list);
    }
    oct_handle_list_free(&top);
    CHECK_EQ(status, OCT_E_TOO_LARGE);
    CHECK(oct_store_object_count(store) <= oct_store_max_objects(store));
    oct_store_close(store);

    /* The default budget enumerates this tree whole. */
    CHECK_OK(oct_directory_backend_create(root, &backend));
    CHECK_OK(oct_store_open(NULL, backend, &store));
    CHECK_EQ(oct_store_max_objects(store), OCT_STORE_DEFAULT_MAX_OBJECTS);
    CHECK_OK(oct_store_list(store, OCT_ROOT, true, &list));
    CHECK_EQ(list.count, 24u);
    oct_handle_list_free(&list);
    oct_store_close(store);
}

static void test_directory_backend(void)
{
    char root[] = "/tmp/octopus-test-XXXXXX", path[512];
    CHECK(mkdtemp(root) != NULL);
    oct_backend backend;
    CHECK_EQ(oct_directory_backend_create(root, &backend), OCT_E_INVALID_ARGUMENT); /* not designated */
    snprintf(path, sizeof path, "%s/%s", root, OCT_DISPOSABLE_MARKER);
    write_file(path, "disposable\n");
    snprintf(path, sizeof path, "%s/docs", root);
    CHECK_EQ(mkdir(path, 0755), 0);
    snprintf(path, sizeof path, "%s/docs/a.txt", root);
    write_file(path, "alpha contents");
    snprintf(path, sizeof path, "%s/link", root);
    CHECK_EQ(symlink("/etc", path), 0);
    snprintf(path, sizeof path, "%s/pipe", root);
    CHECK_EQ(mkfifo(path, 0600), 0);
    char alias[600];
    snprintf(alias, sizeof alias, "%s-alias", root);
    CHECK_EQ(symlink(root, alias), 0);
    CHECK_EQ(oct_directory_backend_create(alias, &backend), OCT_E_INVALID_ARGUMENT); /* symlinked root */
    unlink(alias);

    oct_store *store = NULL;
    CHECK_OK(oct_directory_backend_create(root, &backend));
    CHECK_OK(oct_store_open(NULL, backend, &store));
    CHECK_STR(oct_store_backend_kind(store), "directory");
    oct_handle_list list = { 0 };
    CHECK_OK(oct_store_list(store, OCT_ROOT, false, &list));
    CHECK_EQ(list.count, 1); /* marker hidden, symlink and FIFO not exported */
    CHECK_STR(name_of(store, list.handles[0]), "docs");
    uint32_t docs = find(store, OCT_ROOT, "docs"), a = find(store, docs, "a.txt");
    CHECK(a != 0);
    char buffer[64] = { 0 };
    size_t read = 0;
    CHECK_OK(oct_store_read(store, a, 6, buffer, sizeof buffer, &read));
    CHECK_EQ(read, 8);
    CHECK_STR(buffer, "contents");
    uint32_t created = 0, sub = 0, nested = 0, deleted = 0;
    CHECK_OK(oct_store_create(store, OCT_ROOT, "new.txt", false, &created));
    CHECK_OK(oct_store_write(store, created, 0, "written", 7));
    struct stat st;
    snprintf(path, sizeof path, "%s/new.txt", root);
    CHECK(stat(path, &st) == 0 && st.st_size == 7);
    CHECK_EQ(oct_store_create(store, OCT_ROOT, OCT_DISPOSABLE_MARKER, false, &sub), OCT_E_INVALID_NAME);
    CHECK_EQ(oct_store_rename(store, created, OCT_DISPOSABLE_MARKER), OCT_E_INVALID_NAME);
    CHECK_EQ(oct_store_rename(store, created, "docs"), OCT_E_EXISTS);
    CHECK_OK(oct_store_rename(store, docs, "documents"));
    memset(buffer, 0, sizeof buffer);
    CHECK_OK(oct_store_read(store, a, 0, buffer, 5, &read)); /* child path follows the rename */
    CHECK_STR(buffer, "alpha");
    snprintf(path, sizeof path, "%s/documents/a.txt", root);
    CHECK(stat(path, &st) == 0);
    CHECK_OK(oct_store_create(store, docs, "sub", true, &sub));
    CHECK_OK(oct_store_create(store, sub, "b.txt", false, &nested));
    CHECK_OK(oct_store_write(store, nested, 0, "beta", 4));
    CHECK_OK(oct_store_truncate(store, nested, 2));
    CHECK_OK(oct_store_delete(store, docs, &deleted));
    CHECK_EQ(deleted, 4); /* documents, a.txt, sub, b.txt */
    snprintf(path, sizeof path, "%s/documents", root);
    CHECK(stat(path, &st) != 0);
    /* A directory replaced by a symlink after indexing must not be followed. */
    uint32_t safe = 0;
    CHECK_OK(oct_store_create(store, OCT_ROOT, "safe", true, &safe));
    snprintf(path, sizeof path, "%s/safe", root);
    CHECK_EQ(rmdir(path), 0);
    CHECK_EQ(symlink("/etc", path), 0);
    oct_handle_list escaped = { 0 };
    oct_store_list(store, safe, false, &escaped);
    CHECK_EQ(escaped.count, 0);
    uint32_t outside = 0;
    CHECK(oct_store_create(store, safe, "planted.txt", false, &outside) != OCT_OK);
    oct_storage_info storage;
    CHECK_OK(oct_store_storage_info(store, &storage));
    CHECK(storage.max_capacity > 0);
    CHECK_EQ(storage.access_capability, 0x0000);
    oct_handle_list_free(&list);
    oct_handle_list_free(&escaped);
    oct_store_close(store);
    char command[600];
    snprintf(command, sizeof command, "rm -rf '%s'", root);
    CHECK_EQ(system(command), 0);
}

void run_store_tests(void)
{
    test_enumeration();
    test_relationships();
    test_stable_handles();
    test_metadata();
    test_reads();
    test_writes_in_test_directory();
    test_partial_deletion_and_capacity();
    test_names();
    test_directory_backend();
    test_enumeration_budget();
    test_probe_objects();
    test_chunked_streaming_reads();
}
