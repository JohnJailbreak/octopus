#if !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#include "oct_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* In-memory backend: a flat node table; node id = index + 1, 0 = root. */

typedef struct mem_node {
    char    *name;
    uint64_t parent;
    bool     is_directory;
    bool     writable;
    bool     alive;
    uint8_t *data;
    size_t   size;
    size_t   capacity;
    int64_t  created;
    int64_t  modified;
} mem_node;

typedef struct mem_backend {
    mem_node *nodes;
    size_t    count;
    size_t    capacity;
    uint64_t  limit;  /* bytes of file data the storage may hold */
    uint64_t  used;
    bool      root_writable;
} mem_backend;

static const oct_backend_ops memory_ops;

static int64_t now_seconds(void) { return (int64_t)time(NULL); }

static mem_node *node_at(mem_backend *m, uint64_t id)
{
    if (id == 0 || id > m->count) return NULL;
    mem_node *n = &m->nodes[id - 1];
    return n->alive ? n : NULL;
}

static bool is_container(mem_backend *m, uint64_t id)
{
    if (id == 0) return true;
    const mem_node *n = node_at(m, id);
    return n && n->is_directory;
}

static bool container_writable(mem_backend *m, uint64_t id)
{
    if (id == 0) return m->root_writable;
    const mem_node *n = node_at(m, id);
    return n && n->is_directory && n->writable;
}

static bool sibling_exists(const mem_backend *m, uint64_t parent, const char *name, uint64_t except)
{
    for (size_t i = 0; i < m->count; i++) {
        const mem_node *n = &m->nodes[i];
        if (n->alive && n->parent == parent && (uint64_t)(i + 1) != except && strcmp(n->name, name) == 0) {
            return true;
        }
    }
    return false;
}

static void fill_stat(const mem_node *n, oct_node_stat *stat)
{
    stat->is_directory = n->is_directory;
    stat->writable = n->writable;
    stat->size = n->is_directory ? 0 : (uint64_t)n->size;
    stat->created = n->created;
    stat->modified = n->modified;
}

static oct_status append_node(mem_backend *m, uint64_t parent, const char *name, bool is_directory,
                              bool writable, int64_t time, uint64_t *id)
{
    size_t length = strlen(name);
    char *copy = malloc(length + 1);
    if (!copy) return OCT_E_NO_MEMORY;
    memcpy(copy, name, length + 1);
    if (m->count == m->capacity) {
        size_t capacity = m->capacity ? m->capacity * 2 : 16;
        mem_node *grown = m->capacity > SIZE_MAX / 2 / sizeof(mem_node)
                              ? NULL
                              : realloc(m->nodes, capacity * sizeof *grown);
        if (!grown) {
            free(copy);
            return OCT_E_NO_MEMORY;
        }
        m->nodes = grown;
        m->capacity = capacity;
    }
    mem_node *n = &m->nodes[m->count];
    memset(n, 0, sizeof *n);
    n->name = copy;
    n->parent = parent;
    n->is_directory = is_directory;
    n->writable = writable;
    n->alive = true;
    n->created = time;
    n->modified = time;
    m->count++;
    *id = (uint64_t)m->count;
    return OCT_OK;
}

/* Grows (zero-filling) or shrinks a file, keeping the storage accounting exact. */
static oct_status resize(mem_backend *m, mem_node *n, size_t size)
{
    if (size > n->size) {
        uint64_t growth = (uint64_t)(size - n->size);
        if (growth > m->limit - m->used) return OCT_E_NO_SPACE;
        if (size > n->capacity) {
            size_t capacity = n->capacity ? n->capacity : 64;
            while (capacity < size) {
                if (capacity > SIZE_MAX / 2) {
                    capacity = size;
                    break;
                }
                capacity *= 2;
            }
            uint8_t *grown = realloc(n->data, capacity);
            if (!grown) return OCT_E_NO_MEMORY;
            n->data = grown;
            n->capacity = capacity;
        }
        memset(n->data + n->size, 0, size - n->size);
        m->used += growth;
    } else {
        m->used -= (uint64_t)(n->size - size);
    }
    n->size = size;
    return OCT_OK;
}

