#if !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "oct_ptp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- Output buffer ------------------------------------------------------------ */

void oct_buf_init(oct_buf *buffer, size_t limit)
{
    memset(buffer, 0, sizeof *buffer);
    buffer->limit = limit ? limit : OCT_BUF_DEFAULT_LIMIT;
}

void oct_buf_free(oct_buf *buffer)
{
    size_t limit = buffer->limit;
    free(buffer->data);
    memset(buffer, 0, sizeof *buffer);
    buffer->limit = limit;
}

void oct_buf_clear(oct_buf *buffer)
{
    buffer->length = 0;
    buffer->failed = false;
}

bool oct_buf_reserve(oct_buf *buffer, size_t extra)
{
    if (buffer->failed) return false;
    if (extra > buffer->limit || buffer->length > buffer->limit - extra) {
        buffer->failed = true;
        return false;
    }
    size_t needed = buffer->length + extra;
    if (needed <= buffer->capacity) return true;
    size_t capacity = buffer->capacity ? buffer->capacity : 256;
    while (capacity < needed) capacity = capacity > buffer->limit / 2 ? buffer->limit : capacity * 2;
    uint8_t *grown = realloc(buffer->data, capacity);
    if (!grown) {
        buffer->failed = true;
        return false;
    }
    buffer->data = grown;
    buffer->capacity = capacity;
    return true;
}

void oct_buf_bytes(oct_buf *buffer, const void *data, size_t length)
{
    if (length == 0 || !oct_buf_reserve(buffer, length)) return;
    memcpy(buffer->data + buffer->length, data, length);
    buffer->length += length;
}

void oct_buf_u8(oct_buf *buffer, uint8_t value) { oct_buf_bytes(buffer, &value, 1); }

void oct_buf_u16(oct_buf *buffer, uint16_t value)
{
    uint8_t bytes[2] = { (uint8_t)(value & 0xFFu), (uint8_t)(value >> 8) };
    oct_buf_bytes(buffer, bytes, sizeof bytes);
}

void oct_buf_u32(oct_buf *buffer, uint32_t value)
{
    uint8_t bytes[4] = { (uint8_t)(value & 0xFFu), (uint8_t)((value >> 8) & 0xFFu),
                         (uint8_t)((value >> 16) & 0xFFu), (uint8_t)(value >> 24) };
    oct_buf_bytes(buffer, bytes, sizeof bytes);
}

void oct_buf_u64(oct_buf *buffer, uint64_t value)
{
    uint8_t bytes[8];
    for (unsigned i = 0; i < 8; i++) bytes[i] = (uint8_t)((value >> (8u * i)) & 0xFFu);
    oct_buf_bytes(buffer, bytes, sizeof bytes);
}

void oct_buf_patch_u32(oct_buf *buffer, size_t offset, uint32_t value)
{
    if (buffer->failed || offset > buffer->length || buffer->length - offset < 4) return;
    for (unsigned i = 0; i < 4; i++) buffer->data[offset + i] = (uint8_t)((value >> (8u * i)) & 0xFFu);
}

void oct_buf_string(oct_buf *buffer, const char *utf8)
{
    if (!utf8 || utf8[0] == '\0') {
        oct_buf_u8(buffer, 0);
        return;
    }
    size_t length = strlen(utf8);
    long units = oct_utf8_utf16_length(utf8, length);
    if (units < 1 || units > OCT_NAME_UTF16_MAX) {
        buffer->failed = true;
        return;
    }
    if (!oct_buf_reserve(buffer, 1 + 2 * ((size_t)units + 1))) return;
    oct_buf_u8(buffer, (uint8_t)(units + 1));
    size_t position = 0;
    while (position < length) {
        uint32_t codepoint = 0;
        position += oct_utf8_next(utf8, length, position, &codepoint);
        if (codepoint >= 0x10000u) {
            codepoint -= 0x10000u;
            oct_buf_u16(buffer, (uint16_t)(0xD800u + (codepoint >> 10)));
            oct_buf_u16(buffer, (uint16_t)(0xDC00u + (codepoint & 0x3FFu)));
        } else {
            oct_buf_u16(buffer, (uint16_t)codepoint);
        }
    }
    oct_buf_u16(buffer, 0);
}

void oct_buf_u16_array(oct_buf *buffer, const uint16_t *values, size_t count)
{
    if (count > UINT32_MAX) {
        buffer->failed = true;
        return;
    }
    oct_buf_u32(buffer, (uint32_t)count);
    for (size_t i = 0; i < count; i++) oct_buf_u16(buffer, values[i]);
}

