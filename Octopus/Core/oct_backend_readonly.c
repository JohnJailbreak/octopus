#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#else
#define _POSIX_C_SOURCE 200809L
#endif
#include "oct_readonly.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#define RO_DEPTH 256u
#define RO_BYTES (16u * 1024u * 1024u)
#define RO_CHUNK 65536u

typedef struct ro_node {
    char *name;
    uint32_t parent;
    uint32_t depth;
    struct stat identity;
} ro_node;

typedef struct ro_backend {
    int root_fd;
    oct_readonly_options options;
    ro_node *nodes; /* root is index 0; node IDs are never reused */
    uint32_t count;
    size_t name_bytes;
} ro_backend;

typedef struct ro_entry {
    char *name;
    struct stat identity;
} ro_entry;

static oct_status error_status(int error)
{
    switch (error) {
    case ENOENT: case ELOOP: case ENOTDIR: return OCT_E_INVALID_HANDLE;
    case EACCES: case EPERM: return OCT_E_WRITE_PROTECTED;
    case ENOMEM: return OCT_E_NO_MEMORY;
    case ENAMETOOLONG: return OCT_E_INVALID_NAME;
    default: return OCT_E_IO;
    }
}

static bool allowed(const ro_backend *b)
{
    return b->options.authorized(b->options.authorization_context);
}

static bool exportable(const struct stat *s)
{
    return S_ISDIR(s->st_mode) || (S_ISREG(s->st_mode) && s->st_size >= 0);
}

static bool same(const struct stat *a, const struct stat *b)
{
    if (a->st_dev != b->st_dev || a->st_ino != b->st_ino ||
        S_ISDIR(a->st_mode) != S_ISDIR(b->st_mode) || !exportable(b)) return false;
    if (S_ISDIR(a->st_mode)) return true;
#if defined(__APPLE__)
    return a->st_size == b->st_size && a->st_mtimespec.tv_sec == b->st_mtimespec.tv_sec &&
           a->st_mtimespec.tv_nsec == b->st_mtimespec.tv_nsec && a->st_ctimespec.tv_sec == b->st_ctimespec.tv_sec &&
           a->st_ctimespec.tv_nsec == b->st_ctimespec.tv_nsec;
#else
    return a->st_size == b->st_size && a->st_mtim.tv_sec == b->st_mtim.tv_sec &&
           a->st_mtim.tv_nsec == b->st_mtim.tv_nsec && a->st_ctim.tv_sec == b->st_ctim.tv_sec &&
           a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
#endif
}

static void convert(const struct stat *s, oct_node_stat *out)
{
    memset(out, 0, sizeof *out);
    out->is_directory = S_ISDIR(s->st_mode);
    out->size = out->is_directory ? 0 : (uint64_t)s->st_size;
    out->modified = (int64_t)s->st_mtime;
#if defined(__APPLE__)
    out->created = (int64_t)s->st_birthtimespec.tv_sec;
#else
    out->created = (int64_t)s->st_mtime;
#endif
    out->writable = false;
}

/* Resolve from the pinned root descriptor, checking the identity of EVERY
 * ancestor. No path strings, symlink resolution, or ambient working directory. */
