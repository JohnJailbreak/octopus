#include "oct_responder.h"

#include <stdlib.h>
#include <string.h>

static const uint16_t operations[] = {
    OCT_OP_GET_DEVICE_INFO, OCT_OP_OPEN_SESSION, OCT_OP_CLOSE_SESSION, OCT_OP_GET_STORAGE_IDS,
    OCT_OP_GET_STORAGE_INFO, OCT_OP_GET_NUM_OBJECTS, OCT_OP_GET_OBJECT_HANDLES,
    OCT_OP_GET_OBJECT_INFO, OCT_OP_GET_OBJECT, OCT_OP_DELETE_OBJECT, OCT_OP_SEND_OBJECT_INFO,
    OCT_OP_SEND_OBJECT, OCT_OP_GET_DEVICE_PROP_DESC, OCT_OP_GET_DEVICE_PROP_VALUE,
    OCT_OP_GET_PARTIAL_OBJECT, OCT_OP_GET_OBJECT_PROPS_SUPPORTED, OCT_OP_GET_OBJECT_PROP_DESC,
    OCT_OP_GET_OBJECT_PROP_VALUE, OCT_OP_SET_OBJECT_PROP_VALUE, OCT_OP_GET_OBJECT_PROP_LIST,
};
static const uint16_t device_properties[] = { OCT_DEVICE_PROP_FRIENDLY_NAME };
static const uint16_t formats[] = { OCT_FORMAT_UNDEFINED, OCT_FORMAT_ASSOCIATION, OCT_FORMAT_TEXT };

#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

struct oct_responder {
    oct_store          *store;
    char                manufacturer[OCT_LABEL_MAX + 1];
    char                model[OCT_LABEL_MAX + 1];
    char                device_version[OCT_LABEL_MAX + 1];
    char                serial_number[OCT_LABEL_MAX + 1];
    char                friendly_name[OCT_LABEL_MAX + 1];
    bool                session_open;
    uint32_t            session_id;
    uint32_t            expected_transaction;
    bool                awaiting_data;
    oct_container       pending;        /* command whose data phase is outstanding */
    bool                send_reserved;  /* SendObjectInfo accepted a file */
    uint32_t            send_handle;
    uint32_t            send_size;
    oct_responder_stats stats;
};

const uint16_t *oct_responder_operations(size_t *count)
{
    *count = COUNT(operations);
    return operations;
}

static bool format_supported(uint32_t format)
{
    for (size_t i = 0; i < COUNT(formats); i++) {
        if (formats[i] == format) return true;
    }
    return false;
}

static uint32_t next_transaction(uint32_t transaction)
{
    /* 0 is reserved for OpenSession and 0xFFFFFFFF is invalid. */
    return transaction >= 0xFFFFFFFEu ? 1u : transaction + 1u;
}

static uint16_t response_for(oct_status status)
{
    switch (status) {
    case OCT_OK: return OCT_RC_OK;
    case OCT_E_INVALID_HANDLE:
    case OCT_E_IS_DIRECTORY: return OCT_RC_INVALID_OBJECT_HANDLE;
    case OCT_E_INVALID_PARENT:
    case OCT_E_NOT_DIRECTORY: return OCT_RC_INVALID_PARENT_OBJECT;
    case OCT_E_WRITE_PROTECTED: return OCT_RC_OBJECT_WRITE_PROTECTED;
    case OCT_E_PARTIAL_DELETION: return OCT_RC_PARTIAL_DELETION;
    case OCT_E_OUT_OF_RANGE:
    case OCT_E_INVALID_ARGUMENT: return OCT_RC_INVALID_PARAMETER;
    case OCT_E_NO_SPACE: return OCT_RC_STORE_FULL;
    case OCT_E_TOO_LARGE: return OCT_RC_OBJECT_TOO_LARGE;
    case OCT_E_UNSUPPORTED: return OCT_RC_OPERATION_NOT_SUPPORTED;
    default: return OCT_RC_GENERAL_ERROR;
    }
}

static oct_status respond(oct_responder *r, oct_buf *out, uint16_t code, uint32_t transaction,
                          const uint32_t *params, uint32_t count)
{
    oct_container_response(out, code, transaction, params, count);
    r->stats.last_response = code;
    if (code != OCT_RC_OK) r->stats.failures++;
    return out->failed ? OCT_E_NO_MEMORY : OCT_OK;
}

static oct_status respond_code(oct_responder *r, oct_buf *out, uint16_t code, uint32_t transaction)
{
    return respond(r, out, code, transaction, NULL, 0);
}

/* Closes a data container started at `start` and appends OK. If the payload
 * could not be built, the partial container is withdrawn and GeneralError is
 * returned instead, so the host never sees a truncated dataset. */
