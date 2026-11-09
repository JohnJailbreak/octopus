#if !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "oct_store.h"

#include <stdlib.h>
#include <string.h>

void oct_handle_list_free(oct_handle_list *list)
{
    if (!list) return;
    free(list->handles);
    list->handles = NULL;
    list->count = 0;
    list->capacity = 0;
}

static oct_status handle_list_push(oct_handle_list *list, uint32_t handle)
{
    if (list->count == list->capacity) {
        if (list->capacity > UINT32_MAX / 2u) return OCT_E_NO_MEMORY;
        uint32_t capacity = list->capacity ? list->capacity * 2u : 16u;
        uint32_t *grown = realloc(list->handles, (size_t)capacity * sizeof *grown);
        if (!grown) return OCT_E_NO_MEMORY;
        list->handles = grown;
        list->capacity = capacity;
    }
    list->handles[list->count++] = handle;
    return OCT_OK;
}

/* ---- UTF-8 and names ------------------------------------------------------ */

size_t oct_utf8_next(const char *text, size_t length, size_t position, uint32_t *codepoint)
{
    if (!text || position >= length) return 0;
    const unsigned char *p = (const unsigned char *)text + position;
    size_t available = length - position;
    uint32_t value;
    size_t width;
    if (p[0] < 0x80u) {
        value = p[0];
        width = 1;
    } else if ((p[0] & 0xE0u) == 0xC0u) {
        value = p[0] & 0x1Fu;
        width = 2;
    } else if ((p[0] & 0xF0u) == 0xE0u) {
        value = p[0] & 0x0Fu;
        width = 3;
    } else if ((p[0] & 0xF8u) == 0xF0u) {
        value = p[0] & 0x07u;
        width = 4;
    } else {
        return 0;
    }
    if (width > available) return 0;
    for (size_t i = 1; i < width; i++) {
        if ((p[i] & 0xC0u) != 0x80u) return 0;
        value = (value << 6) | (p[i] & 0x3Fu);
    }
    if ((width == 2 && value < 0x80u) || (width == 3 && value < 0x800u) ||
        (width == 4 && value < 0x10000u) || value > 0x10FFFFu ||
        (value >= 0xD800u && value <= 0xDFFFu)) {
        return 0;
    }
    if (codepoint) *codepoint = value;
    return width;
}

long oct_utf8_utf16_length(const char *utf8, size_t length)
{
    long units = 0;
    size_t position = 0;
    while (position < length) {
        uint32_t codepoint = 0;
        size_t width = oct_utf8_next(utf8, length, position, &codepoint);
        if (width == 0) return -1;
        units += codepoint >= 0x10000u ? 2 : 1;
        position += width;
    }
    return units;
}

oct_status oct_name_validate(const char *name)
{
    if (!name) return OCT_E_INVALID_NAME;
    size_t length = strnlen(name, OCT_NAME_MAX + 1);
    if (length == 0 || length > OCT_NAME_MAX) return OCT_E_INVALID_NAME;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return OCT_E_INVALID_NAME;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20u || c == 0x7Fu || c == '/') return OCT_E_INVALID_NAME;
    }
    long units = oct_utf8_utf16_length(name, length);
    if (units < 1 || units > OCT_NAME_UTF16_MAX) return OCT_E_INVALID_NAME;
    return OCT_OK;
}

static bool ascii_equal_ignoring_case(const char *a, const char *b)
{
    for (;; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return false;
        if (x == '\0') return true;
    }
}

uint16_t oct_format_for_name(const char *name, bool is_directory)
{
    if (is_directory) return OCT_FORMAT_ASSOCIATION;
    const char *dot = name ? strrchr(name, '.') : NULL;
    if (dot && dot != name &&
        (ascii_equal_ignoring_case(dot + 1, "txt") || ascii_equal_ignoring_case(dot + 1, "text"))) {
        return OCT_FORMAT_TEXT;
    }
    return OCT_FORMAT_UNDEFINED;
}

void oct_copy_utf8(char *dst, size_t capacity, const char *src)
{
    size_t length = strlen(src), used = 0;
    while (used < length) {
        size_t width = oct_utf8_next(src, length, used, NULL);
        if (width == 0 || used + width > capacity - 1) break;
        used += width;
    }
    memcpy(dst, src, used);
    dst[used] = '\0';
}

/* ---- Store ------------------------------------------------------------------ */

