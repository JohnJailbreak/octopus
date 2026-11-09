/*
 * test_image.c — selecting the one loaded image that is /usr/libexec/ptpd.
 *
 * The 9 November 2026 device run rejected a ptpd whose on-disk bytes are byte-identical to the
 * reference (417184 bytes, SHA-256 a1742358...), so the image the companion inspected was not the
 * one it meant to inspect. These tests drive the real selection code with image lists the device
 * could plausibly present: the executable not first, an injected dylib where it was assumed to be,
 * the target absent, listed twice, and a range of broken headers. Every failure must be distinct,
 * and none may be mistaken for success.
 *
 * The valid header is the genuine ptpd one, rebuilt from the load-command table recorded in
 * docs/usb-research/evidence/inventory/ptpd.json: 32 commands, sizeofcmds 4816, 4848 bytes total.
 */
#include "test_support.h"

#include "../../Core/oct_device.h"

#include <stdlib.h>
#include <string.h>

#define PTPD_PATH "/usr/libexec/ptpd"
#define PTPD_UUID "FFD54FC7-0508-3AB8-8D17-2F698B611E10"

/* ---- the fixture ------------------------------------------------------------------------- */

enum { HEADER_BYTES = 4848, COMMAND_COUNT = 32, SIZEOFCMDS = 4816 };

/* name, size; offsets follow from the sizes, as recorded. */
static const struct { uint32_t cmd, size; } ptpd_commands[COMMAND_COUNT] = {
    /* Transcribed from docs/usb-research/evidence/inventory/ptpd.json, which records the real
     * table: five LC_SEGMENT_64, then chained fixups, exports trie, symtab, dysymtab, dylinker,
     * LC_UUID, LC_BUILD_VERSION, source version, LC_MAIN, one weak dylib and fourteen dylibs,
     * function starts, data-in-code and the code signature. Sizes sum to 4816. */
    {0x19,72},{0x19,1192},{0x19,1112},{0x19,792},
    {0x19,72},{0x80000034,16},{0x80000033,16},{0x2,24},
    {0xb,80},{0xe,32},{0x1b,24},{0x32,32},
    {0x2a,16},{0x80000028,24},{0x80000018,112},{0xc,112},
    {0xc,80},{0xc,88},{0xc,96},{0xc,96},
    {0xc,96},{0xc,88},{0xc,88},{0xc,96},
    {0xc,56},{0xc,56},{0xc,88},{0xc,56},
    {0xc,56},{0x26,16},{0x29,16},{0x1d,16},
};

static void put32(uint8_t *at, uint32_t value)
{
    at[0] = (uint8_t)value; at[1] = (uint8_t)(value >> 8);
    at[2] = (uint8_t)(value >> 16); at[3] = (uint8_t)(value >> 24);
}

/* file_type 2 is MH_EXECUTE; the tests also build MH_DYLIB (6) versions. */
static uint8_t *build_header(uint32_t cpu_type, uint32_t cpu_subtype, uint32_t file_type,
                             const char *uuid_hex, size_t *length)
{
    uint32_t total = 0;
    for (int i = 0; i < COMMAND_COUNT; i++) total += ptpd_commands[i].size;
    uint8_t *bytes = calloc(1, 32 + total);
    CHECK(bytes != NULL);
    if (!bytes) return NULL;
    put32(bytes, 0xfeedfacfu);
    put32(bytes + 4, cpu_type);
    put32(bytes + 8, cpu_subtype);
    put32(bytes + 12, file_type);
    put32(bytes + 16, COMMAND_COUNT);
    put32(bytes + 20, total);
    put32(bytes + 24, 0x200085u);
    size_t pos = 32;
    for (int i = 0; i < COMMAND_COUNT; i++) {
        put32(bytes + pos, ptpd_commands[i].cmd);
        put32(bytes + pos + 4, ptpd_commands[i].size);
        if (ptpd_commands[i].cmd == 0x1bu) {
            for (int b = 0; b < 16; b++) {
                char pair[3] = { uuid_hex[b * 2], uuid_hex[b * 2 + 1], 0 };
                bytes[pos + 8 + (size_t)b] = (uint8_t)strtoul(pair, NULL, 16);
            }
        } else if (ptpd_commands[i].cmd == 0x32u) {
            put32(bytes + pos + 8, 2);           /* platform iOS */
            put32(bytes + pos + 12, 17u << 16);  /* minimum OS 17.0.0 */
            put32(bytes + pos + 16, 17u << 16);  /* SDK 17.0.0 */
            put32(bytes + pos + 20, (ptpd_commands[i].size - 24) / 8);
        }
        pos += ptpd_commands[i].size;
    }
    if (length) *length = pos;
    return bytes;
}

static uint8_t *genuine_header(size_t *length)
{
    return build_header(0x100000cu, 0x80000002u, 2, "FFD54FC705083AB88D172F698B611E10", length);
}

/* ---- an image list the tests control ------------------------------------------------------ */

typedef struct { const char *name; const void *header; } fake_image;
typedef struct { const fake_image *images; uint32_t count; bool break_enumeration; } fake_list;