static oct_status finish_data(oct_responder *r, oct_buf *out, size_t start, uint32_t transaction,
                              const uint32_t *params, uint32_t count)
{
    if (out->failed) {
        out->length = start;
        out->failed = false;
        return respond_code(r, out, OCT_RC_GENERAL_ERROR, transaction);
    }
    oct_container_data_end(out, start);
    return respond(r, out, OCT_RC_OK, transaction, params, count);
}

static void abandon_send(oct_responder *r)
{
    if (!r->send_reserved) return;
    uint32_t deleted = 0;
    oct_store_delete(r->store, r->send_handle, &deleted);
    r->send_reserved = false;
}

static void end_session(oct_responder *r)
{
    abandon_send(r);
    r->session_open = false;
    r->session_id = 0;
    r->awaiting_data = false;
}

static bool storage_writable(oct_responder *r)
{
    oct_storage_info info;
    return oct_store_storage_info(r->store, &info) == OCT_OK && info.access_capability == 0x0000u;
}

/* ---- Operations ------------------------------------------------------------------ */

static oct_status device_info(oct_responder *r, uint32_t transaction, oct_buf *out)
{
    oct_device_identity identity = { r->manufacturer, r->model, r->device_version, r->serial_number };
    oct_capabilities capabilities = { operations, COUNT(operations), device_properties,
                                      COUNT(device_properties), formats, COUNT(formats) };
    size_t start = oct_container_data_begin(out, OCT_OP_GET_DEVICE_INFO, transaction);
    oct_dataset_device_info(out, &identity, &capabilities);
    return finish_data(r, out, start, transaction, NULL, 0);
}

static oct_status open_session(oct_responder *r, const oct_container *c, oct_buf *out)
{
    if (r->session_open) {
        /* Keep the session but follow the host's numbering: an initiator that
         * restarted (libmtp does) continues from OpenSession's transaction. */
        uint32_t current = r->session_id;
        r->expected_transaction = next_transaction(c->transaction_id);
        return respond(r, out, OCT_RC_SESSION_ALREADY_OPEN, c->transaction_id, &current, 1);
    }
    if (c->params[0] == 0) return respond_code(r, out, OCT_RC_INVALID_PARAMETER, c->transaction_id);
    r->session_open = true;
    r->session_id = c->params[0];
    r->expected_transaction = next_transaction(c->transaction_id);
    return respond_code(r, out, OCT_RC_OK, c->transaction_id);
}

static oct_status storage_ids(oct_responder *r, uint32_t transaction, oct_buf *out)
{
    uint32_t id = oct_store_storage_id(r->store);
    size_t start = oct_container_data_begin(out, OCT_OP_GET_STORAGE_IDS, transaction);
    oct_buf_u32_array(out, &id, 1);
    return finish_data(r, out, start, transaction, NULL, 0);
}

static oct_status storage_info(oct_responder *r, const oct_container *c, oct_buf *out)
{
    oct_storage_info info;
    if (c->params[0] != oct_store_storage_id(r->store)) {
        return respond_code(r, out, OCT_RC_INVALID_STORAGE_ID, c->transaction_id);
    }
    oct_status status = oct_store_storage_info(r->store, &info);
    if (status != OCT_OK) return respond_code(r, out, response_for(status), c->transaction_id);
    size_t start = oct_container_data_begin(out, OCT_OP_GET_STORAGE_INFO, c->transaction_id);
    oct_dataset_storage_info(out, &info);
    return finish_data(r, out, start, c->transaction_id, NULL, 0);
}

static bool format_matches(uint32_t filter, uint16_t format)
{
    if (filter == 0) return true;
    if (filter == 0xFFFFFFFFu) return (format & 0xFF00u) == 0x3800u; /* image formats */
    return filter == format;
}

static oct_status filter_by_format(oct_store *store, oct_handle_list *list, uint32_t format)
{
    if (format == 0) return OCT_OK;
    uint32_t kept = 0;
    for (uint32_t i = 0; i < list->count; i++) {
        oct_object_info info;
        oct_status status = oct_store_get(store, list->handles[i], &info);
        if (status != OCT_OK) return status;
        if (format_matches(format, info.format)) list->handles[kept++] = list->handles[i];
    }
    list->count = kept;
    return OCT_OK;
}

