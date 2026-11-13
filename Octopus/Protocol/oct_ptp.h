/*
 * PTP/MTP wire format: generic containers, little-endian builders and
 * bounds-checked readers, PTP strings and the datasets the responder sends.
 * References: PIMA 15740:2000 (PTP 1.0), USB Still Image Capture Device
 * Definition 1.0 (containers) and Microsoft MTP 1.0 (object properties).
 */
#ifndef OCT_PTP_H
#define OCT_PTP_H

#include "../Core/oct_store.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OCT_PTP_HEADER_SIZE  12u
#define OCT_PTP_MAX_PARAMS   5u
#define OCT_PTP_MAX_COMMAND  32u                     /* header + five parameters */
#define OCT_PTP_MAX_TRANSFER (32u * 1024u * 1024u)   /* largest buffered data phase */

typedef enum oct_container_type {
    OCT_CONTAINER_COMMAND = 1,
    OCT_CONTAINER_DATA = 2,
    OCT_CONTAINER_RESPONSE = 3,
    OCT_CONTAINER_EVENT = 4
} oct_container_type;

enum {
    OCT_OP_GET_DEVICE_INFO = 0x1001,
    OCT_OP_OPEN_SESSION = 0x1002,
    OCT_OP_CLOSE_SESSION = 0x1003,
    OCT_OP_GET_STORAGE_IDS = 0x1004,
    OCT_OP_GET_STORAGE_INFO = 0x1005,
    OCT_OP_GET_NUM_OBJECTS = 0x1006,
    OCT_OP_GET_OBJECT_HANDLES = 0x1007,
    OCT_OP_GET_OBJECT_INFO = 0x1008,
    OCT_OP_GET_OBJECT = 0x1009,
    OCT_OP_GET_THUMB = 0x100A,
    OCT_OP_DELETE_OBJECT = 0x100B,
    OCT_OP_SEND_OBJECT_INFO = 0x100C,
    OCT_OP_SEND_OBJECT = 0x100D,
    OCT_OP_GET_DEVICE_PROP_DESC = 0x1014,
    OCT_OP_GET_DEVICE_PROP_VALUE = 0x1015,
    OCT_OP_MOVE_OBJECT = 0x1019,
    OCT_OP_GET_PARTIAL_OBJECT = 0x101B,
    OCT_OP_GET_OBJECT_PROPS_SUPPORTED = 0x9801,
    OCT_OP_GET_OBJECT_PROP_DESC = 0x9802,
    OCT_OP_GET_OBJECT_PROP_VALUE = 0x9803,
    OCT_OP_SET_OBJECT_PROP_VALUE = 0x9804,
    OCT_OP_GET_OBJECT_PROP_LIST = 0x9805
};

/* PTP events, which a device sends to a host outside any transaction. A host does not re-read the
 * storage list on its own, so a storage that starts being served mid-session stays invisible until
 * one of these says otherwise; that is why restarting ptpd alone never made the volume appear. */
enum {
    OCT_EVENT_OBJECT_ADDED   = 0x4002,
    OCT_EVENT_OBJECT_REMOVED = 0x4003,
    OCT_EVENT_STORE_ADDED    = 0x4004,
    OCT_EVENT_STORE_REMOVED  = 0x4005,
    OCT_EVENT_DEVICE_INFO_CHANGED = 0x4008, /* re-read DeviceInfo: what the device supports changed */
    OCT_EVENT_STORE_FULL     = 0x400A,
    OCT_EVENT_STORAGE_INFO_CHANGED = 0x400C
};