static uint32_t fake_count(void *context)
{
    fake_list *list = context;
    return list->break_enumeration ? 0 : list->count;
}
static const char *fake_name(void *context, uint32_t index)
{
    fake_list *list = context;
    return index < list->count ? list->images[index].name : NULL;
}
static const void *fake_header(void *context, uint32_t index)
{
    fake_list *list = context;
    return index < list->count ? list->images[index].header : NULL;
}

static oct_image_outcome select_from(const fake_image *images, uint32_t count,
                                     uint32_t *index, oct_macho_identity *identity)
{
    fake_list list = { .images = images, .count = count };
    oct_image_source source = { fake_count, fake_name, fake_header, &list };
    return oct_device_select_image(&source, PTPD_PATH, index, identity);
}

/* ---- the cases ---------------------------------------------------------------------------- */

/* The arrangement the device most likely presented: something else at index 0, ptpd later. */
static void test_target_is_found_wherever_it_sits(void)
{
    size_t length = 0;
    uint8_t *ptpd = genuine_header(&length);
    CHECK_EQ(length, (size_t)HEADER_BYTES); /* the real ptpd header size */
    if (!ptpd) return;
    size_t other_length = 0;
    uint8_t *injected = build_header(0x100000cu, 0x80000002u, 6, "00112233445566778899AABBCCDDEEFF",
                                     &other_length);

    for (uint32_t position = 0; position < 4; position++) {
        fake_image images[4] = {
            { "/usr/lib/systemhook.dylib", injected },
            { "/var/jb/usr/lib/TweakInject/OctopusNative.dylib", injected },
            { "/usr/lib/libobjc.A.dylib", injected },
            { "/usr/lib/libSystem.B.dylib", injected },
        };
        images[position] = (fake_image){ PTPD_PATH, ptpd };
        uint32_t index = 0;
        oct_macho_identity identity;
        CHECK_EQ(select_from(images, 4, &index, &identity), OCT_IMAGE_FOUND);
        CHECK_EQ(index, position);
        CHECK(strcmp(identity.uuid, PTPD_UUID) == 0);
        CHECK_EQ(identity.cpu_type, 0x100000cu);
        CHECK_EQ(identity.cpu_subtype, 0x80000002u);
        CHECK_EQ(identity.file_type, 2u);
        CHECK_EQ(identity.platform, 2u);
    }
    free(ptpd);
    free(injected);
}

/* The old code's exact failure mode: index 0 is a dylib, so its UUID was compared instead. */
static void test_an_injected_dylib_at_index_zero_is_not_mistaken_for_ptpd(void)
{
    size_t length = 0, other = 0;
    uint8_t *ptpd = genuine_header(&length);
    uint8_t *dylib = build_header(0x100000cu, 0x80000002u, 6, "DEADBEEFDEADBEEFDEADBEEFDEADBEEF", &other);
    if (!ptpd || !dylib) { free(ptpd); free(dylib); return; }
    const fake_image images[] = { { "/usr/lib/systemhook.dylib", dylib }, { PTPD_PATH, ptpd } };
    uint32_t index = 0;
    oct_macho_identity identity;
    CHECK_EQ(select_from(images, 2, &index, &identity), OCT_IMAGE_FOUND);
    CHECK_EQ(index, 1u);                                   /* not 0 */
    CHECK(strcmp(identity.uuid, PTPD_UUID) == 0);          /* ptpd's UUID, not the dylib's */
    free(ptpd);
    free(dylib);
}