static oct_status object_handles(oct_responder *r, const oct_container *c, oct_buf *out, bool count_only)
{
    uint32_t storage = c->params[0], format = c->params[1], parent = c->params[2];
    uint32_t transaction = c->transaction_id;
    if (storage != 0xFFFFFFFFu && storage != oct_store_storage_id(r->store)) {
        return respond_code(r, out, OCT_RC_INVALID_STORAGE_ID, transaction);
    }
    if (format > 0xFFFFu && format != 0xFFFFFFFFu) {
        return respond_code(r, out, OCT_RC_INVALID_OBJECT_FORMAT_CODE, transaction);
    }
    oct_handle_list list = { 0 };
    /* Parent 0x00000000: the whole hierarchy; 0xFFFFFFFF: root level only. */
    oct_status status = parent == 0 ? oct_store_list(r->store, OCT_ROOT, true, &list)
                                    : oct_store_list(r->store, parent == 0xFFFFFFFFu ? OCT_ROOT : parent, false, &list);
    if (status == OCT_OK) status = filter_by_format(r->store, &list, format);
    if (status != OCT_OK) {
        oct_handle_list_free(&list);
        /* OCT_E_TOO_LARGE means the store's enumeration budget was reached, which only happens
         * for the whole-hierarchy request (association 0) over a large backend. Telling the host
         * that specification by depth is unsupported makes it ask per directory instead, which
         * the store answers without a recursive walk. */
        uint16_t code = status == OCT_E_INVALID_HANDLE ? OCT_RC_INVALID_OBJECT_HANDLE
                        : status == OCT_E_NOT_DIRECTORY ? OCT_RC_INVALID_PARENT_OBJECT
                        : status == OCT_E_TOO_LARGE     ? OCT_RC_SPECIFICATION_BY_DEPTH_UNSUPPORTED
                                                        : response_for(status);
        return respond_code(r, out, code, transaction);
    }
    if (count_only) {
        uint32_t count = list.count;
        oct_handle_list_free(&list);
        return respond(r, out, OCT_RC_OK, transaction, &count, 1);
    }
    size_t start = oct_container_data_begin(out, OCT_OP_GET_OBJECT_HANDLES, transaction);
    oct_buf_u32_array(out, list.handles, list.count);
    oct_handle_list_free(&list);
    return finish_data(r, out, start, transaction, NULL, 0);
}

static oct_status object_info(oct_responder *r, const oct_container *c, oct_buf *out)
{
    oct_object_info info;
    if (oct_store_get(r->store, c->params[0], &info) != OCT_OK) {
        return respond_code(r, out, OCT_RC_INVALID_OBJECT_HANDLE, c->transaction_id);
    }
    size_t start = oct_container_data_begin(out, OCT_OP_GET_OBJECT_INFO, c->transaction_id);
    oct_dataset_object_info(out, &info);
    return finish_data(r, out, start, c->transaction_id, NULL, 0);
}

/* Shared by GetObject and GetPartialObject. */
static oct_status send_bytes(oct_responder *r, uint16_t operation, uint32_t transaction, uint32_t handle,
                             uint64_t offset, uint64_t length, bool report_length, oct_buf *out)
{
    size_t start = oct_container_data_begin(out, operation, transaction);
    size_t count = (size_t)length;
    if (!oct_buf_reserve(out, count)) return finish_data(r, out, start, transaction, NULL, 0);
    size_t read = 0;
    oct_status status = count ? oct_store_read(r->store, handle, offset, out->data + out->length, count, &read)
                              : OCT_OK;
    if (status != OCT_OK || read != count) {
        out->length = start;
        return respond_code(r, out, OCT_RC_GENERAL_ERROR, transaction);
    }
    out->length += read;
    uint32_t sent = (uint32_t)read;
    return finish_data(r, out, start, transaction, report_length ? &sent : NULL, report_length ? 1u : 0u);
}

static oct_status get_object(oct_responder *r, const oct_container *c, oct_buf *out, bool partial)
{
    oct_object_info info;
    uint32_t transaction = c->transaction_id;
    if (oct_store_get(r->store, c->params[0], &info) != OCT_OK || info.is_directory) {
        return respond_code(r, out, OCT_RC_INVALID_OBJECT_HANDLE, transaction);
    }
    if (!partial) {
        if (info.size > OCT_PTP_MAX_TRANSFER) return respond_code(r, out, OCT_RC_GENERAL_ERROR, transaction);
        return send_bytes(r, OCT_OP_GET_OBJECT, transaction, info.handle, 0, info.size, false, out);
    }
    uint64_t offset = c->params[1];
    if (offset > info.size) return respond_code(r, out, OCT_RC_INVALID_PARAMETER, transaction);
    uint64_t length = info.size - offset;
    if (length > c->params[2]) length = c->params[2];
    if (length > OCT_PTP_MAX_TRANSFER) length = OCT_PTP_MAX_TRANSFER;
    return send_bytes(r, OCT_OP_GET_PARTIAL_OBJECT, transaction, info.handle, offset, length, true, out);
}