typedef struct object {
    uint32_t  handle;
    uint32_t  parent;
    uint64_t  node;
    uint16_t  format;
    bool      is_directory;
    bool      writable;
    bool      loaded;
    uint64_t  size;
    int64_t   created;
    int64_t   modified;
    char     *name;
    uint32_t *children;
    uint32_t  child_count;
    uint32_t  child_capacity;
} object;

struct oct_store {
    oct_backend backend;
    uint32_t    storage_id;
    uint32_t    first_handle;
    uint32_t    live;
    object    **table;          /* index = handle - first_handle */
    uint32_t    table_length;
    uint32_t    table_capacity;
    object      root;
    char        description[OCT_LABEL_MAX + 1];
    char        volume_label[OCT_LABEL_MAX + 1];
};

static object *lookup(const oct_store *store, uint32_t handle)
{
    if (handle < store->first_handle) return NULL;
    uint32_t index = handle - store->first_handle;
    if (index >= store->table_length) return NULL;
    return store->table[index];
}

static object *lookup_container(oct_store *store, uint32_t handle)
{
    return handle == OCT_ROOT ? &store->root : lookup(store, handle);
}

static object *child_named(const oct_store *store, const object *dir, const char *name)
{
    for (uint32_t i = 0; i < dir->child_count; i++) {
        object *child = lookup(store, dir->children[i]);
        if (child && strcmp(child->name, name) == 0) return child;
    }
    return NULL;
}

static oct_status add_object(oct_store *store, object *parent, uint64_t node,
                             const char *name, const oct_node_stat *stat, object **out)
{
    if (store->first_handle + store->table_length >= UINT32_MAX) return OCT_E_NO_SPACE;
    if (store->table_length == store->table_capacity) {
        if (store->table_capacity > UINT32_MAX / 2u) return OCT_E_NO_MEMORY;
        uint32_t capacity = store->table_capacity ? store->table_capacity * 2u : 32u;
        object **grown = realloc(store->table, (size_t)capacity * sizeof *grown);
        if (!grown) return OCT_E_NO_MEMORY;
        store->table = grown;
        store->table_capacity = capacity;
    }
    if (parent->child_count == parent->child_capacity) {
        if (parent->child_capacity > UINT32_MAX / 2u) return OCT_E_NO_MEMORY;
        uint32_t capacity = parent->child_capacity ? parent->child_capacity * 2u : 8u;
        uint32_t *grown = realloc(parent->children, (size_t)capacity * sizeof *grown);
        if (!grown) return OCT_E_NO_MEMORY;
        parent->children = grown;
        parent->child_capacity = capacity;
    }
    object *o = calloc(1, sizeof *o);
    if (!o) return OCT_E_NO_MEMORY;
    size_t length = strlen(name);
    o->name = malloc(length + 1);
    if (!o->name) {
        free(o);
        return OCT_E_NO_MEMORY;
    }
    memcpy(o->name, name, length + 1);
    o->handle = store->first_handle + store->table_length;
    o->parent = parent->handle;
    o->node = node;
    o->is_directory = stat->is_directory;
    o->writable = stat->writable;
    o->format = oct_format_for_name(name, stat->is_directory);
    o->size = stat->is_directory ? 0 : stat->size;
    o->created = stat->created;
    o->modified = stat->modified;
    store->table[store->table_length++] = o;
    parent->children[parent->child_count++] = o->handle;
    store->live++;
    *out = o;
    return OCT_OK;
}

static void free_object(oct_store *store, object *o)
{
    store->table[o->handle - store->first_handle] = NULL;
    free(o->name);
    free(o->children);
    free(o);
    store->live--;
}

static void detach(object *parent, uint32_t handle)
{
    for (uint32_t i = 0; i < parent->child_count; i++) {
        if (parent->children[i] == handle) {
            memmove(&parent->children[i], &parent->children[i + 1],
                    (size_t)(parent->child_count - i - 1) * sizeof parent->children[0]);
            parent->child_count--;
            return;
        }
    }
}

typedef struct load_context {
    oct_store *store;
    object    *directory;
} load_context;

static oct_status visit_child(void *context, uint64_t node, const char *name,
                              const oct_node_stat *stat)
{
    load_context *load = context;
    /* Unrepresentable names and duplicate entries are not exported. */
    if (node == 0 || !stat || oct_name_validate(name) != OCT_OK ||
        child_named(load->store, load->directory, name)) {
        return OCT_OK;
    }
    object *created;
    return add_object(load->store, load->directory, node, name, stat, &created);
}

