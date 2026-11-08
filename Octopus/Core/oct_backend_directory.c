#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#elif !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif
#include "oct_store.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * Directory backend confined to one explicitly designated disposable root.
 * Every path is resolved component by component from the root descriptor with
 * O_NOFOLLOW, so symlinks (including ones swapped in later) never lead outside
 * it. Only regular files and directories are exported.
 */

typedef struct dir_backend {
    int     root_fd;
    char  **paths;  /* node id - 1 -> path relative to the root; NULL once removed */
    size_t  count;
    size_t  capacity;
} dir_backend;

static const oct_backend_ops directory_ops;

static oct_status from_errno(int error)
{
    switch (error) {
    case ENOENT:
    case ELOOP: return OCT_E_INVALID_HANDLE;
    case EEXIST: return OCT_E_EXISTS;
    case ENOTDIR: return OCT_E_NOT_DIRECTORY;
    case EISDIR: return OCT_E_IS_DIRECTORY;
    case EACCES:
    case EPERM:
    case EROFS: return OCT_E_WRITE_PROTECTED;
    case ENOSPC:
    case EDQUOT: return OCT_E_NO_SPACE;
    case ENOMEM: return OCT_E_NO_MEMORY;
    case ENAMETOOLONG: return OCT_E_INVALID_NAME;
    case ENOTEMPTY: return OCT_E_STATE;
    default: return OCT_E_IO;
    }
}

static const char *path_of(const dir_backend *d, uint64_t node)
{
    if (node == 0) return "";
    if (node > d->count) return NULL;
    return d->paths[node - 1];
}

static char *join(const char *directory, const char *name)
{
    size_t a = strlen(directory), b = strlen(name);
    char *path = malloc(a + b + 2);
    if (!path) return NULL;
    if (a) {
        memcpy(path, directory, a);
        path[a] = '/';
        memcpy(path + a + 1, name, b + 1);
    } else {
        memcpy(path, name, b + 1);
    }
    return path;
}