static oct_status delete_object(oct_responder *r, const oct_container *c, oct_buf *out)
{
    uint32_t handle = c->params[0], transaction = c->transaction_id;
    if (!storage_writable(r)) return respond_code(r, out, OCT_RC_STORE_READ_ONLY, transaction);
    uint16_t code;
    if (handle == 0xFFFFFFFFu) {
        if (c->params[1] != 0) return respond_code(r, out, OCT_RC_SPECIFICATION_BY_FORMAT_UNSUPPORTED, transaction);
        oct_handle_list list = { 0 };
        oct_status status = oct_store_list(r->store, OCT_ROOT, false, &list);
        uint32_t removed = 0, failures = 0;
        for (uint32_t i = 0; status == OCT_OK && i < list.count; i++) {
            uint32_t deleted = 0;
            if (oct_store_delete(r->store, list.handles[i], &deleted) != OCT_OK) failures++;
            removed += deleted;
        }
        oct_handle_list_free(&list);
        code = status != OCT_OK ? response_for(status)
               : failures == 0  ? OCT_RC_OK
               : removed > 0    ? OCT_RC_PARTIAL_DELETION
                                : OCT_RC_OBJECT_WRITE_PROTECTED;
    } else {
        uint32_t deleted = 0;
        code = response_for(oct_store_delete(r->store, handle, &deleted));
    }
    oct_object_info info;
    if (r->send_reserved && oct_store_get(r->store, r->send_handle, &info) != OCT_OK) r->send_reserved = false;
    return respond_code(r, out, code, transaction);
}

static oct_status send_object_info(oct_responder *r, const oct_container *c, const uint8_t *payload,
                                   size_t length, oct_buf *out)
{
    uint32_t storage = c->params[0], parent = c->params[1], transaction = c->transaction_id;
    uint32_t storage_id = oct_store_storage_id(r->store);
    if (storage != 0 && storage != storage_id) return respond_code(r, out, OCT_RC_INVALID_STORAGE_ID, transaction);
    oct_object_info_request request;
    if (oct_dataset_parse_object_info(payload, length, &request) != OCT_OK) {
        return respond_code(r, out, OCT_RC_INVALID_DATASET, transaction);
    }
    abandon_send(r); /* a new SendObjectInfo replaces an unfinished one */
    oct_storage_info storage_state;
    if (oct_store_storage_info(r->store, &storage_state) != OCT_OK || storage_state.access_capability != 0) {
        return respond_code(r, out, OCT_RC_STORE_READ_ONLY, transaction);
    }
    uint32_t target = (parent == 0 || parent == 0xFFFFFFFFu) ? OCT_ROOT : parent;
    if (target != OCT_ROOT) {
        oct_object_info info;
        if (oct_store_get(r->store, target, &info) != OCT_OK || !info.is_directory) {
            return respond_code(r, out, OCT_RC_INVALID_PARENT_OBJECT, transaction);
        }
    }
    bool directory = request.format == OCT_FORMAT_ASSOCIATION;
    if (!directory) {
        if (request.compressed_size == 0xFFFFFFFFu || request.compressed_size > OCT_PTP_MAX_TRANSFER) {
            return respond_code(r, out, OCT_RC_OBJECT_TOO_LARGE, transaction);
        }
        if (request.compressed_size > storage_state.free_space) {
            return respond_code(r, out, OCT_RC_STORE_FULL, transaction);
        }
    }
    uint32_t handle = 0;
    oct_status status = oct_store_create(r->store, target, request.name, directory, &handle);
    if (status != OCT_OK) {
        uint16_t code = status == OCT_E_INVALID_NAME      ? OCT_RC_INVALID_DATASET
                        : status == OCT_E_WRITE_PROTECTED ? OCT_RC_ACCESS_DENIED
                                                          : response_for(status);
        return respond_code(r, out, code, transaction);
    }
    if (!directory) {
        r->send_reserved = true;
        r->send_handle = handle;
        r->send_size = request.compressed_size;
    }
    uint32_t params[3] = { storage_id, target, handle };
    return respond(r, out, OCT_RC_OK, transaction, params, 3);
}

