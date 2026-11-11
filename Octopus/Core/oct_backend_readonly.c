#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#elif defined(__linux__)
#define _GNU_SOURCE 1 /* O_PATH: walk up through directories we may search but not read */
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
#include <stdio.h>
#include <unistd.h>

/*
 * How the containment walk opens each ancestor: for search only, where the platform allows it.
 * Opening for reading needs read permission, which is exactly what a sandbox withholds from
 * /var/mobile while still letting us traverse it, so a read-only walk failed at the very directory
 * a graft goes through and every grafted child was discarded. O_SEARCH is POSIX and Darwin's;
 * O_PATH is Linux's. Where neither exists this falls back to reading, which fails closed: a child
 * whose containment cannot be shown is not offered.
 */
#if defined(O_SEARCH)
#define RO_SEARCH_FLAGS (O_SEARCH | O_CLOEXEC)
#elif defined(O_PATH)
#define RO_SEARCH_FLAGS (O_PATH | O_DIRECTORY | O_CLOEXEC)
#else
#define RO_SEARCH_FLAGS (O_RDONLY | O_DIRECTORY | O_CLOEXEC)
#endif

#define RO_DEPTH 256u
#define RO_BYTES (16u * 1024u * 1024u)
#define RO_CHUNK 65536u

typedef struct ro_node {
    char *name;
    uint32_t parent;
    uint32_t depth;
    struct stat identity;
    /* This entry is a symlink whose target was verified to lie inside the served root, so it is
     * opened and stat'd by following the link. On iOS /etc, /tmp and /var are all symlinks, so
     * refusing every one of them hid /var and with it everything under it. */
    bool follow;
    /* Reached through a parent the sandbox will not let us list or open, by opening
     * "<parent>/<name>" relative to the grandparent. On iOS /var/mobile is like this: listing it is
     * denied while /var/mobile/Media beneath it is readable, so traversal is allowed and only
     * enumeration is not. The parent is skipped on the way down, never opened. */
    bool via;
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
    bool follow;
    bool via;
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

/*
 * Whether an opened directory lies inside the served root, decided by walking ".." from it until
 * the root's own device and inode appear. Paths are never compared as strings, so a symlink
 * cannot escape by spelling its way out, and crossing a mount boundary is fine because ".." at a
 * mount point leads to the directory it is mounted on.
 *
 * The walk is bounded, and a filesystem root that is not ours ends it: there ".." is itself, which
 * is detected and refused rather than looped on.
 */
static bool contained(const ro_backend *b, int fd)
{
    struct stat here;
    if (fstat(fd, &here) < 0) return false;
    if (here.st_dev == b->nodes[0].identity.st_dev && here.st_ino == b->nodes[0].identity.st_ino) {
        return true;
    }
    int at = openat(fd, ".", RO_SEARCH_FLAGS);
    if (at < 0) return false;
    for (unsigned step = 0; step < RO_DEPTH; step++) {
        if (!allowed(b)) { close(at); return false; }
        int up = openat(at, "..", RO_SEARCH_FLAGS);
        if (up < 0) { close(at); return false; }
        struct stat parent;
        if (fstat(up, &parent) < 0) { close(up); close(at); return false; }
        if (parent.st_dev == b->nodes[0].identity.st_dev &&
            parent.st_ino == b->nodes[0].identity.st_ino) {
            close(up); close(at);
            return true;
        }
        /* The top of a filesystem that is not ours: ".." no longer moves. */
        if (parent.st_dev == here.st_dev && parent.st_ino == here.st_ino) { close(up); close(at); return false; }
        here = parent;
        close(at);
        at = up;
    }
    close(at);
    return false;
}


static bool join_components(char *out, size_t capacity, const char *first, const char *second)
{
    int written = snprintf(out, capacity, "%s/%s", first, second);
    return written > 0 && (size_t)written < capacity;
}

/* Directory names tried beneath a directory that cannot be listed. Only ones that open, are
 * directories, and lie inside the served root are shown; the rest are skipped silently, so a name
 * here that does not exist costs one failed open. These are what an iOS /var/mobile holds. */
static const char *const graft_candidates[] = {
    "Media", "Documents", "Library", "Containers", "Downloads",
};

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
        if (i >= 2 && b->nodes[chain[i - 2]].via) {
            /* The next node is reached through this one without opening it: open both components
             * at once from here, verify the child, and step past the pair. */
            ro_node *child = &b->nodes[chain[i - 2]];
            char joined[2 * (OCT_NAME_MAX + 1) + 2];
            if (!join_components(joined, sizeof joined, n->name, child->name)) {
                close(fd);
                return OCT_E_INVALID_NAME;
            }
            int via_flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | (child->follow ? 0 : O_NOFOLLOW);
            int reached = openat(fd, joined, via_flags);
            int reach_error = errno;
            close(fd);
            if (reached < 0) return error_status(reach_error);
            struct stat reached_stat;
            if (fstat(reached, &reached_stat) < 0 || !same(&child->identity, &reached_stat)) {
                close(reached);
                return OCT_E_INVALID_HANDLE;
            }
            fd = reached;
            i--; /* the child is done too */
            continue;
        }
        int flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | (n->follow ? 0 : O_NOFOLLOW);
        int next = openat(fd, n->name, flags);
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
    struct stat st;
    int result;
    if (node && n->via) {
        /* Its parent cannot be opened, so stat it from the grandparent through the parent's name. */
        ro_node *parent = &b->nodes[n->parent];
        char joined[2 * (OCT_NAME_MAX + 1) + 2];
        if (!join_components(joined, sizeof joined, parent->name, n->name)) return OCT_E_INVALID_NAME;
        oct_status opened = open_directory(b, parent->parent, &fd);
        if (opened != OCT_OK) return opened;
        result = fstatat(fd, joined, &st, n->follow ? 0 : AT_SYMLINK_NOFOLLOW);
    } else {
        oct_status opened = open_directory(b, node ? n->parent : 0, &fd);
        if (opened != OCT_OK) return opened;
        result = node ? fstatat(fd, n->name, &st, n->follow ? 0 : AT_SYMLINK_NOFOLLOW) : fstat(fd, &st);
    }
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
        if (n->parent == parent && n->follow == entry->follow && n->via == entry->via &&
            !strcmp(n->name, entry->name) &&
            same(&n->identity, &entry->identity)) {
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
    /* Carried onto the node, or the entry would be listed and then opened without following,
     * which fails for every symlink it was resolved from. */
    n->follow = entry->follow;
    n->via = entry->via;
    b->name_bytes += bytes;
    *out = b->count;
    return OCT_OK;
}

/* Children of a directory the sandbox will not open, found by opening "<directory>/<candidate>"
 * from its parent. Each is verified as a directory inside the served root before it is shown. */
static oct_status list_grafted(ro_backend *b, uint64_t directory, oct_node_visitor visit, void *visit_context)
{
    ro_node *dir = &b->nodes[directory];
    if (dir->depth >= b->options.max_depth) return OCT_E_TOO_LARGE;
    int parent_fd;
    oct_status status = open_directory(b, dir->parent, &parent_fd);
    if (status != OCT_OK) return status;
    ro_entry found[sizeof graft_candidates / sizeof graft_candidates[0]];
    uint32_t count = 0;
    for (size_t i = 0; i < sizeof graft_candidates / sizeof graft_candidates[0]; i++) {
        if (!allowed(b)) { status = OCT_E_STATE; break; }
        char joined[2 * (OCT_NAME_MAX + 1) + 2];
        if (!join_components(joined, sizeof joined, dir->name, graft_candidates[i])) continue;
        int child = openat(parent_fd, joined, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (child < 0) continue;
        struct stat st;
        bool keep = fstat(child, &st) == 0 && S_ISDIR(st.st_mode) && contained(b, child);
        close(child);
        if (!keep) continue;
        found[count].name = strdup(graft_candidates[i]);
        if (!found[count].name) { status = OCT_E_NO_MEMORY; break; }
        found[count].identity = st;
        found[count].follow = false;
        found[count].via = true;
        count++;
    }
    close(parent_fd);
    for (uint32_t i = 0; i < count && status == OCT_OK; i++) {
        uint64_t node;
        status = intern(b, (uint32_t)directory, &found[i], &node);
        oct_node_stat stat;
        if (status == OCT_OK) status = ro_stat(b, node, &stat);
        if (status == OCT_OK) status = visit(visit_context, node, found[i].name, &stat);
    }
    for (uint32_t i = 0; i < count; i++) free(found[i].name);
    if (status == OCT_OK && !allowed(b)) status = OCT_E_STATE;
    return status;
}

static oct_status ro_list(void *context, uint64_t directory, oct_node_visitor visit, void *visit_context)
{
    ro_backend *b = context;
    if (!visit) return OCT_E_INVALID_ARGUMENT;
    int fd;
    oct_status status = open_directory(b, directory, &fd);
    if (status == OCT_E_WRITE_PROTECTED && directory != 0) {
        /* The sandbox refused this directory. Traversal through it may still be allowed, as it is
         * for /var/mobile, so offer the children that can be reached rather than nothing. */
        return list_grafted(b, directory, visit, visit_context);
    }
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
        bool follow = false;
        if (S_ISLNK(st.st_mode)) {
            /* Resolve it and keep it only if the target is a directory or regular file that is
             * still inside the served root. A link that leaves the root, dangles, or loops is
             * skipped exactly as before. */
            struct stat target;
            if (fstatat(dirfd(stream), item->d_name, &target, 0) < 0) continue;
            if (!exportable(&target)) continue;
            if (S_ISDIR(target.st_mode)) {
                int probe = openat(dirfd(stream), item->d_name, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
                if (probe < 0) continue;
                bool inside = contained(b, probe);
                close(probe);
                if (!inside) continue;
            } else if (!b->options.cross_filesystems &&
                       target.st_dev != b->nodes[0].identity.st_dev) {
                /* A file reached through a link on another filesystem, when crossing is off. */
                continue;
            }
            st = target;
            follow = true;
        }
        if (!exportable(&st)) continue;
        /* A directory reached through a verified link may sit on another filesystem, which on iOS
         * is the normal case for /var; containment above already established it is ours. */
        if (!follow && !b->options.cross_filesystems && st.st_dev != b->nodes[0].identity.st_dev) continue;
        size_t bytes = strlen(item->d_name) + 1;
        if (bytes > b->options.max_name_bytes - staged_bytes) { status = OCT_E_TOO_LARGE; break; }
        entries[count].name = strdup(item->d_name);
        if (!entries[count].name) { status = OCT_E_NO_MEMORY; break; }
        entries[count].identity = st;
        entries[count++].follow = follow;
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
    int fd = openat(parent, n->name,
                    O_RDONLY | O_NONBLOCK | O_CLOEXEC | (n->follow ? 0 : O_NOFOLLOW));
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
    /* The ceiling was 65535, which a real root passes within a few directories: this phone holds
     * about 600,000 objects under /private alone. Raising it here is what lets a filesystem scope
     * keep serving instead of failing every listing once the table fills. */
    if (o.max_nodes > OCT_READONLY_MAX_NODES || o.max_entries > 4096 || o.max_depth > RO_DEPTH ||
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