enum {
    OCT_RC_OK = 0x2001,
    OCT_RC_GENERAL_ERROR = 0x2002,
    OCT_RC_SESSION_NOT_OPEN = 0x2003,
    OCT_RC_INVALID_TRANSACTION_ID = 0x2004,
    OCT_RC_OPERATION_NOT_SUPPORTED = 0x2005,
    OCT_RC_INCOMPLETE_TRANSFER = 0x2007,
    OCT_RC_INVALID_STORAGE_ID = 0x2008,
    OCT_RC_INVALID_OBJECT_HANDLE = 0x2009,
    OCT_RC_DEVICE_PROP_NOT_SUPPORTED = 0x200A,
    OCT_RC_INVALID_OBJECT_FORMAT_CODE = 0x200B,
    OCT_RC_STORE_FULL = 0x200C,
    OCT_RC_OBJECT_WRITE_PROTECTED = 0x200D,
    OCT_RC_STORE_READ_ONLY = 0x200E,
    OCT_RC_ACCESS_DENIED = 0x200F,
    OCT_RC_PARTIAL_DELETION = 0x2012,
    OCT_RC_SPECIFICATION_BY_FORMAT_UNSUPPORTED = 0x2014,
    OCT_RC_NO_VALID_OBJECT_INFO = 0x2015,
    OCT_RC_INVALID_PARENT_OBJECT = 0x201A,
    OCT_RC_INVALID_PARAMETER = 0x201D,
    /* Apple's responder answers this for a data phase it does not recognise, and it is the right
     * answer for an upload too large for one USB read: a smaller file through the same path works,
     * so a transient-sounding refusal is the honest one. */
    OCT_RC_DEVICE_BUSY = 0x2019,
    OCT_RC_SESSION_ALREADY_OPEN = 0x201E,
    OCT_RC_INVALID_OBJECT_PROP_CODE = 0xA801,
    OCT_RC_INVALID_OBJECT_PROP_FORMAT = 0xA802,
    OCT_RC_INVALID_OBJECT_PROP_VALUE = 0xA803,
    OCT_RC_INVALID_DATASET = 0xA806,
    OCT_RC_SPECIFICATION_BY_GROUP_UNSUPPORTED = 0xA807,
    OCT_RC_SPECIFICATION_BY_DEPTH_UNSUPPORTED = 0xA808,
    OCT_RC_OBJECT_TOO_LARGE = 0xA809
};

enum {
    OCT_PROP_STORAGE_ID = 0xDC01,
    OCT_PROP_OBJECT_FORMAT = 0xDC02,
    OCT_PROP_PROTECTION_STATUS = 0xDC03,
    OCT_PROP_OBJECT_SIZE = 0xDC04,
    OCT_PROP_OBJECT_FILE_NAME = 0xDC07,
    OCT_PROP_DATE_CREATED = 0xDC08,
    OCT_PROP_DATE_MODIFIED = 0xDC09,
    OCT_PROP_PARENT_OBJECT = 0xDC0B,
    OCT_PROP_PERSISTENT_UID = 0xDC41,
    OCT_PROP_NAME = 0xDC44
};

enum {
    OCT_TYPE_UINT8 = 0x0002,
    OCT_TYPE_UINT16 = 0x0004,
    OCT_TYPE_UINT32 = 0x0006,
    OCT_TYPE_UINT64 = 0x0008,
    OCT_TYPE_UINT128 = 0x000A,
    OCT_TYPE_STRING = 0xFFFF
};

#define OCT_DEVICE_PROP_FRIENDLY_NAME  0xD402u
#define OCT_ASSOCIATION_GENERIC_FOLDER 0x0001u
#define OCT_DATETIME_LENGTH            15u     /* "YYYYMMDDThhmmss" */

/* Growable output buffer. Appends never write out of bounds: on allocation
 * failure or when `limit` would be exceeded, `failed` is set and stays set. */
#define OCT_BUF_DEFAULT_LIMIT (64u * 1024u * 1024u)
typedef struct oct_buf {
    uint8_t *data;
    size_t   length;
    size_t   capacity;
    size_t   limit;
    bool     failed;
} oct_buf;

void oct_buf_init(oct_buf *buffer, size_t limit); /* 0 selects the default */
void oct_buf_free(oct_buf *buffer);
void oct_buf_clear(oct_buf *buffer);
bool oct_buf_reserve(oct_buf *buffer, size_t extra);
void oct_buf_u8(oct_buf *buffer, uint8_t value);
void oct_buf_u16(oct_buf *buffer, uint16_t value);
void oct_buf_u32(oct_buf *buffer, uint32_t value);
void oct_buf_u64(oct_buf *buffer, uint64_t value);
void oct_buf_bytes(oct_buf *buffer, const void *data, size_t length);
void oct_buf_patch_u32(oct_buf *buffer, size_t offset, uint32_t value);
void oct_buf_string(oct_buf *buffer, const char *utf8); /* PTP string */
void oct_buf_u16_array(oct_buf *buffer, const uint16_t *values, size_t count);
void oct_buf_u32_array(oct_buf *buffer, const uint32_t *values, size_t count);