static oct_status send_object(oct_responder *r, const oct_container *c, const uint8_t *payload,
                              size_t length, oct_buf *out)
{
    uint32_t transaction = c->transaction_id;
    if (!r->send_reserved) return respond_code(r, out, OCT_RC_NO_VALID_OBJECT_INFO, transaction);
    uint32_t handle = r->send_handle;
    if ((uint64_t)length != (uint64_t)r->send_size) {
        abandon_send(r);
        return respond_code(r, out, OCT_RC_INCOMPLETE_TRANSFER, transaction);
    }
    r->send_reserved = false;
    oct_status status = length ? oct_store_write(r->store, handle, 0, payload, length) : OCT_OK;
    if (status != OCT_OK) {
        uint32_t deleted = 0;
        oct_store_delete(r->store, handle, &deleted);
        return respond_code(r, out, status == OCT_E_NO_SPACE ? OCT_RC_STORE_FULL : OCT_RC_GENERAL_ERROR, transaction);
    }
    return respond_code(r, out, OCT_RC_OK, transaction);
}

static oct_status set_object_prop_value(oct_responder *r, const oct_container *c, const uint8_t *payload,
                                        size_t length, oct_buf *out)
{
    uint32_t handle = c->params[0], property = c->params[1], transaction = c->transaction_id;
    oct_object_info info;
    if (oct_store_get(r->store, handle, &info) != OCT_OK) {
        return respond_code(r, out, OCT_RC_INVALID_OBJECT_HANDLE, transaction);
    }
    if (!oct_object_property_supported(property)) {
        return respond_code(r, out, OCT_RC_INVALID_OBJECT_PROP_CODE, transaction);
    }
    if (!oct_object_property_settable(property)) return respond_code(r, out, OCT_RC_ACCESS_DENIED, transaction);
    oct_reader reader;
    char name[1024];
    oct_reader_init(&reader, payload, length);
    if (!oct_read_string(&reader, name, sizeof name) || oct_reader_remaining(&reader) != 0) {
        return respond_code(r, out, OCT_RC_INVALID_OBJECT_PROP_FORMAT, transaction);
    }
    oct_status status = oct_store_rename(r->store, handle, name);
    uint16_t code = (status == OCT_E_INVALID_NAME || status == OCT_E_EXISTS) ? OCT_RC_INVALID_OBJECT_PROP_VALUE
                                                                             : response_for(status);
    return respond_code(r, out, code, transaction);
}

static oct_status object_props_supported(oct_responder *r, const oct_container *c, oct_buf *out)
{
    if (!format_supported(c->params[0])) {
        return respond_code(r, out, OCT_RC_INVALID_OBJECT_FORMAT_CODE, c->transaction_id);
    }
    size_t count = 0;
    const uint16_t *properties = oct_object_properties(&count);
    size_t start = oct_container_data_begin(out, OCT_OP_GET_OBJECT_PROPS_SUPPORTED, c->transaction_id);
    oct_buf_u16_array(out, properties, count);
    return finish_data(r, out, start, c->transaction_id, NULL, 0);
}

static oct_status object_prop_desc(oct_responder *r, const oct_container *c, oct_buf *out)
{
    uint32_t property = c->params[0], transaction = c->transaction_id;
    if (!oct_object_property_supported(property)) {
        return respond_code(r, out, OCT_RC_INVALID_OBJECT_PROP_CODE, transaction);
    }
    if (!format_supported(c->params[1])) return respond_code(r, out, OCT_RC_INVALID_OBJECT_FORMAT_CODE, transaction);
    size_t start = oct_container_data_begin(out, OCT_OP_GET_OBJECT_PROP_DESC, transaction);
    oct_dataset_object_prop_desc(out, (uint16_t)property,
                                 oct_object_property_settable(property) && storage_writable(r));
    return finish_data(r, out, start, transaction, NULL, 0);
}

static oct_status object_prop_value(oct_responder *r, const oct_container *c, oct_buf *out)
{
    oct_object_info info;
    uint32_t property = c->params[1], transaction = c->transaction_id;
    if (oct_store_get(r->store, c->params[0], &info) != OCT_OK) {
        return respond_code(r, out, OCT_RC_INVALID_OBJECT_HANDLE, transaction);
    }
    if (!oct_object_property_supported(property)) {
        return respond_code(r, out, OCT_RC_INVALID_OBJECT_PROP_CODE, transaction);
    }
    size_t start = oct_container_data_begin(out, OCT_OP_GET_OBJECT_PROP_VALUE, transaction);
    oct_object_property_value(out, (uint16_t)property, &info);
    return finish_data(r, out, start, transaction, NULL, 0);
}