static oct_status mem_describe(void *ctx, oct_backend_info *info)
{
    mem_backend *m = ctx;
    bool writable = m->root_writable;
    for (size_t i = 0; i < m->count && !writable; i++) {
        writable = m->nodes[i].alive && m->nodes[i].writable;
    }
    info->capacity = m->limit;
    info->available = m->limit - m->used;
    info->root_writable = m->root_writable;
    info->writable = writable;
    info->storage_type = 0x0003u; /* fixed RAM */
    return OCT_OK;
}

static oct_status mem_list(void *ctx, uint64_t directory, oct_node_visitor visit, void *visit_context)
{
    mem_backend *m = ctx;
    if (!is_container(m, directory)) return OCT_E_NOT_DIRECTORY;
    for (size_t i = 0; i < m->count; i++) {
        const mem_node *n = &m->nodes[i];
        if (!n->alive || n->parent != directory) continue;
        oct_node_stat stat;
        fill_stat(n, &stat);
        oct_status status = visit(visit_context, (uint64_t)(i + 1), n->name, &stat);
        if (status != OCT_OK) return status;
    }
    return OCT_OK;
}

static oct_status mem_stat(void *ctx, uint64_t node, oct_node_stat *stat)
{
    mem_backend *m = ctx;
    if (node == 0) {
        memset(stat, 0, sizeof *stat);
        stat->is_directory = true;
        stat->writable = m->root_writable;
        return OCT_OK;
    }
    const mem_node *n = node_at(m, node);
    if (!n) return OCT_E_INVALID_HANDLE;
    fill_stat(n, stat);
    return OCT_OK;
}

static oct_status mem_read(void *ctx, uint64_t node, uint64_t offset, void *buffer, size_t length, size_t *read)
{
    mem_backend *m = ctx;
    *read = 0;
    const mem_node *n = node_at(m, node);
    if (!n) return OCT_E_INVALID_HANDLE;
    if (n->is_directory) return OCT_E_IS_DIRECTORY;
    if (offset > n->size) return OCT_E_OUT_OF_RANGE;
    size_t available = n->size - (size_t)offset;
    size_t count = length < available ? length : available;
    if (count) memcpy(buffer, n->data + offset, count);
    *read = count;
    return OCT_OK;
}

static oct_status mem_create(void *ctx, uint64_t parent, const char *name, bool is_directory,
                             uint64_t *node, oct_node_stat *stat)
{
    mem_backend *m = ctx;
    if (!is_container(m, parent)) return OCT_E_NOT_DIRECTORY;
    if (!container_writable(m, parent)) return OCT_E_WRITE_PROTECTED;
    if (oct_name_validate(name) != OCT_OK) return OCT_E_INVALID_NAME;
    if (sibling_exists(m, parent, name, 0)) return OCT_E_EXISTS;
    uint64_t id = 0;
    oct_status status = append_node(m, parent, name, is_directory, true, now_seconds(), &id);
    if (status != OCT_OK) return status;
    fill_stat(&m->nodes[id - 1], stat);
    *node = id;
    return OCT_OK;
}

static oct_status mem_write(void *ctx, uint64_t node, uint64_t offset, const void *data, size_t length)
{
    mem_backend *m = ctx;
    mem_node *n = node_at(m, node);
    if (!n) return OCT_E_INVALID_HANDLE;
    if (n->is_directory) return OCT_E_IS_DIRECTORY;
    if (!n->writable) return OCT_E_WRITE_PROTECTED;
    if (offset > (uint64_t)(SIZE_MAX - length)) return OCT_E_TOO_LARGE;
    size_t end = (size_t)offset + length;
    if (end > n->size) {
        oct_status status = resize(m, n, end);
        if (status != OCT_OK) return status;
    }
    if (length) memcpy(n->data + offset, data, length);
    n->modified = now_seconds();
    return OCT_OK;
}