static oct_status load_children(oct_store *store, object *dir)
{
    if (dir->loaded) return OCT_OK;
    uint32_t before = dir->child_count;
    load_context load = { store, dir };
    oct_status status = store->backend.ops->list(store->backend.ctx, dir->node, visit_child, &load);
    if (status != OCT_OK) {
        while (dir->child_count > before) {
            object *child = lookup(store, dir->children[dir->child_count - 1]);
            dir->child_count--;
            if (child) free_object(store, child);
        }
        return status;
    }
    dir->loaded = true;
    return OCT_OK;
}

static void refresh(oct_store *store, object *o)
{
    oct_node_stat stat;
    if (store->backend.ops->stat(store->backend.ctx, o->node, &stat) == OCT_OK) {
        o->size = stat.is_directory ? 0 : stat.size;
        o->modified = stat.modified;
        o->writable = stat.writable;
    }
}

oct_status oct_store_open(const oct_store_options *options, oct_backend backend, oct_store **out)
{
    if (out) *out = NULL;
    const oct_backend_ops *ops = backend.ops;
    bool complete = ops && ops->describe && ops->list && ops->stat && ops->read && ops->create &&
                    ops->write && ops->truncate && ops->remove && ops->rename && ops->destroy;
    const char *description = options && options->description ? options->description : "Octopus";
    const char *label = options && options->volume_label ? options->volume_label : description;
    uint32_t first = options && options->first_handle ? options->first_handle : 1u;
    if (!out || !complete || first == UINT32_MAX ||
        oct_utf8_utf16_length(description, strlen(description)) < 0 ||
        oct_utf8_utf16_length(label, strlen(label)) < 0) {
        if (ops && ops->destroy) ops->destroy(backend.ctx);
        return OCT_E_INVALID_ARGUMENT;
    }
    oct_backend_info info;
    oct_status status = ops->describe(backend.ctx, &info);
    oct_store *store = status == OCT_OK ? calloc(1, sizeof *store) : NULL;
    if (!store) {
        ops->destroy(backend.ctx);
        return status == OCT_OK ? OCT_E_NO_MEMORY : status;
    }
    store->backend = backend;
    store->storage_id = options && options->storage_id ? options->storage_id : OCT_DEFAULT_STORAGE_ID;
    store->first_handle = first;
    store->root.handle = OCT_ROOT;
    store->root.parent = OCT_ROOT;
    store->root.node = 0;
    store->root.is_directory = true;
    store->root.writable = info.root_writable;
    store->root.format = OCT_FORMAT_ASSOCIATION;
    oct_copy_utf8(store->description, sizeof store->description, description);
    oct_copy_utf8(store->volume_label, sizeof store->volume_label, label);
    *out = store;
    return OCT_OK;
}

void oct_store_close(oct_store *store)
{
    if (!store) return;
    for (uint32_t i = 0; i < store->table_length; i++) {
        object *o = store->table[i];
        if (o) {
            free(o->name);
            free(o->children);
            free(o);
        }
    }
    free(store->table);
    free(store->root.children);
    store->backend.ops->destroy(store->backend.ctx);
    free(store);
}

uint32_t oct_store_storage_id(const oct_store *store) { return store ? store->storage_id : 0; }
uint32_t oct_store_object_count(const oct_store *store) { return store ? store->live : 0; }

const char *oct_store_backend_kind(const oct_store *store)
{
    return store && store->backend.ops->kind ? store->backend.ops->kind : "unknown";
}

oct_status oct_store_storage_info(oct_store *store, oct_storage_info *out)
{
    if (!store || !out) return OCT_E_INVALID_ARGUMENT;
    oct_backend_info info;
    oct_status status = store->backend.ops->describe(store->backend.ctx, &info);
    if (status != OCT_OK) return status;
    memset(out, 0, sizeof *out);
    out->storage_id = store->storage_id;
    out->storage_type = info.storage_type;
    out->filesystem_type = 0x0002u;
    out->access_capability = (uint16_t)(info.writable ? 0x0000u : 0x0001u);
    out->max_capacity = info.capacity;
    out->free_space = info.available < info.capacity ? info.available : info.capacity;
    out->object_count = store->live;
    memcpy(out->description, store->description, sizeof out->description);
    memcpy(out->volume_label, store->volume_label, sizeof out->volume_label);
    return OCT_OK;
}