static oct_status object_prop_list(oct_responder *r, const oct_container *c, oct_buf *out)
{
    uint32_t handle = c->params[0], format = c->params[1], property = c->params[2];
    uint32_t depth = c->params[4], transaction = c->transaction_id;
    if (property == 0) return respond_code(r, out, OCT_RC_SPECIFICATION_BY_GROUP_UNSUPPORTED, transaction);
    if (property != 0xFFFFFFFFu && !oct_object_property_supported(property)) {
        return respond_code(r, out, OCT_RC_INVALID_OBJECT_PROP_CODE, transaction);
    }
    if (format > 0xFFFFu) return respond_code(r, out, OCT_RC_INVALID_OBJECT_FORMAT_CODE, transaction);
    oct_handle_list list = { 0 };
    oct_status status = OCT_OK;
    if (handle == 0xFFFFFFFFu) {
        status = oct_store_list(r->store, OCT_ROOT, true, &list);
    } else if (depth == 0) {
        oct_object_info info;
        if (handle != 0) {
            if (oct_store_get(r->store, handle, &info) != OCT_OK) {
                return respond_code(r, out, OCT_RC_INVALID_OBJECT_HANDLE, transaction);
            }
            list.handles = malloc(sizeof *list.handles);
            if (!list.handles) return respond_code(r, out, OCT_RC_GENERAL_ERROR, transaction);
            list.handles[0] = handle;
            list.count = list.capacity = 1;
        }
    } else if (depth == 1 || depth == 0xFFFFFFFFu) {
        status = oct_store_list(r->store, handle == 0 ? OCT_ROOT : handle, depth != 1, &list);
        if (status == OCT_E_NOT_DIRECTORY) status = OCT_OK; /* a file has no children */
    } else {
        return respond_code(r, out, OCT_RC_SPECIFICATION_BY_DEPTH_UNSUPPORTED, transaction);
    }
    if (status == OCT_OK) status = filter_by_format(r->store, &list, format);
    if (status != OCT_OK) {
        oct_handle_list_free(&list);
        return respond_code(r, out, status == OCT_E_INVALID_HANDLE ? OCT_RC_INVALID_OBJECT_HANDLE
                                                                   : response_for(status), transaction);
    }
    size_t all_count = 0;
    const uint16_t *all = oct_object_properties(&all_count);
    uint16_t single = (uint16_t)property;
    const uint16_t *selected = property == 0xFFFFFFFFu ? all : &single;
    size_t selected_count = property == 0xFFFFFFFFu ? all_count : 1;
    size_t start = oct_container_data_begin(out, OCT_OP_GET_OBJECT_PROP_LIST, transaction);
    size_t count_at = out->length;
    oct_buf_u32(out, 0);
    uint32_t elements = 0;
    for (uint32_t i = 0; i < list.count && !out->failed; i++) {
        oct_object_info info;
        if (oct_store_get(r->store, list.handles[i], &info) != OCT_OK) continue;
        for (size_t k = 0; k < selected_count; k++) {
            oct_buf_u32(out, info.handle);
            oct_buf_u16(out, selected[k]);
            oct_buf_u16(out, oct_object_property_type(selected[k]));
            oct_object_property_value(out, selected[k], &info);
            elements++;
        }
    }
    oct_handle_list_free(&list);
    oct_buf_patch_u32(out, count_at, elements);
    return finish_data(r, out, start, transaction, NULL, 0);
}

static oct_status device_prop(oct_responder *r, const oct_container *c, oct_buf *out, bool description)
{
    if (c->params[0] != OCT_DEVICE_PROP_FRIENDLY_NAME) {
        return respond_code(r, out, OCT_RC_DEVICE_PROP_NOT_SUPPORTED, c->transaction_id);
    }
    uint16_t operation = description ? OCT_OP_GET_DEVICE_PROP_DESC : OCT_OP_GET_DEVICE_PROP_VALUE;
    size_t start = oct_container_data_begin(out, operation, c->transaction_id);
    if (description) {
        oct_buf_u16(out, OCT_DEVICE_PROP_FRIENDLY_NAME);
        oct_buf_u16(out, OCT_TYPE_STRING);
        oct_buf_u8(out, 0x00);                  /* read-only */
        oct_buf_string(out, r->friendly_name);  /* factory default */
        oct_buf_string(out, r->friendly_name);  /* current value */
        oct_buf_u8(out, 0x00);                  /* no form */
    } else {
        oct_buf_string(out, r->friendly_name);
    }
    return finish_data(r, out, start, c->transaction_id, NULL, 0);
}

/* ---- Dispatch ---------------------------------------------------------------------- */