/* Opens a directory below the root, refusing symlinks at every component. */
static int open_directory(const dir_backend *d, const char *relative)
{
    int fd = openat(d->root_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0 || relative[0] == '\0') return fd;
    char *copy = strdup(relative);
    if (!copy) {
        close(fd);
        errno = ENOMEM;
        return -1;
    }
    char *save = NULL;
    for (char *part = strtok_r(copy, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        if (strcmp(part, ".") == 0 || strcmp(part, "..") == 0) {
            close(fd);
            fd = -1;
            errno = EINVAL;
            break;
        }
        int next = openat(fd, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        int saved = errno;
        close(fd);
        errno = saved;
        fd = next;
        if (fd < 0) break;
    }
    free(copy);
    return fd;
}

static int open_parent(const dir_backend *d, const char *relative, const char **leaf)
{
    const char *slash = strrchr(relative, '/');
    if (!slash) {
        *leaf = relative;
        return open_directory(d, "");
    }
    *leaf = slash + 1;
    size_t length = (size_t)(slash - relative);
    char *parent = malloc(length + 1);
    if (!parent) {
        errno = ENOMEM;
        return -1;
    }
    memcpy(parent, relative, length);
    parent[length] = '\0';
    int fd = open_directory(d, parent);
    int saved = errno;
    free(parent);
    errno = saved;
    return fd;
}

static oct_status intern(dir_backend *d, const char *path, uint64_t *node)
{
    for (size_t i = 0; i < d->count; i++) {
        if (d->paths[i] && strcmp(d->paths[i], path) == 0) {
            *node = (uint64_t)(i + 1);
            return OCT_OK;
        }
    }
    if (d->count == d->capacity) {
        if (d->capacity > SIZE_MAX / 2 / sizeof(char *)) return OCT_E_NO_MEMORY;
        size_t capacity = d->capacity ? d->capacity * 2 : 32;
        char **grown = realloc(d->paths, capacity * sizeof *grown);
        if (!grown) return OCT_E_NO_MEMORY;
        d->paths = grown;
        d->capacity = capacity;
    }
    char *copy = strdup(path);
    if (!copy) return OCT_E_NO_MEMORY;
    d->paths[d->count++] = copy;
    *node = (uint64_t)d->count;
    return OCT_OK;
}

static bool convert(const struct stat *st, oct_node_stat *out)
{
    if (S_ISDIR(st->st_mode)) {
        out->is_directory = true;
    } else if (S_ISREG(st->st_mode)) {
        out->is_directory = false;
    } else {
        return false; /* symlinks, devices, sockets and FIFOs are not exported */
    }
    out->size = out->is_directory ? 0 : (uint64_t)st->st_size;
    out->modified = (int64_t)st->st_mtime;
#if defined(__APPLE__)
    out->created = (int64_t)st->st_birthtimespec.tv_sec;
#else
    out->created = (int64_t)st->st_mtime;
#endif
    out->writable = (st->st_mode & S_IWUSR) != 0;
    return true;
}

static oct_status dir_describe(void *ctx, oct_backend_info *info)
{
    dir_backend *d = ctx;
    struct statvfs vfs;
    if (fstatvfs(d->root_fd, &vfs) != 0) return from_errno(errno);
    info->capacity = (uint64_t)vfs.f_blocks * (uint64_t)vfs.f_frsize;
    info->available = (uint64_t)vfs.f_bavail * (uint64_t)vfs.f_frsize;
    info->root_writable = true;
    info->writable = true;
    info->storage_type = 0x0003u;
    return OCT_OK;
}

static oct_status dir_stat(void *ctx, uint64_t node, oct_node_stat *stat)
{
    dir_backend *d = ctx;
    const char *relative = path_of(d, node);
    if (!relative) return OCT_E_INVALID_HANDLE;
    struct stat st;
    if (relative[0] == '\0') {
        if (fstat(d->root_fd, &st) != 0) return from_errno(errno);
    } else {
        const char *leaf;
        int parent = open_parent(d, relative, &leaf);
        if (parent < 0) return from_errno(errno);
        int rc = fstatat(parent, leaf, &st, AT_SYMLINK_NOFOLLOW);
        int saved = errno;
        close(parent);
        if (rc != 0) return from_errno(saved);
    }
    return convert(&st, stat) ? OCT_OK : OCT_E_UNSUPPORTED;
}

typedef struct listing_entry {
    char         *name;
    oct_node_stat stat;
} listing_entry;

static int compare_entries(const void *a, const void *b)
{
    return strcmp(((const listing_entry *)a)->name, ((const listing_entry *)b)->name);
}

static oct_status dir_list(void *ctx, uint64_t directory, oct_node_visitor visit, void *visit_context)
{
    dir_backend *d = ctx;
    const char *relative = path_of(d, directory);
    if (!relative) return OCT_E_INVALID_HANDLE;
    int fd = open_directory(d, relative);
    if (fd < 0) return from_errno(errno);
    DIR *stream = fdopendir(fd);
    if (!stream) {
        int saved = errno;
        close(fd);
        return from_errno(saved);
    }
    listing_entry *entries = NULL;
    size_t count = 0, capacity = 0;
    oct_status status = OCT_OK;
    for (;;) {
        errno = 0;
        struct dirent *item = readdir(stream);
        if (!item) {
            if (errno) status = from_errno(errno);
            break;
        }
        const char *name = item->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        if (directory == 0 && strcmp(name, OCT_DISPOSABLE_MARKER) == 0) continue;
        if (oct_name_validate(name) != OCT_OK) continue;
        struct stat st;
        oct_node_stat converted;
        if (fstatat(dirfd(stream), name, &st, AT_SYMLINK_NOFOLLOW) != 0 || !convert(&st, &converted)) continue;
        if (count == capacity) {
            size_t grown_capacity = capacity ? capacity * 2 : 32;
            listing_entry *grown = realloc(entries, grown_capacity * sizeof *grown);
            if (!grown) {
                status = OCT_E_NO_MEMORY;
                break;
            }
            entries = grown;
            capacity = grown_capacity;
        }
        entries[count].name = strdup(name);
        if (!entries[count].name) {
            status = OCT_E_NO_MEMORY;
            break;
        }
        entries[count].stat = converted;
        count++;
    }
    closedir(stream);
    if (status == OCT_OK && count > 1) qsort(entries, count, sizeof *entries, compare_entries);
    for (size_t i = 0; i < count && status == OCT_OK; i++) {
        char *path = join(relative, entries[i].name);
        uint64_t node = 0;
        status = path ? intern(d, path, &node) : OCT_E_NO_MEMORY;
        free(path);
        if (status == OCT_OK) status = visit(visit_context, node, entries[i].name, &entries[i].stat);
    }
    for (size_t i = 0; i < count; i++) free(entries[i].name);
    free(entries);
    return status;
}

static oct_status open_file(const dir_backend *d, uint64_t node, int flags, int *out)
{
    const char *relative = path_of(d, node);
    if (!relative || relative[0] == '\0') return OCT_E_INVALID_HANDLE;
    const char *leaf;
    int parent = open_parent(d, relative, &leaf);
    if (parent < 0) return from_errno(errno);
    int fd = openat(parent, leaf, flags | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    int saved = errno;
    close(parent);
    if (fd < 0) return from_errno(saved);
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        bool directory = S_ISDIR(st.st_mode);
        close(fd);
        return directory ? OCT_E_IS_DIRECTORY : OCT_E_UNSUPPORTED;
    }
    *out = fd;
    return OCT_OK;
}

static oct_status dir_read(void *ctx, uint64_t node, uint64_t offset, void *buffer, size_t length, size_t *read)
{
    dir_backend *d = ctx;
    *read = 0;
    if (offset > (uint64_t)INT64_MAX - length) return OCT_E_OUT_OF_RANGE;
    int fd;
    oct_status status = open_file(d, node, O_RDONLY, &fd);
    if (status != OCT_OK) return status;
    size_t total = 0;
    while (total < length) {
        ssize_t got = pread(fd, (uint8_t *)buffer + total, length - total, (off_t)(offset + total));
        if (got < 0) {
            if (errno == EINTR) continue;
            status = from_errno(errno);
            break;
        }
        if (got == 0) break;
        total += (size_t)got;
    }
    close(fd);
    *read = total;
    return status;
}

static oct_status dir_create(void *ctx, uint64_t parent, const char *name, bool is_directory,
                             uint64_t *node, oct_node_stat *stat)
{
    dir_backend *d = ctx;
    const char *relative = path_of(d, parent);
    if (!relative) return OCT_E_INVALID_PARENT;
    if (oct_name_validate(name) != OCT_OK || (parent == 0 && strcmp(name, OCT_DISPOSABLE_MARKER) == 0)) {
        return OCT_E_INVALID_NAME;
    }
    int dir = open_directory(d, relative);
    if (dir < 0) return from_errno(errno);
    int rc;
    if (is_directory) {
        rc = mkdirat(dir, name, 0755);
    } else {
        int fd = openat(dir, name, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
        rc = fd < 0 ? -1 : close(fd);
    }
    struct stat st;
    if (rc == 0) rc = fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW);
    int saved = errno;
    close(dir);
    if (rc != 0) return from_errno(saved);
    char *path = join(relative, name);
    oct_status status = path ? intern(d, path, node) : OCT_E_NO_MEMORY;
    free(path);
    if (status == OCT_OK && !convert(&st, stat)) status = OCT_E_UNSUPPORTED;
    return status;
}

static oct_status dir_write(void *ctx, uint64_t node, uint64_t offset, const void *data, size_t length)
{
    dir_backend *d = ctx;
    if (offset > (uint64_t)INT64_MAX - length) return OCT_E_TOO_LARGE;
    int fd;
    oct_status status = open_file(d, node, O_WRONLY, &fd);
    if (status != OCT_OK) return status;
    size_t total = 0;
    while (total < length) {
        ssize_t put = pwrite(fd, (const uint8_t *)data + total, length - total, (off_t)(offset + total));
        if (put < 0) {
            if (errno == EINTR) continue;
            status = from_errno(errno);
            break;
        }
        total += (size_t)put;
    }
    if (close(fd) != 0 && status == OCT_OK) status = from_errno(errno);
    return status;
}

static oct_status dir_truncate(void *ctx, uint64_t node, uint64_t size)
{
    dir_backend *d = ctx;
    if (size > (uint64_t)INT64_MAX) return OCT_E_TOO_LARGE;
    int fd;
    oct_status status = open_file(d, node, O_WRONLY, &fd);
    if (status != OCT_OK) return status;
    if (ftruncate(fd, (off_t)size) != 0) status = from_errno(errno);
    close(fd);
    return status;
}

static oct_status dir_remove(void *ctx, uint64_t node)
{
    dir_backend *d = ctx;
    const char *relative = path_of(d, node);
    if (!relative || relative[0] == '\0') return OCT_E_INVALID_HANDLE;
    const char *leaf;
    int parent = open_parent(d, relative, &leaf);
    if (parent < 0) return from_errno(errno);
    struct stat st;
    int rc = fstatat(parent, leaf, &st, AT_SYMLINK_NOFOLLOW);
    if (rc == 0) rc = unlinkat(parent, leaf, S_ISDIR(st.st_mode) ? AT_REMOVEDIR : 0);
    int saved = errno;
    close(parent);
    if (rc != 0) return from_errno(saved);
    free(d->paths[node - 1]);
    d->paths[node - 1] = NULL;
    return OCT_OK;
}

static int rename_without_replacing(int dir, const char *from, const char *to)
{
#if defined(__APPLE__)
    return renameatx_np(dir, from, dir, to, RENAME_EXCL);
#else
#if defined(RENAME_NOREPLACE)
    int rc = renameat2(dir, from, dir, to, RENAME_NOREPLACE);
    if (rc == 0 || (errno != EINVAL && errno != ENOSYS)) return rc;
#endif
    struct stat st;
    if (fstatat(dir, to, &st, AT_SYMLINK_NOFOLLOW) == 0) {
        errno = EEXIST;
        return -1;
    }
    return renameat(dir, from, dir, to);
#endif
}

static oct_status dir_rename(void *ctx, uint64_t node, const char *name)
{
    dir_backend *d = ctx;
    const char *relative = path_of(d, node);
    if (!relative || relative[0] == '\0') return OCT_E_INVALID_HANDLE;
    bool at_root = strchr(relative, '/') == NULL;
    if (oct_name_validate(name) != OCT_OK || (at_root && strcmp(name, OCT_DISPOSABLE_MARKER) == 0)) {
        return OCT_E_INVALID_NAME;
    }
    const char *leaf;
    size_t old_length = strlen(relative);
    size_t prefix = (size_t)(strrchr(relative, '/') ? strrchr(relative, '/') - relative + 1 : 0);
    size_t name_length = strlen(name);
    /* Prepare every replacement path first so the rename never half-applies. */
    char **replacement = calloc(d->count, sizeof *replacement);
    if (!replacement && d->count) return OCT_E_NO_MEMORY;
    oct_status status = OCT_OK;
    for (size_t i = 0; i < d->count && status == OCT_OK; i++) {
        const char *p = d->paths[i];
        if (!p || strncmp(p, relative, old_length) != 0 || (p[old_length] != '\0' && p[old_length] != '/')) continue;
        size_t tail = strlen(p + old_length);
        replacement[i] = malloc(prefix + name_length + tail + 1);
        if (!replacement[i]) {
            status = OCT_E_NO_MEMORY;
            break;
        }
        memcpy(replacement[i], relative, prefix);
        memcpy(replacement[i] + prefix, name, name_length);
        memcpy(replacement[i] + prefix + name_length, p + old_length, tail + 1);
    }
    if (status == OCT_OK) {
        int parent = open_parent(d, relative, &leaf);
        if (parent < 0) {
            status = from_errno(errno);
        } else {
            int rc = rename_without_replacing(parent, leaf, name);
            int saved = errno;
            close(parent);
            if (rc != 0) status = from_errno(saved);
        }
    }
    for (size_t i = 0; i < d->count; i++) {
        if (!replacement || !replacement[i]) continue;
        if (status == OCT_OK) {
            free(d->paths[i]);
            d->paths[i] = replacement[i];
        } else {
            free(replacement[i]);
        }
    }
    free(replacement);
    return status;
}

static void dir_destroy(void *ctx)
{
    dir_backend *d = ctx;
    if (!d) return;
    if (d->root_fd >= 0) close(d->root_fd);
    for (size_t i = 0; i < d->count; i++) free(d->paths[i]);
    free(d->paths);
    free(d);
}

static const oct_backend_ops directory_ops = {
    "directory", dir_describe, dir_list, dir_stat, dir_read, dir_create,
    dir_write, dir_truncate, dir_remove, dir_rename, dir_destroy,
};

oct_status oct_directory_backend_create(const char *root, oct_backend *out)
{
    if (!out) return OCT_E_INVALID_ARGUMENT;
    out->ops = NULL;
    out->ctx = NULL;
    struct stat st;
    if (!root || lstat(root, &st) != 0 || !S_ISDIR(st.st_mode)) return OCT_E_INVALID_ARGUMENT;
    int fd = open(root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return OCT_E_INVALID_ARGUMENT;
    struct stat marker;
    if (fstatat(fd, OCT_DISPOSABLE_MARKER, &marker, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(marker.st_mode)) {
        close(fd);
        return OCT_E_INVALID_ARGUMENT; /* not explicitly designated as disposable */
    }
    dir_backend *d = calloc(1, sizeof *d);
    if (!d) {
        close(fd);
        return OCT_E_NO_MEMORY;
    }
    d->root_fd = fd;
    out->ops = &directory_ops;
    out->ctx = d;
    return OCT_OK;
}