void oct_buf_u32_array(oct_buf *buffer, const uint32_t *values, size_t count)
{
    if (count > UINT32_MAX) {
        buffer->failed = true;
        return;
    }
    oct_buf_u32(buffer, (uint32_t)count);
    for (size_t i = 0; i < count; i++) oct_buf_u32(buffer, values[i]);
}

/* ---- Reader ------------------------------------------------------------------- */

void oct_reader_init(oct_reader *reader, const void *data, size_t length)
{
    reader->data = data;
    reader->length = data ? length : 0;
    reader->position = 0;
    reader->failed = false;
}

size_t oct_reader_remaining(const oct_reader *reader)
{
    return reader->failed ? 0 : reader->length - reader->position;
}

static const uint8_t *take(oct_reader *reader, size_t count)
{
    if (reader->failed || count > reader->length - reader->position) {
        reader->failed = true;
        return NULL;
    }
    const uint8_t *p = reader->data + reader->position;
    reader->position += count;
    return p;
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint8_t oct_read_u8(oct_reader *reader)
{
    const uint8_t *p = take(reader, 1);
    return p ? p[0] : 0;
}

uint16_t oct_read_u16(oct_reader *reader)
{
    const uint8_t *p = take(reader, 2);
    return p ? le16(p) : 0;
}

uint32_t oct_read_u32(oct_reader *reader)
{
    const uint8_t *p = take(reader, 4);
    return p ? le32(p) : 0;
}

uint64_t oct_read_u64(oct_reader *reader)
{
    const uint8_t *p = take(reader, 8);
    return p ? (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32) : 0;
}

static size_t encode_utf8(uint32_t codepoint, char out[4])
{
    if (codepoint < 0x80u) {
        out[0] = (char)codepoint;
        return 1;
    }
    if (codepoint < 0x800u) {
        out[0] = (char)(0xC0u | (codepoint >> 6));
        out[1] = (char)(0x80u | (codepoint & 0x3Fu));
        return 2;
    }
    if (codepoint < 0x10000u) {
        out[0] = (char)(0xE0u | (codepoint >> 12));
        out[1] = (char)(0x80u | ((codepoint >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (codepoint & 0x3Fu));
        return 3;
    }
    out[0] = (char)(0xF0u | (codepoint >> 18));
    out[1] = (char)(0x80u | ((codepoint >> 12) & 0x3Fu));
    out[2] = (char)(0x80u | ((codepoint >> 6) & 0x3Fu));
    out[3] = (char)(0x80u | (codepoint & 0x3Fu));
    return 4;
}

bool oct_read_string(oct_reader *reader, char *utf8, size_t capacity)
{
    if (!utf8 || capacity == 0) return false;
    utf8[0] = '\0';
    uint8_t count = oct_read_u8(reader);
    if (reader->failed) return false;
    if (count == 0) return true;
    const uint8_t *units = take(reader, (size_t)count * 2u);
    if (!units) return false;
    if (le16(units + 2u * (count - 1u)) != 0) return false; /* missing terminator */
    size_t used = 0;
    for (size_t i = 0; i + 1 < count; i++) {
        uint32_t codepoint = le16(units + 2u * i);
        if (codepoint == 0) return false;
        if (codepoint >= 0xD800u && codepoint <= 0xDBFFu) {
            if (i + 2 >= count) return false;
            uint32_t low = le16(units + 2u * (i + 1));
            if (low < 0xDC00u || low > 0xDFFFu) return false;
            codepoint = 0x10000u + ((codepoint - 0xD800u) << 10) + (low - 0xDC00u);
            i++;
        } else if (codepoint >= 0xDC00u && codepoint <= 0xDFFFu) {
            return false;
        }
        char encoded[4];
        size_t width = encode_utf8(codepoint, encoded);
        if (used + width + 1 > capacity) return false;
        memcpy(utf8 + used, encoded, width);
        used += width;
    }
    utf8[used] = '\0';
    return true;
}

/* ---- Containers --------------------------------------------------------------- */

oct_parse_result oct_container_parse(const uint8_t *bytes, size_t length, oct_container *out)
{
    memset(out, 0, sizeof *out);
    if (!bytes || length < OCT_PTP_HEADER_SIZE) return OCT_PARSE_TRUNCATED;
    out->length = le32(bytes);
    out->type = le16(bytes + 4);
    out->code = le16(bytes + 6);
    out->transaction_id = le32(bytes + 8);
    if (out->type == OCT_CONTAINER_DATA) {
        /* 0xFFFFFFFF announces a data phase longer than 4 GiB. */
        if (out->length != 0xFFFFFFFFu && (uint64_t)out->length != (uint64_t)length) {
            return OCT_PARSE_LENGTH_MISMATCH;
        }
        out->payload = bytes + OCT_PTP_HEADER_SIZE;
        out->payload_length = length - OCT_PTP_HEADER_SIZE;
        return OCT_PARSE_OK;
    }
    if ((uint64_t)out->length != (uint64_t)length) return OCT_PARSE_LENGTH_MISMATCH;
    if (out->type != OCT_CONTAINER_COMMAND) return OCT_PARSE_BAD_TYPE;
    if (length > OCT_PTP_MAX_COMMAND || (length - OCT_PTP_HEADER_SIZE) % 4u != 0) {
        return OCT_PARSE_BAD_PARAMETERS;
    }
    out->param_count = (uint32_t)((length - OCT_PTP_HEADER_SIZE) / 4u);
    for (uint32_t i = 0; i < out->param_count; i++) {
        out->params[i] = le32(bytes + OCT_PTP_HEADER_SIZE + 4u * i);
    }
    return OCT_PARSE_OK;
}

static void container_with_params(oct_buf *buffer, uint16_t type, uint16_t code, uint32_t transaction_id,
                                  const uint32_t *params, uint32_t count)
{
    if (count > OCT_PTP_MAX_PARAMS || (count && !params)) count = 0;
    oct_buf_u32(buffer, OCT_PTP_HEADER_SIZE + 4u * count);
    oct_buf_u16(buffer, type);
    oct_buf_u16(buffer, code);
    oct_buf_u32(buffer, transaction_id);
    for (uint32_t i = 0; i < count; i++) oct_buf_u32(buffer, params[i]);
}

void oct_container_command(oct_buf *buffer, uint16_t code, uint32_t transaction_id,
                           const uint32_t *params, uint32_t count)
{
    container_with_params(buffer, OCT_CONTAINER_COMMAND, code, transaction_id, params, count);
}

void oct_container_response(oct_buf *buffer, uint16_t code, uint32_t transaction_id,
                            const uint32_t *params, uint32_t count)
{
    container_with_params(buffer, OCT_CONTAINER_RESPONSE, code, transaction_id, params, count);
}

size_t oct_container_data_begin(oct_buf *buffer, uint16_t code, uint32_t transaction_id)
{
    size_t start = buffer->length;
    oct_buf_u32(buffer, 0);
    oct_buf_u16(buffer, OCT_CONTAINER_DATA);
    oct_buf_u16(buffer, code);
    oct_buf_u32(buffer, transaction_id);
    return start;
}

void oct_container_data_end(oct_buf *buffer, size_t start)
{
    if (buffer->failed || start > buffer->length || buffer->length - start < OCT_PTP_HEADER_SIZE) return;
    uint64_t total = (uint64_t)(buffer->length - start);
    oct_buf_patch_u32(buffer, start, total >= 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)total);
}

/* ---- Datasets ----------------------------------------------------------------- */

bool oct_format_datetime(int64_t seconds, char out[OCT_DATETIME_LENGTH + 1])
{
    out[0] = '\0';
    time_t value = (time_t)seconds;
    struct tm parts;
    if (!localtime_r(&value, &parts) || parts.tm_year < -1900 || parts.tm_year > 9999 - 1900) return false;
    char text[64];
    int written = snprintf(text, sizeof text, "%04d%02d%02dT%02d%02d%02d", parts.tm_year + 1900,
                           parts.tm_mon + 1, parts.tm_mday, parts.tm_hour, parts.tm_min, parts.tm_sec);
    if (written != (int)OCT_DATETIME_LENGTH) return false;
    memcpy(out, text, OCT_DATETIME_LENGTH + 1);
    return true;
}

void oct_persistent_uid(const oct_object_info *info, uint8_t out[16])
{
    /* Unique per storage and handle; stable across sessions while handles are. */
    uint64_t low = ((uint64_t)info->storage_id << 32) | info->handle;
    uint64_t high = 0x214F43544F505553ull; /* "SUPOTCO!" little-endian tag */
    for (unsigned i = 0; i < 8; i++) {
        out[i] = (uint8_t)((low >> (8u * i)) & 0xFFu);
        out[8 + i] = (uint8_t)((high >> (8u * i)) & 0xFFu);
    }
}

void oct_dataset_device_info(oct_buf *buffer, const oct_device_identity *identity,
                             const oct_capabilities *capabilities)
{
    oct_buf_u16(buffer, 100);         /* StandardVersion 1.00 */
    oct_buf_u32(buffer, 0x00000006u); /* VendorExtensionID: Microsoft, i.e. MTP */
    oct_buf_u16(buffer, 100);         /* VendorExtensionVersion 1.00 */
    oct_buf_string(buffer, "microsoft.com: 1.0;");
    oct_buf_u16(buffer, 0);           /* FunctionalMode: standard */
    oct_buf_u16_array(buffer, capabilities->operations, capabilities->operation_count);
    oct_buf_u16_array(buffer, NULL, 0); /* EventsSupported: this responder sends none */
    oct_buf_u16_array(buffer, capabilities->device_properties, capabilities->device_property_count);
    oct_buf_u16_array(buffer, NULL, 0); /* CaptureFormats */
    oct_buf_u16_array(buffer, capabilities->formats, capabilities->format_count);
    oct_buf_string(buffer, identity->manufacturer);
    oct_buf_string(buffer, identity->model);
    oct_buf_string(buffer, identity->device_version);
    oct_buf_string(buffer, identity->serial_number);
}

void oct_dataset_storage_info(oct_buf *buffer, const oct_storage_info *info)
{
    oct_buf_u16(buffer, info->storage_type);
    oct_buf_u16(buffer, info->filesystem_type);
    oct_buf_u16(buffer, info->access_capability);
    oct_buf_u64(buffer, info->max_capacity);
    oct_buf_u64(buffer, info->free_space);
    oct_buf_u32(buffer, 0xFFFFFFFFu); /* FreeSpaceInImages: unused */
    oct_buf_string(buffer, info->description);
    oct_buf_string(buffer, info->volume_label);
}

void oct_dataset_object_info(oct_buf *buffer, const oct_object_info *info)
{
    char created[OCT_DATETIME_LENGTH + 1], modified[OCT_DATETIME_LENGTH + 1];
    oct_format_datetime(info->created, created);
    oct_format_datetime(info->modified, modified);
    uint64_t size = info->is_directory ? 0 : info->size;
    oct_buf_u32(buffer, info->storage_id);
    oct_buf_u16(buffer, info->format);
    oct_buf_u16(buffer, (uint16_t)(info->write_protected ? 0x0001u : 0x0000u));
    oct_buf_u32(buffer, size >= 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)size);
    oct_buf_u16(buffer, 0);                 /* ThumbFormat */
    for (int i = 0; i < 6; i++) oct_buf_u32(buffer, 0); /* thumb size/w/h, image w/h/depth */
    oct_buf_u32(buffer, info->parent);
    oct_buf_u16(buffer, (uint16_t)(info->is_directory ? OCT_ASSOCIATION_GENERIC_FOLDER : 0u));
    oct_buf_u32(buffer, 0);                 /* AssociationDesc */
    oct_buf_u32(buffer, 0);                 /* SequenceNumber */
    oct_buf_string(buffer, info->name);
    oct_buf_string(buffer, created);
    oct_buf_string(buffer, modified);
    oct_buf_string(buffer, "");             /* Keywords */
}

oct_status oct_dataset_parse_object_info(const uint8_t *bytes, size_t length, oct_object_info_request *out)
{
    memset(out, 0, sizeof *out);
    oct_reader reader;
    oct_reader_init(&reader, bytes, length);
    out->storage_id = oct_read_u32(&reader);
    out->format = oct_read_u16(&reader);
    out->protection_status = oct_read_u16(&reader);
    out->compressed_size = oct_read_u32(&reader);
    (void)oct_read_u16(&reader);                        /* ThumbFormat */
    for (int i = 0; i < 6; i++) (void)oct_read_u32(&reader);
    out->parent = oct_read_u32(&reader);
    out->association_type = oct_read_u16(&reader);
    (void)oct_read_u32(&reader);                        /* AssociationDesc */
    (void)oct_read_u32(&reader);                        /* SequenceNumber */
    if (!oct_read_string(&reader, out->name, sizeof out->name)) return OCT_E_MALFORMED;
    /* CaptureDate, ModificationDate and Keywords are validated, not used. */
    char ignored[1024];
    for (int i = 0; i < 3 && oct_reader_remaining(&reader) > 0; i++) {
        if (!oct_read_string(&reader, ignored, sizeof ignored)) return OCT_E_MALFORMED;
    }
    return reader.failed ? OCT_E_MALFORMED : OCT_OK;
}

static const uint16_t object_properties[] = {
    OCT_PROP_STORAGE_ID, OCT_PROP_OBJECT_FORMAT, OCT_PROP_PROTECTION_STATUS, OCT_PROP_OBJECT_SIZE,
    OCT_PROP_OBJECT_FILE_NAME, OCT_PROP_DATE_CREATED, OCT_PROP_DATE_MODIFIED, OCT_PROP_PARENT_OBJECT,
    OCT_PROP_PERSISTENT_UID, OCT_PROP_NAME,
};

const uint16_t *oct_object_properties(size_t *count)
{
    *count = sizeof object_properties / sizeof object_properties[0];
    return object_properties;
}

bool oct_object_property_supported(uint32_t property)
{
    for (size_t i = 0; i < sizeof object_properties / sizeof object_properties[0]; i++) {
        if (object_properties[i] == property) return true;
    }
    return false;
}

bool oct_object_property_settable(uint32_t property)
{
    return property == OCT_PROP_OBJECT_FILE_NAME || property == OCT_PROP_NAME;
}

uint16_t oct_object_property_type(uint32_t property)
{
    switch (property) {
    case OCT_PROP_STORAGE_ID:
    case OCT_PROP_PARENT_OBJECT: return OCT_TYPE_UINT32;
    case OCT_PROP_OBJECT_FORMAT:
    case OCT_PROP_PROTECTION_STATUS: return OCT_TYPE_UINT16;
    case OCT_PROP_OBJECT_SIZE: return OCT_TYPE_UINT64;
    case OCT_PROP_PERSISTENT_UID: return OCT_TYPE_UINT128;
    case OCT_PROP_OBJECT_FILE_NAME:
    case OCT_PROP_DATE_CREATED:
    case OCT_PROP_DATE_MODIFIED:
    case OCT_PROP_NAME: return OCT_TYPE_STRING;
    default: return 0;
    }
}

void oct_object_property_value(oct_buf *buffer, uint16_t property, const oct_object_info *info)
{
    char date[OCT_DATETIME_LENGTH + 1];
    uint8_t uid[16];
    switch (property) {
    case OCT_PROP_STORAGE_ID: oct_buf_u32(buffer, info->storage_id); break;
    case OCT_PROP_OBJECT_FORMAT: oct_buf_u16(buffer, info->format); break;
    case OCT_PROP_PROTECTION_STATUS: oct_buf_u16(buffer, (uint16_t)(info->write_protected ? 1u : 0u)); break;
    case OCT_PROP_OBJECT_SIZE: oct_buf_u64(buffer, info->is_directory ? 0 : info->size); break;
    case OCT_PROP_OBJECT_FILE_NAME:
    case OCT_PROP_NAME: oct_buf_string(buffer, info->name); break;
    case OCT_PROP_DATE_CREATED:
        oct_format_datetime(info->created, date);
        oct_buf_string(buffer, date);
        break;
    case OCT_PROP_DATE_MODIFIED:
        oct_format_datetime(info->modified, date);
        oct_buf_string(buffer, date);
        break;
    case OCT_PROP_PARENT_OBJECT: oct_buf_u32(buffer, info->parent); break;
    case OCT_PROP_PERSISTENT_UID:
        oct_persistent_uid(info, uid);
        oct_buf_bytes(buffer, uid, sizeof uid);
        break;
    default: buffer->failed = true; break;
    }
}

void oct_dataset_object_prop_desc(oct_buf *buffer, uint16_t property, bool settable)
{
    uint16_t type = oct_object_property_type(property);
    static const uint8_t zero[16] = { 0 };
    oct_buf_u16(buffer, property);
    oct_buf_u16(buffer, type);
    oct_buf_u8(buffer, settable ? 0x01 : 0x00); /* Get/Set */
    switch (type) {                            /* FactoryDefaultValue */
    case OCT_TYPE_UINT16: oct_buf_u16(buffer, 0); break;
    case OCT_TYPE_UINT32: oct_buf_u32(buffer, 0); break;
    case OCT_TYPE_UINT64: oct_buf_u64(buffer, 0); break;
    case OCT_TYPE_UINT128: oct_buf_bytes(buffer, zero, sizeof zero); break;
    default: oct_buf_string(buffer, ""); break;
    }
    oct_buf_u32(buffer, 0); /* GroupCode */
    if (property == OCT_PROP_PROTECTION_STATUS) {
        oct_buf_u8(buffer, 0x02); /* enumeration form */
        oct_buf_u16(buffer, 2);
        oct_buf_u16(buffer, 0x0000);
        oct_buf_u16(buffer, 0x0001);
    } else if (property == OCT_PROP_DATE_CREATED || property == OCT_PROP_DATE_MODIFIED) {
        oct_buf_u8(buffer, 0x03); /* DateTime form */
    } else {
        oct_buf_u8(buffer, 0x00); /* no form */
    }
}