static oct_status open_directory(ro_backend *b, uint64_t node, int *out)
{
    *out = -1;
    if (!allowed(b)) return OCT_E_STATE;
    if (node > b->count) return OCT_E_INVALID_HANDLE;
    if (!S_ISDIR(b->nodes[node].identity.st_mode)) return OCT_E_NOT_DIRECTORY;
    uint32_t chain[RO_DEPTH];
    uint32_t count = 0, at = (uint32_t)node;
    while (at) {
        if (count == RO_DEPTH) return OCT_E_TOO_LARGE;
        chain[count++] = at;
        at = b->nodes[at].parent;
    }
    int fd = openat(b->root_fd, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return error_status(errno);
    for (uint32_t i = count; i > 0; i--) {
        if (!allowed(b)) { close(fd); return OCT_E_STATE; }
        ro_node *n = &b->nodes[chain[i - 1]];
        int next = openat(fd, n->name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        int saved = errno;
        close(fd);
        if (next < 0) return error_status(saved);
        struct stat st;
        if (fstat(next, &st) < 0 || !same(&n->identity, &st)) { close(next); return OCT_E_INVALID_HANDLE; }
        fd = next;
    }
    if (!allowed(b)) { close(fd); return OCT_E_STATE; }
    *out = fd;
    return OCT_OK;
}

static oct_status ro_stat(void *context, uint64_t node, oct_node_stat *out)
{
    ro_backend *b = context;
    if (!out) return OCT_E_INVALID_ARGUMENT;
    memset(out, 0, sizeof *out);
    if (!allowed(b)) return OCT_E_STATE;
    if (node > b->count) return OCT_E_INVALID_HANDLE;
    int fd;
    ro_node *n = &b->nodes[node];
    oct_status status = open_directory(b, node ? n->parent : 0, &fd);
    if (status != OCT_OK) return status;
    struct stat st;
    int result = node ? fstatat(fd, n->name, &st, AT_SYMLINK_NOFOLLOW) : fstat(fd, &st);
    int saved = errno;
    close(fd);
    if (result < 0) return error_status(saved);
    if (!same(&n->identity, &st)) return OCT_E_INVALID_HANDLE;
    if (!allowed(b)) return OCT_E_STATE;
    convert(&st, out);
    return OCT_OK;
}

static oct_status ro_access(void *context, uint64_t node, bool recursive)
{
    ro_backend *b = context;
    if (!allowed(b)) return OCT_E_STATE;
    if (recursive) return OCT_E_UNSUPPORTED;
    oct_node_stat stat;
    return ro_stat(context, node, &stat);
}

static uint64_t multiply_clamped(uint64_t a, uint64_t b)
{
    return b && a > UINT64_MAX / b ? UINT64_MAX : a * b;
}

static oct_status ro_describe(void *context, oct_backend_info *out)
{
    ro_backend *b = context;
    if (!out) return OCT_E_INVALID_ARGUMENT;
    memset(out, 0, sizeof *out);
    if (!allowed(b)) return OCT_E_STATE;
    struct statvfs fs;
    if (fstatvfs(b->root_fd, &fs) < 0) return error_status(errno);
    if (!allowed(b)) return OCT_E_STATE;
    out->capacity = multiply_clamped((uint64_t)fs.f_blocks, (uint64_t)fs.f_frsize);
    out->available = multiply_clamped((uint64_t)fs.f_bavail, (uint64_t)fs.f_frsize);
    out->storage_type = 0x0003;
    return OCT_OK;
}

static int compare_entries(const void *a, const void *b)
{
    return strcmp(((const ro_entry *)a)->name, ((const ro_entry *)b)->name);
}

static oct_status intern(ro_backend *b, uint32_t parent, const ro_entry *entry, uint64_t *out)
{
    for (uint32_t i = 1; i <= b->count; i++) {
        ro_node *n = &b->nodes[i];
        if (n->parent == parent && !strcmp(n->name, entry->name) && same(&n->identity, &entry->identity)) {
            *out = i;
            return OCT_OK;
        }
    }
    size_t bytes = strlen(entry->name) + 1;
    if (b->count == b->options.max_nodes || bytes > b->options.max_name_bytes - b->name_bytes)
        return OCT_E_TOO_LARGE;
    char *name = strdup(entry->name);
    if (!name) return OCT_E_NO_MEMORY;
    ro_node *n = &b->nodes[++b->count];
    n->name = name;
    n->parent = parent;
    n->depth = b->nodes[parent].depth + 1;
    n->identity = entry->identity;
    b->name_bytes += bytes;
    *out = b->count;
    return OCT_OK;
}

static oct_status ro_list(void *context, uint64_t directory, oct_node_visitor visit, void *visit_context)
{
    ro_backend *b = context;
    if (!visit) return OCT_E_INVALID_ARGUMENT;
    int fd;
    oct_status status = open_directory(b, directory, &fd);
    if (status != OCT_OK) return status;
    if (b->nodes[directory].depth >= b->options.max_depth) { close(fd); return OCT_E_TOO_LARGE; }
    DIR *stream = fdopendir(fd);
    if (!stream) { int saved = errno; close(fd); return error_status(saved); }
    ro_entry *entries = calloc(b->options.max_entries, sizeof *entries);
    if (!entries) { closedir(stream); return OCT_E_NO_MEMORY; }
    uint32_t count = 0, scanned = 0;
    size_t staged_bytes = 0;
    for (;;) {
        if (!allowed(b)) { status = OCT_E_STATE; break; }
        errno = 0;
        struct dirent *item = readdir(stream);
        if (!item) { if (errno) status = error_status(errno); break; }
        if (!strcmp(item->d_name, ".") || !strcmp(item->d_name, "..")) continue;
        if (scanned++ == b->options.max_entries) { status = OCT_E_TOO_LARGE; break; }
        if (oct_name_validate(item->d_name) != OCT_OK) continue;
        struct stat st;
        if (fstatat(dirfd(stream), item->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0) {
            if (errno == ENOENT || errno == EACCES || errno == EPERM) continue;
            status = error_status(errno); break;
        }
        if (!exportable(&st) || (!b->options.cross_filesystems && st.st_dev != b->nodes[0].identity.st_dev)) continue;
        size_t bytes = strlen(item->d_name) + 1;
        if (bytes > b->options.max_name_bytes - staged_bytes) { status = OCT_E_TOO_LARGE; break; }
        entries[count].name = strdup(item->d_name);
        if (!entries[count].name) { status = OCT_E_NO_MEMORY; break; }
        entries[count++].identity = st;
        staged_bytes += bytes;
    }
    closedir(stream);
    if (status == OCT_OK && count > 1) qsort(entries, count, sizeof *entries, compare_entries);
    for (uint32_t i = 0; i < count && status == OCT_OK; i++) {
        if (!allowed(b)) { status = OCT_E_STATE; break; }
        uint64_t node;
        status = intern(b, (uint32_t)directory, &entries[i], &node);
        oct_node_stat stat;
        if (status == OCT_OK) status = ro_stat(b, node, &stat);
        if (status == OCT_OK) status = visit(visit_context, node, entries[i].name, &stat);
    }
    if (status == OCT_OK && !allowed(b)) status = OCT_E_STATE;
    for (uint32_t i = 0; i < count; i++) free(entries[i].name);
    free(entries);
    return status;
}

static oct_status ro_read(void *context, uint64_t node, uint64_t offset, void *buffer, size_t length, size_t *read_count)
{
    ro_backend *b = context;
    if (read_count) *read_count = 0;
    if (!read_count || (!buffer && length)) return OCT_E_INVALID_ARGUMENT;
    if (!allowed(b)) return OCT_E_STATE;
    if (node > b->count || !node) return OCT_E_INVALID_HANDLE;
    if (S_ISDIR(b->nodes[node].identity.st_mode)) return OCT_E_IS_DIRECTORY;
    if (length > b->options.max_read_bytes) return OCT_E_TOO_LARGE;
    if (offset > INT64_MAX || length > (uint64_t)INT64_MAX - offset || sizeof(off_t) < 8) return OCT_E_OUT_OF_RANGE;
    ro_node *n = &b->nodes[node];
    int parent;
    oct_status status = open_directory(b, n->parent, &parent);
    if (status != OCT_OK) return status;
    int fd = openat(parent, n->name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    int saved = errno;
    close(parent);
    if (fd < 0) return error_status(saved);
    struct stat before, after;
    size_t total = 0;
    if (fstat(fd, &before) < 0 || !same(&n->identity, &before)) status = OCT_E_INVALID_HANDLE;
    if (status == OCT_OK && offset > (uint64_t)before.st_size) status = OCT_E_OUT_OF_RANGE;
    while (status == OCT_OK && total < length) {
        if (!allowed(b)) { status = OCT_E_STATE; break; }
        size_t chunk = length - total < RO_CHUNK ? length - total : RO_CHUNK;
        ssize_t got = pread(fd, (char *)buffer + total, chunk, (off_t)(offset + total));
        if (got < 0) { if (errno == EINTR) continue; status = error_status(errno); break; }
        if (!got) break;
        total += (size_t)got;
    }
    if (status == OCT_OK && (fstat(fd, &after) < 0 || !same(&before, &after))) status = OCT_E_INVALID_HANDLE;
    if (!allowed(b)) status = OCT_E_STATE;
    close(fd);
    if (status != OCT_OK) { if (total) memset(buffer, 0, total); return status; }
    *read_count = total;
    return OCT_OK;
}

static oct_status ro_create(void *c, uint64_t p, const char *n, bool d, uint64_t *o, oct_node_stat *s)
{ (void)p; (void)n; (void)d; (void)o; (void)s; return allowed(c) ? OCT_E_WRITE_PROTECTED : OCT_E_STATE; }
static oct_status ro_write(void *c, uint64_t n, uint64_t o, const void *b, size_t l)
{ (void)n; (void)o; (void)b; (void)l; return allowed(c) ? OCT_E_WRITE_PROTECTED : OCT_E_STATE; }
static oct_status ro_truncate(void *c, uint64_t n, uint64_t s)
{ (void)n; (void)s; return allowed(c) ? OCT_E_WRITE_PROTECTED : OCT_E_STATE; }
static oct_status ro_remove(void *c, uint64_t n)
{ (void)n; return allowed(c) ? OCT_E_WRITE_PROTECTED : OCT_E_STATE; }
static oct_status ro_rename(void *c, uint64_t n, const char *s)
{ (void)n; (void)s; return allowed(c) ? OCT_E_WRITE_PROTECTED : OCT_E_STATE; }
static void ro_destroy(void *context)
{
    ro_backend *b = context;
    if (!b) return;
    if (b->root_fd >= 0) close(b->root_fd);
    for (uint32_t i = 1; i <= b->count; i++) free(b->nodes[i].name);
    free(b->nodes);
    free(b);
}
static const oct_backend_ops readonly_ops = {
    "filesystem-readonly", ro_describe, ro_list, ro_stat, ro_read, ro_create,
    ro_write, ro_truncate, ro_remove, ro_rename, ro_destroy, ro_access
};

static int open_root(const char *root)
{
    size_t length = root ? strnlen(root, 4097) : 0;
    if (!length || length > 4096 || root[0] != '/') { errno = EINVAL; return -1; }
    int fd = open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0 || length == 1) return fd;
    char *copy = strdup(root + 1), *save = NULL;
    if (!copy) { close(fd); errno = ENOMEM; return -1; }
    for (char *part = strtok_r(copy, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        if (oct_name_validate(part) != OCT_OK) { close(fd); fd = -1; errno = EINVAL; break; }
        int next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        int saved = errno;
        close(fd); fd = next; errno = saved;
        if (fd < 0) break;
    }
    free(copy);
    return fd;
}

oct_status oct_readonly_backend_create(const oct_readonly_options *options, oct_backend *out)
{
    if (!out) return OCT_E_INVALID_ARGUMENT;
    *out = (oct_backend){ 0 };
    if (!options || !options->authorized || !options->root) return OCT_E_INVALID_ARGUMENT;
    oct_readonly_options o = *options;
    if (!o.max_nodes) o.max_nodes = 4096;
    if (!o.max_entries) o.max_entries = 1024;
    if (!o.max_depth) o.max_depth = 64;
    if (!o.max_name_bytes) o.max_name_bytes = 1024u * 1024u;
    if (!o.max_read_bytes) o.max_read_bytes = 1024u * 1024u;
    if (o.max_nodes > 65535 || o.max_entries > 4096 || o.max_depth > RO_DEPTH ||
        o.max_name_bytes > RO_BYTES || o.max_read_bytes > RO_BYTES) return OCT_E_INVALID_ARGUMENT;
    if (!o.authorized(o.authorization_context)) return OCT_E_STATE;
    int fd = open_root(o.root);
    if (fd < 0) return error_status(errno);
    ro_backend *b = calloc(1, sizeof *b);
    if (!b) { close(fd); return OCT_E_NO_MEMORY; }
    b->root_fd = fd;
    b->options = o;
    b->options.root = NULL; /* never retain a caller's path pointer */
    b->nodes = calloc((size_t)o.max_nodes + 1, sizeof *b->nodes);
    if (!b->nodes) { ro_destroy(b); return OCT_E_NO_MEMORY; }
    if (fstat(fd, &b->nodes[0].identity) < 0) { ro_destroy(b); return OCT_E_IO; }
    if (!allowed(b)) { ro_destroy(b); return OCT_E_STATE; }
    out->ops = &readonly_ops; out->ctx = b;
    return OCT_OK;
}

oct_status oct_readonly_store_open(const oct_readonly_options *options,
                                   const oct_store_options *store_options, oct_store **out)
{
    if (!out) return OCT_E_INVALID_ARGUMENT;
    *out = NULL;
    oct_backend backend;
    oct_status status = oct_readonly_backend_create(options, &backend);
    return status == OCT_OK ? oct_store_open(store_options, backend, out) : status;
}