static oct_status mem_truncate(void *ctx, uint64_t node, uint64_t size)
{
    mem_backend *m = ctx;
    mem_node *n = node_at(m, node);
    if (!n) return OCT_E_INVALID_HANDLE;
    if (n->is_directory) return OCT_E_IS_DIRECTORY;
    if (!n->writable) return OCT_E_WRITE_PROTECTED;
    if (size > SIZE_MAX) return OCT_E_TOO_LARGE;
    oct_status status = resize(m, n, (size_t)size);
    if (status == OCT_OK) n->modified = now_seconds();
    return status;
}

static oct_status mem_remove(void *ctx, uint64_t node)
{
    mem_backend *m = ctx;
    mem_node *n = node_at(m, node);
    if (!n) return OCT_E_INVALID_HANDLE;
    if (!n->writable) return OCT_E_WRITE_PROTECTED;
    if (n->is_directory) {
        for (size_t i = 0; i < m->count; i++) {
            if (m->nodes[i].alive && m->nodes[i].parent == node) return OCT_E_STATE; /* not empty */
        }
    }
    m->used -= (uint64_t)n->size;
    free(n->name);
    free(n->data);
    memset(n, 0, sizeof *n);
    return OCT_OK;
}

static oct_status mem_rename(void *ctx, uint64_t node, const char *name)
{
    mem_backend *m = ctx;
    mem_node *n = node_at(m, node);
    if (!n) return OCT_E_INVALID_HANDLE;
    if (!n->writable) return OCT_E_WRITE_PROTECTED;
    if (oct_name_validate(name) != OCT_OK) return OCT_E_INVALID_NAME;
    if (sibling_exists(m, n->parent, name, node)) return OCT_E_EXISTS;
    size_t length = strlen(name);
    char *copy = malloc(length + 1);
    if (!copy) return OCT_E_NO_MEMORY;
    memcpy(copy, name, length + 1);
    free(n->name);
    n->name = copy;
    return OCT_OK;
}

static void mem_destroy(void *ctx)
{
    mem_backend *m = ctx;
    if (!m) return;
    for (size_t i = 0; i < m->count; i++) {
        free(m->nodes[i].name);
        free(m->nodes[i].data);
    }
    free(m->nodes);
    free(m);
}

static const oct_backend_ops memory_ops = {
    "memory", mem_describe, mem_list, mem_stat, mem_read, mem_create,
    mem_write, mem_truncate, mem_remove, mem_rename, mem_destroy,
};

oct_status oct_memory_backend_create(uint64_t capacity, oct_backend *out)
{
    if (!out) return OCT_E_INVALID_ARGUMENT;
    out->ops = NULL;
    out->ctx = NULL;
    mem_backend *m = calloc(1, sizeof *m);
    if (!m) return OCT_E_NO_MEMORY;
    m->limit = capacity;
    out->ops = &memory_ops;
    out->ctx = m;
    return OCT_OK;
}

oct_status oct_memory_backend_add(oct_backend *backend, uint64_t parent, const char *name,
                                  bool is_directory, const void *data, size_t length,
                                  bool writable, int64_t time, uint64_t *node)
{
    if (!backend || backend->ops != &memory_ops || !backend->ctx || (length && !data) ||
        (is_directory && length)) {
        return OCT_E_INVALID_ARGUMENT;
    }
    mem_backend *m = backend->ctx;
    if (!is_container(m, parent)) return OCT_E_NOT_DIRECTORY;
    if (oct_name_validate(name) != OCT_OK) return OCT_E_INVALID_NAME;
    if (sibling_exists(m, parent, name, 0)) return OCT_E_EXISTS;
    if ((uint64_t)length > m->limit - m->used) return OCT_E_NO_SPACE;
    uint8_t *copy = NULL;
    if (length) {
        copy = malloc(length);
        if (!copy) return OCT_E_NO_MEMORY;
        memcpy(copy, data, length);
    }
    uint64_t id = 0;
    oct_status status = append_node(m, parent, name, is_directory, writable, time, &id);
    if (status != OCT_OK) {
        free(copy);
        return status;
    }
    mem_node *n = &m->nodes[id - 1];
    n->data = copy;
    n->size = length;
    n->capacity = length;
    m->used += (uint64_t)length;
    if (node) *node = id;
    return OCT_OK;
}