static void test_each_failure_is_distinct(void)
{
    size_t length = 0;
    uint8_t *ptpd = genuine_header(&length);
    if (!ptpd) return;
    uint32_t index = 0;
    oct_macho_identity identity;

    /* Absent. */
    const fake_image absent[] = { { "/usr/lib/dyld", ptpd }, { "/usr/libexec/ptpcamerad", ptpd } };
    CHECK_EQ(select_from(absent, 2, &index, &identity), OCT_IMAGE_NOT_PRESENT);
    CHECK_EQ(index, UINT32_MAX);
    CHECK_EQ(identity.uuid[0], 0);

    /* Listed twice: reported, never resolved by picking one. */
    const fake_image twice[] = { { PTPD_PATH, ptpd }, { PTPD_PATH, ptpd } };
    CHECK_EQ(select_from(twice, 2, &index, &identity), OCT_IMAGE_AMBIGUOUS);
    CHECK_EQ(index, 0u); /* the first, so a caller can report where */

    /* Listed, but dyld handed back nothing. */
    const fake_image headerless[] = { { PTPD_PATH, NULL } };
    CHECK_EQ(select_from(headerless, 1, &index, &identity), OCT_IMAGE_NO_HEADER);

    /* Not a Mach-O at all, and a fat header, which must not be read as thin. */
    static const uint8_t rubbish[64] = { 'n', 'o', 't', 'm' };
    static const uint8_t fat[64] = { 0xca, 0xfe, 0xba, 0xbe };
    const fake_image wrong_magic[] = { { PTPD_PATH, rubbish } };
    CHECK_EQ(select_from(wrong_magic, 1, &index, &identity), OCT_IMAGE_NOT_MACHO);
    const fake_image fat_magic[] = { { PTPD_PATH, fat } };
    CHECK_EQ(select_from(fat_magic, 1, &index, &identity), OCT_IMAGE_NOT_MACHO);

    /* A header claiming more load commands than may be read is refused, not trusted. */
    uint8_t *huge = malloc(64);
    CHECK(huge != NULL);
    if (huge) {
        memcpy(huge, ptpd, 32);
        put32(huge + 20, OCT_IMAGE_MAX_HEADER_BYTES + 1);
        const fake_image oversized[] = { { PTPD_PATH, huge } };
        CHECK_EQ(select_from(oversized, 1, &index, &identity), OCT_IMAGE_HEADER_TOO_LARGE);
        free(huge);
    }

    /* Right magic, truncated commands. */
    uint8_t *broken = malloc(length);
    CHECK(broken != NULL);
    if (broken) {
        memcpy(broken, ptpd, length);
        put32(broken + 20, SIZEOFCMDS - 8); /* commands no longer fill the span */
        const fake_image malformed[] = { { PTPD_PATH, broken } };
        CHECK_EQ(select_from(malformed, 1, &index, &identity), OCT_IMAGE_MALFORMED);
        CHECK_EQ(identity.uuid[0], 0); /* nothing trustworthy was produced */
        free(broken);
    }

    /* Parses, but it is a library sitting at that path, not the program. */
    size_t dylib_length = 0;
    uint8_t *as_dylib = build_header(0x100000cu, 0x80000002u, 6, "FFD54FC705083AB88D172F698B611E10",
                                     &dylib_length);
    if (as_dylib) {
        const fake_image not_executable[] = { { PTPD_PATH, as_dylib } };
        CHECK_EQ(select_from(not_executable, 1, &index, &identity), OCT_IMAGE_NOT_EXECUTABLE);
        CHECK(strcmp(identity.uuid, PTPD_UUID) == 0); /* the UUID read is still reported */
        free(as_dylib);
    }

    /* No list at all, and a source missing its callbacks. */
    fake_list empty = { .images = NULL, .count = 0 };
    oct_image_source none = { fake_count, fake_name, fake_header, &empty };
    CHECK_EQ(oct_device_select_image(&none, PTPD_PATH, &index, &identity), OCT_IMAGE_NO_ENUMERATION);
    oct_image_source partial = { fake_count, NULL, fake_header, &empty };
    CHECK_EQ(oct_device_select_image(&partial, PTPD_PATH, &index, &identity), OCT_IMAGE_NO_ENUMERATION);
    CHECK_EQ(oct_device_select_image(NULL, PTPD_PATH, &index, &identity), OCT_IMAGE_NO_ENUMERATION);
    const fake_image one[] = { { PTPD_PATH, ptpd } };
    CHECK_EQ(select_from(one, 1, NULL, NULL), OCT_IMAGE_FOUND); /* optional outputs */
    free(ptpd);
}

/* A wrong UUID or architecture is the caller's comparison, not this function's: it reports what it
 * read so the caller can say which one differed, instead of one blanket verdict. */
static void test_the_identity_read_is_reported_for_comparison(void)
{
    size_t length = 0;
    uint8_t *other_uuid = build_header(0x100000cu, 0x80000002u, 2, "0123456789ABCDEF0123456789ABCDEF",
                                       &length);
    uint8_t *other_cpu = build_header(0x0100000Cu, 0x00000000u, 2, "FFD54FC705083AB88D172F698B611E10",
                                      &length);
    uint32_t index = 0;
    oct_macho_identity identity;
    if (other_uuid) {
        const fake_image images[] = { { PTPD_PATH, other_uuid } };
        CHECK_EQ(select_from(images, 1, &index, &identity), OCT_IMAGE_FOUND);
        CHECK(strcmp(identity.uuid, "01234567-89AB-CDEF-0123-456789ABCDEF") == 0);
        CHECK(strcmp(identity.uuid, PTPD_UUID) != 0);
        free(other_uuid);
    }
    if (other_cpu) {
        const fake_image images[] = { { PTPD_PATH, other_cpu } };
        CHECK_EQ(select_from(images, 1, &index, &identity), OCT_IMAGE_FOUND);
        CHECK_EQ(identity.cpu_subtype, 0u); /* arm64, not arm64e: the caller rejects it */
        free(other_cpu);
    }
    for (int outcome = OCT_IMAGE_FOUND; outcome <= OCT_IMAGE_NOT_EXECUTABLE; outcome++) {
        CHECK(strcmp(oct_image_outcome_name((oct_image_outcome)outcome), "unknown") != 0);
    }
}

void run_image_tests(void)
{
    test_target_is_found_wherever_it_sits();
    test_an_injected_dylib_at_index_zero_is_not_mistaken_for_ptpd();
    test_each_failure_is_distinct();
    test_the_identity_read_is_reported_for_comparison();
}