oct_status oct_store_index_all(oct_store *store)
{
    oct_handle_list list = { 0 };
    oct_status status = oct_store_list(store, OCT_ROOT, true, &list);
    oct_handle_list_free(&list);
    return status;
}

oct_status oct_store_get(oct_store *store, uint32_t handle, oct_object_info *out)
{
    if (!store || !out) return OCT_E_INVALID_ARGUMENT;
    const object *o = lookup(store, handle);
    if (!o) return OCT_E_INVALID_HANDLE;
    memset(out, 0, sizeof *out);
    out->handle = o->handle;
    out->parent = o->parent;
    out->storage_id = store->storage_id;
    out->format = o->format;
    out->is_directory = o->is_directory;
    out->write_protected = !o->writable;
    out->size = o->is_directory ? 0 : o->size;
    out->created = o->created;
    out->modified = o->modified;
    memcpy(out->name, o->name, strlen(o->name) + 1);
    return OCT_OK;
}

oct_status oct_store_find(oct_store *store, uint32_t parent, const char *name, uint32_t *handle)
{
    if (!store || !name || !handle) return OCT_E_INVALID_ARGUMENT;
    object *dir = lookup_container(store, parent);
    if (!dir) return OCT_E_INVALID_HANDLE;
    if (!dir->is_directory) return OCT_E_NOT_DIRECTORY;
    oct_status status = load_children(store, dir);
    if (status != OCT_OK) return status;
    object *child = child_named(store, dir, name);
    if (!child) return OCT_E_INVALID_HANDLE;
    *handle = child->handle;
    return OCT_OK;
}

oct_status oct_store_list(oct_store *store, uint32_t parent, bool recursive, oct_handle_list *out)
{
    if (!store || !out) return OCT_E_INVALID_ARGUMENT;
    out->count = 0;
    object *dir = lookup_container(store, parent);
    if (!dir) return OCT_E_INVALID_HANDLE;
    if (!dir->is_directory) return OCT_E_NOT_DIRECTORY;
    oct_status status = load_children(store, dir);
    for (uint32_t i = 0; status == OCT_OK && i < dir->child_count; i++) {
        status = handle_list_push(out, dir->children[i]);
    }
    /* Breadth-first expansion; the list itself is the queue. */
    for (uint32_t i = 0; recursive && status == OCT_OK && i < out->count; i++) {
        object *o = lookup(store, out->handles[i]);
        if (!o || !o->is_directory) continue;
        status = load_children(store, o);
        for (uint32_t k = 0; status == OCT_OK && k < o->child_count; k++) {
            status = handle_list_push(out, o->children[k]);
        }
    }
    if (status != OCT_OK) out->count = 0;
    return status;
}

oct_status oct_store_read(oct_store *store, uint32_t handle, uint64_t offset,
                          void *buffer, size_t length, size_t *read)
{
    if (read) *read = 0;
    if (!store || !read || (!buffer && length)) return OCT_E_INVALID_ARGUMENT;
    const object *o = lookup(store, handle);
    if (!o) return OCT_E_INVALID_HANDLE;
    if (o->is_directory) return OCT_E_IS_DIRECTORY;
    if (offset > o->size) return OCT_E_OUT_OF_RANGE;
    uint64_t remaining = o->size - offset;
    size_t wanted = (uint64_t)length > remaining ? (size_t)remaining : length;
    if (wanted == 0) return OCT_OK;
    return store->backend.ops->read(store->backend.ctx, o->node, offset, buffer, wanted, read);
}

oct_status oct_store_create(oct_store *store, uint32_t parent, const char *name,
                            bool is_directory, uint32_t *handle)
{
    if (!store || !handle) return OCT_E_INVALID_ARGUMENT;
    object *dir = lookup_container(store, parent);
    if (!dir) return OCT_E_INVALID_PARENT;
    if (!dir->is_directory) return OCT_E_NOT_DIRECTORY;
    if (!dir->writable) return OCT_E_WRITE_PROTECTED;
    oct_status status = oct_name_validate(name);
    if (status != OCT_OK) return status;
    status = load_children(store, dir);
    if (status != OCT_OK) return status;
    if (child_named(store, dir, name)) return OCT_E_EXISTS;
    uint64_t node = 0;
    oct_node_stat stat;
    status = store->backend.ops->create(store->backend.ctx, dir->node, name, is_directory, &node, &stat);
    if (status != OCT_OK) return status;
    object *o = NULL;
    status = add_object(store, dir, node, name, &stat, &o);
    if (status != OCT_OK) {
        store->backend.ops->remove(store->backend.ctx, node);
        return status;
    }
    o->loaded = is_directory; /* a new directory starts empty */
    *handle = o->handle;
    return OCT_OK;
}