static oct_status command(oct_responder *r, const oct_container *c, oct_buf *out)
{
    uint32_t transaction = c->transaction_id;
    r->stats.commands++;
    r->stats.last_operation = c->code;
    if (c->code == OCT_OP_OPEN_SESSION) return open_session(r, c, out);
    if (!r->session_open) {
        if (c->code == OCT_OP_GET_DEVICE_INFO) return device_info(r, transaction, out);
        return respond_code(r, out, OCT_RC_SESSION_NOT_OPEN, transaction);
    }
    if (transaction != r->expected_transaction) {
        return respond_code(r, out, OCT_RC_INVALID_TRANSACTION_ID, transaction);
    }
    r->expected_transaction = next_transaction(transaction);
    switch (c->code) {
    case OCT_OP_GET_DEVICE_INFO: return device_info(r, transaction, out);
    case OCT_OP_CLOSE_SESSION:
        end_session(r);
        return respond_code(r, out, OCT_RC_OK, transaction);
    case OCT_OP_GET_STORAGE_IDS: return storage_ids(r, transaction, out);
    case OCT_OP_GET_STORAGE_INFO: return storage_info(r, c, out);
    case OCT_OP_GET_NUM_OBJECTS: return object_handles(r, c, out, true);
    case OCT_OP_GET_OBJECT_HANDLES: return object_handles(r, c, out, false);
    case OCT_OP_GET_OBJECT_INFO: return object_info(r, c, out);
    case OCT_OP_GET_OBJECT: return get_object(r, c, out, false);
    case OCT_OP_GET_PARTIAL_OBJECT: return get_object(r, c, out, true);
    case OCT_OP_DELETE_OBJECT: return delete_object(r, c, out);
    case OCT_OP_GET_DEVICE_PROP_DESC: return device_prop(r, c, out, true);
    case OCT_OP_GET_DEVICE_PROP_VALUE: return device_prop(r, c, out, false);
    case OCT_OP_GET_OBJECT_PROPS_SUPPORTED: return object_props_supported(r, c, out);
    case OCT_OP_GET_OBJECT_PROP_DESC: return object_prop_desc(r, c, out);
    case OCT_OP_GET_OBJECT_PROP_VALUE: return object_prop_value(r, c, out);
    case OCT_OP_GET_OBJECT_PROP_LIST: return object_prop_list(r, c, out);
    case OCT_OP_SEND_OBJECT_INFO:
    case OCT_OP_SEND_OBJECT:
    case OCT_OP_SET_OBJECT_PROP_VALUE:
        r->awaiting_data = true;
        r->pending = *c;
        return OCT_OK;
    default: return respond_code(r, out, OCT_RC_OPERATION_NOT_SUPPORTED, transaction);
    }
}

static oct_status data_phase(oct_responder *r, const uint8_t *payload, size_t length, oct_buf *out)
{
    oct_container pending = r->pending;
    r->awaiting_data = false;
    switch (pending.code) {
    case OCT_OP_SEND_OBJECT_INFO: return send_object_info(r, &pending, payload, length, out);
    case OCT_OP_SEND_OBJECT: return send_object(r, &pending, payload, length, out);
    default: return set_object_prop_value(r, &pending, payload, length, out);
    }
}

oct_status oct_responder_handle(oct_responder *r, const uint8_t *bytes, size_t length, oct_buf *out)
{
    if (!r || !out) return OCT_E_INVALID_ARGUMENT;
    if (out->failed) return OCT_E_NO_MEMORY;
    size_t before = out->length;
    r->stats.bytes_in += length;
    oct_container c;
    oct_parse_result parsed = oct_container_parse(bytes, length, &c);
    oct_status status;
    if (parsed != OCT_PARSE_OK) {
        r->stats.malformed++;
        bool readable = parsed != OCT_PARSE_TRUNCATED;
        if (readable && c.type == OCT_CONTAINER_DATA && r->awaiting_data && c.code == r->pending.code &&
            c.transaction_id == r->pending.transaction_id) {
            /* The expected data phase arrived damaged: fail that transaction. */
            r->awaiting_data = false;
            if (r->pending.code == OCT_OP_SEND_OBJECT) abandon_send(r);
            status = respond_code(r, out, OCT_RC_INCOMPLETE_TRANSFER, c.transaction_id);
        } else if (readable && c.type == OCT_CONTAINER_COMMAND) {
            status = respond_code(r, out, OCT_RC_GENERAL_ERROR, c.transaction_id);
        } else {
            status = OCT_OK;
        }
        if (status == OCT_OK) status = OCT_E_MALFORMED;
    } else if (c.type == OCT_CONTAINER_DATA) {
        if (!r->awaiting_data || c.code != r->pending.code || c.transaction_id != r->pending.transaction_id) {
            status = OCT_E_STATE;
        } else {
            status = data_phase(r, c.payload, c.payload_length, out);
        }
    } else {
        if (r->awaiting_data) {
            r->awaiting_data = false;
            r->stats.abandoned++;
            if (r->pending.code == OCT_OP_SEND_OBJECT) abandon_send(r);
        }
        status = command(r, &c, out);
    }
    r->stats.bytes_out += out->length - before;
    return status;
}