/* Bounds-checked reader: reading past the end sets `failed` and yields zeros. */
typedef struct oct_reader {
    const uint8_t *data;
    size_t         length;
    size_t         position;
    bool           failed;
} oct_reader;

void     oct_reader_init(oct_reader *reader, const void *data, size_t length);
size_t   oct_reader_remaining(const oct_reader *reader);
uint8_t  oct_read_u8(oct_reader *reader);
uint16_t oct_read_u16(oct_reader *reader);
uint32_t oct_read_u32(oct_reader *reader);
uint64_t oct_read_u64(oct_reader *reader);
/* Decodes a PTP string to UTF-8. Rejects a missing terminator, embedded NULs,
 * unpaired surrogates and output that does not fit `capacity`. */
bool     oct_read_string(oct_reader *reader, char *utf8, size_t capacity);

typedef struct oct_container {
    uint32_t       length;
    uint16_t       type;
    uint16_t       code;
    uint32_t       transaction_id;
    uint32_t       params[OCT_PTP_MAX_PARAMS];
    uint32_t       param_count;
    const uint8_t *payload;        /* data containers: bytes after the header */
    size_t         payload_length;
} oct_container;

typedef enum oct_parse_result {
    OCT_PARSE_OK = 0,
    OCT_PARSE_TRUNCATED,        /* shorter than a container header */
    OCT_PARSE_LENGTH_MISMATCH,  /* declared length differs from received bytes */
    OCT_PARSE_BAD_TYPE,         /* not a command or data container */
    OCT_PARSE_BAD_PARAMETERS    /* command longer than 32 bytes or misaligned */
} oct_parse_result;

/* Parses one complete host-to-device container. Header fields are filled in
 * whenever at least 12 bytes are present, even if validation then fails. */
oct_parse_result oct_container_parse(const uint8_t *bytes, size_t length, oct_container *out);
void   oct_container_command(oct_buf *buffer, uint16_t code, uint32_t transaction_id,
                             const uint32_t *params, uint32_t count);
void   oct_container_response(oct_buf *buffer, uint16_t code, uint32_t transaction_id,
                              const uint32_t *params, uint32_t count);
size_t oct_container_data_begin(oct_buf *buffer, uint16_t code, uint32_t transaction_id);
void   oct_container_data_end(oct_buf *buffer, size_t start);

typedef struct oct_device_identity {
    const char *manufacturer;
    const char *model;
    const char *device_version;
    const char *serial_number;
} oct_device_identity;

typedef struct oct_capabilities {
    const uint16_t *operations;
    size_t          operation_count;
    const uint16_t *device_properties;
    size_t          device_property_count;
    const uint16_t *formats;
    size_t          format_count;
} oct_capabilities;

typedef struct oct_object_info_request { /* ObjectInfo sent with SendObjectInfo */
    uint32_t storage_id;
    uint16_t format;
    uint16_t protection_status;
    uint32_t compressed_size;
    uint32_t parent;
    uint16_t association_type;
    char     name[OCT_NAME_MAX + 1];
} oct_object_info_request;

void       oct_dataset_device_info(oct_buf *buffer, const oct_device_identity *identity,
                                   const oct_capabilities *capabilities);
void       oct_dataset_storage_info(oct_buf *buffer, const oct_storage_info *info);
void       oct_dataset_object_info(oct_buf *buffer, const oct_object_info *info);
oct_status oct_dataset_parse_object_info(const uint8_t *bytes, size_t length,
                                         oct_object_info_request *out);

const uint16_t *oct_object_properties(size_t *count);
bool     oct_object_property_supported(uint32_t property);
bool     oct_object_property_settable(uint32_t property);
uint16_t oct_object_property_type(uint32_t property);
void     oct_object_property_value(oct_buf *buffer, uint16_t property, const oct_object_info *info);
void     oct_dataset_object_prop_desc(oct_buf *buffer, uint16_t property, bool settable);

bool oct_format_datetime(int64_t seconds, char out[OCT_DATETIME_LENGTH + 1]); /* local time */
void oct_persistent_uid(const oct_object_info *info, uint8_t out[16]);

#ifdef __cplusplus
}
#endif

#endif /* OCT_PTP_H */