static oct_status writable_file(oct_store *store, uint32_t handle, object **out)
{
    object *o = lookup(store, handle);
    if (!o) return OCT_E_INVALID_HANDLE;
    if (o->is_directory) return OCT_E_IS_DIRECTORY;
    if (!o->writable) return OCT_E_WRITE_PROTECTED;
    *out = o;
    return OCT_OK;
}

oct_status oct_store_write(oct_store *store, uint32_t handle, uint64_t offset,
                           const void *data, size_t length)
{
    if (!store || (!data && length)) return OCT_E_INVALID_ARGUMENT;
    object *o = NULL;
    oct_status status = writable_file(store, handle, &o);
    if (status != OCT_OK || length == 0) return status;
    if (offset > UINT64_MAX - length) return OCT_E_OUT_OF_RANGE;
    status = store->backend.ops->write(store->backend.ctx, o->node, offset, data, length);
    refresh(store, o);
    return status;
}

oct_status oct_store_truncate(oct_store *store, uint32_t handle, uint64_t size)
{
    if (!store) return OCT_E_INVALID_ARGUMENT;
    object *o = NULL;
    oct_status status = writable_file(store, handle, &o);
    if (status != OCT_OK) return status;
    status = store->backend.ops->truncate(store->backend.ctx, o->node, size);
    refresh(store, o);
    return status;
}

static oct_status delete_object(oct_store *store, object *o, uint32_t *deleted)
{
    if (!o->writable) return OCT_E_WRITE_PROTECTED;
    if (o->is_directory) {
        oct_status status = load_children(store, o);
        if (status != OCT_OK) return status;
        bool incomplete = false;
        for (uint32_t i = o->child_count; i > 0; i--) {
            object *child = lookup(store, o->children[i - 1]);
            if (child && delete_object(store, child, deleted) != OCT_OK) incomplete = true;
        }
        if (incomplete) return OCT_E_PARTIAL_DELETION;
    }
    oct_status status = store->backend.ops->remove(store->backend.ctx, o->node);
    if (status != OCT_OK) return status;
    object *parent = lookup_container(store, o->parent);
    if (parent) detach(parent, o->handle);
    free_object(store, o);
    (*deleted)++;
    return OCT_OK;
}

oct_status oct_store_delete(oct_store *store, uint32_t handle, uint32_t *deleted)
{
    uint32_t count = 0;
    if (deleted) *deleted = 0;
    if (!store) return OCT_E_INVALID_ARGUMENT;
    object *o = lookup(store, handle);
    if (!o) return OCT_E_INVALID_HANDLE;
    oct_status status = delete_object(store, o, &count);
    if (deleted) *deleted = count;
    if (status != OCT_OK && count > 0) return OCT_E_PARTIAL_DELETION;
    return status;
}

oct_status oct_store_rename(oct_store *store, uint32_t handle, const char *name)
{
    if (!store) return OCT_E_INVALID_ARGUMENT;
    object *o = lookup(store, handle);
    if (!o) return OCT_E_INVALID_HANDLE;
    if (!o->writable) return OCT_E_WRITE_PROTECTED;
    oct_status status = oct_name_validate(name);
    if (status != OCT_OK) return status;
    if (strcmp(o->name, name) == 0) return OCT_OK;
    object *parent = lookup_container(store, o->parent);
    if (!parent) return OCT_E_INVALID_PARENT;
    if (child_named(store, parent, name)) return OCT_E_EXISTS;
    size_t length = strlen(name);
    char *copy = malloc(length + 1);
    if (!copy) return OCT_E_NO_MEMORY;
    memcpy(copy, name, length + 1);
    status = store->backend.ops->rename(store->backend.ctx, o->node, name);
    if (status != OCT_OK) {
        free(copy);
        return status;
    }
    free(o->name);
    o->name = copy;
    refresh(store, o);
    return OCT_OK;
}