oct_status oct_responder_create(oct_store *store, const oct_responder_identity *identity, oct_responder **out)
{
    if (!out) return OCT_E_INVALID_ARGUMENT;
    *out = NULL;
    if (!store) return OCT_E_INVALID_ARGUMENT;
    oct_responder *r = calloc(1, sizeof *r);
    if (!r) return OCT_E_NO_MEMORY;
    r->store = store;
    const char *model = identity && identity->model ? identity->model : "Octopus File Provider";
    oct_copy_utf8(r->manufacturer, sizeof r->manufacturer,
                  identity && identity->manufacturer ? identity->manufacturer : "Octopus");
    oct_copy_utf8(r->model, sizeof r->model, model);
    oct_copy_utf8(r->device_version, sizeof r->device_version,
                  identity && identity->device_version ? identity->device_version : OCT_CORE_VERSION);
    oct_copy_utf8(r->serial_number, sizeof r->serial_number,
                  identity && identity->serial_number ? identity->serial_number : "0");
    oct_copy_utf8(r->friendly_name, sizeof r->friendly_name,
                  identity && identity->friendly_name ? identity->friendly_name : model);
    *out = r;
    return OCT_OK;
}

void oct_responder_destroy(oct_responder *r)
{
    if (!r) return;
    end_session(r);
    free(r);
}

void oct_responder_reset(oct_responder *r)
{
    if (r) end_session(r);
}

bool oct_responder_session(const oct_responder *r, uint32_t *session_id)
{
    if (!r || !r->session_open) return false;
    if (session_id) *session_id = r->session_id;
    return true;
}

bool oct_responder_awaiting_data(const oct_responder *r) { return r && r->awaiting_data; }

void oct_responder_get_stats(const oct_responder *r, oct_responder_stats *out)
{
    if (r && out) *out = r->stats;
}

/* ---- Loopback initiator ---------------------------------------------------------- */

oct_status oct_exchange_run(oct_responder *r, uint16_t operation, uint32_t transaction_id,
                            const uint32_t *params, uint32_t param_count, bool send_data,
                            const void *data, size_t data_length, oct_buf *scratch, oct_exchange *result)
{
    if (!r || !scratch || !result || param_count > OCT_PTP_MAX_PARAMS) return OCT_E_INVALID_ARGUMENT;
    memset(result, 0, sizeof *result);
    oct_buf_clear(scratch);
    oct_buf request;
    oct_buf_init(&request, 0);
    oct_container_command(&request, operation, transaction_id, params, param_count);
    oct_status status = request.failed ? OCT_E_NO_MEMORY : oct_responder_handle(r, request.data, request.length, scratch);
    if (status == OCT_OK && send_data && scratch->length == 0) {
        oct_buf_clear(&request);
        size_t start = oct_container_data_begin(&request, operation, transaction_id);
        oct_buf_bytes(&request, data, data_length);
        oct_container_data_end(&request, start);
        status = request.failed ? OCT_E_NO_MEMORY : oct_responder_handle(r, request.data, request.length, scratch);
    }
    oct_buf_free(&request);
    if (status != OCT_OK) return status;

    oct_reader reader;
    oct_reader_init(&reader, scratch->data, scratch->length);
    if (scratch->length >= OCT_PTP_HEADER_SIZE && scratch->data[4] == OCT_CONTAINER_DATA && scratch->data[5] == 0) {
        uint32_t length = oct_read_u32(&reader);
        (void)oct_read_u16(&reader);
        uint16_t code = oct_read_u16(&reader);
        uint32_t transaction = oct_read_u32(&reader);
        if (length < OCT_PTP_HEADER_SIZE || length > scratch->length || code != operation ||
            transaction != transaction_id) {
            return OCT_E_MALFORMED;
        }
        result->has_data = true;
        result->data = scratch->data + OCT_PTP_HEADER_SIZE;
        result->data_length = length - OCT_PTP_HEADER_SIZE;
        reader.position = length;
    }
    size_t remaining = oct_reader_remaining(&reader);
    uint32_t length = oct_read_u32(&reader);
    uint16_t type = oct_read_u16(&reader);
    result->response_code = oct_read_u16(&reader);
    uint32_t transaction = oct_read_u32(&reader);
    if (reader.failed || length != remaining || length > OCT_PTP_MAX_COMMAND || (length - OCT_PTP_HEADER_SIZE) % 4u ||
        type != OCT_CONTAINER_RESPONSE || transaction != transaction_id) {
        return OCT_E_MALFORMED;
    }
    result->param_count = (length - OCT_PTP_HEADER_SIZE) / 4u;
    for (uint32_t i = 0; i < result->param_count; i++) result->params[i] = oct_read_u32(&reader);
    return OCT_OK;
}