void oct_memory_backend_set_root_writable(oct_backend *backend, bool writable)
{
    if (backend && backend->ops == &memory_ops && backend->ctx) {
        ((mem_backend *)backend->ctx)->root_writable = writable;
    }
}

/* ---- The Octopus synthetic hierarchy --------------------------------------- */

#define SYNTHETIC_CAPACITY (16u * 1024u * 1024u)

uint8_t oct_sample_byte(size_t offset)
{
    return (uint8_t)((offset ^ ((offset >> 8) * 0x5Bu)) & 0xFFu);
}

static const char readme_text[] =
    "Octopus synthetic storage\n"
    "\n"
    "Every object in this storage is generated in memory by the Octopus\n"
    "file-object provider on the iPhone. Nothing here is read from or\n"
    "written to the phone's own filesystem.\n"
    "\n"
    "Documents/example.txt    a short text file\n"
    "Documents/sample.bin     4096 bytes of a fixed binary pattern\n"
    "Information/version.txt  provider and protocol versions\n";

static const char example_text[] =
    "Hello from Octopus.\n"
    "This text file lives in the synthetic Documents folder.\n";

oct_status oct_synthetic_backend_create(const oct_synthetic_options *options, oct_backend *out)
{
    int64_t time = options && options->time ? options->time : now_seconds();
    oct_status status = oct_memory_backend_create(SYNTHETIC_CAPACITY, out);
    if (status != OCT_OK) return status;

    uint8_t sample[OCT_SAMPLE_BIN_SIZE];
    for (size_t i = 0; i < sizeof sample; i++) sample[i] = oct_sample_byte(i);
    char version[256];
    int written = snprintf(version, sizeof version,
                           "Octopus file-object provider %s\n"
                           "Datasets: PTP 1.0 (PIMA 15740) with MTP 1.0 object properties\n"
                           "Storage: synthetic, generated in memory\n",
                           OCT_CORE_VERSION);
    size_t version_length = written > 0 && (size_t)written < sizeof version ? (size_t)written : 0;

    uint64_t documents = 0, information = 0;
    status = oct_memory_backend_add(out, 0, "README.txt", false, readme_text, sizeof readme_text - 1, false, time, NULL);
    if (status == OCT_OK) status = oct_memory_backend_add(out, 0, "Documents", true, NULL, 0, false, time, &documents);
    if (status == OCT_OK) status = oct_memory_backend_add(out, documents, "example.txt", false, example_text, sizeof example_text - 1, false, time, NULL);
    if (status == OCT_OK) status = oct_memory_backend_add(out, documents, "sample.bin", false, sample, sizeof sample, false, time, NULL);
    if (status == OCT_OK) status = oct_memory_backend_add(out, 0, "Information", true, NULL, 0, false, time, &information);
    if (status == OCT_OK) status = oct_memory_backend_add(out, information, "version.txt", false, version, version_length, false, time, NULL);
    if (status == OCT_OK && options && options->scratch) {
        status = oct_memory_backend_add(out, 0, "Scratch", true, NULL, 0, true, time, NULL);
    }
    if (status != OCT_OK) {
        mem_destroy(out->ctx);
        out->ops = NULL;
        out->ctx = NULL;
    }
    return status;
}
