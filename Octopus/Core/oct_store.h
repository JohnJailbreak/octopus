/*
 * Octopus file-object provider.
 *
 * Presents a hierarchical filesystem as portable-device objects: one storage,
 * integer object handles, parent/child relationships, PTP object formats,
 * protection status and byte-addressable file data. The store is independent
 * of any USB transport and of the protocol encoder; storage contents come from
 * a pluggable backend (in-memory synthetic tree or a disposable directory).
 *
 * Handles are allocated from a monotonically increasing counter and are never
 * reused while the store is open, so a handle stays valid until its object is
 * deleted. Directories are enumerated lazily, on first use.
 *
 * The store is not internally synchronised; callers serialise access.
 */
#ifndef OCT_STORE_H
#define OCT_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OCT_CORE_VERSION "0.1.0"

typedef enum oct_status {
    OCT_OK = 0,
    OCT_E_INVALID_ARGUMENT,
    OCT_E_NO_MEMORY,
    OCT_E_INVALID_HANDLE,   /* unknown or deleted object handle */
    OCT_E_INVALID_PARENT,   /* parent does not exist */
    OCT_E_NOT_DIRECTORY,    /* object is not an association/directory */
    OCT_E_IS_DIRECTORY,     /* operation needs a file */
    OCT_E_INVALID_NAME,
    OCT_E_EXISTS,
    OCT_E_WRITE_PROTECTED,  /* object, its parent or the storage is read-only */
    OCT_E_PARTIAL_DELETION,
    OCT_E_OUT_OF_RANGE,
    OCT_E_TOO_LARGE,
    OCT_E_NO_SPACE,
    OCT_E_IO,
    OCT_E_UNSUPPORTED,
    OCT_E_STATE,            /* not valid in the current service/session state */
    OCT_E_MALFORMED         /* undecodable protocol input */
} oct_status;

const char *oct_status_name(oct_status status);

/* PTP object format codes assigned by the provider (PIMA 15740 table 18). */
#define OCT_FORMAT_UNDEFINED   0x3000u
#define OCT_FORMAT_ASSOCIATION 0x3001u
#define OCT_FORMAT_TEXT        0x3004u

#define OCT_DEFAULT_STORAGE_ID 0x00010001u /* physical store 1, logical store 1 */
#define OCT_ROOT               0u          /* parent value for root-level objects */

/* Names are UTF-8. POSIX limits a component to 255 bytes; a PTP string holds
 * at most 254 UTF-16 code units plus its terminator. Both limits apply. */
#define OCT_NAME_MAX        255
#define OCT_NAME_UTF16_MAX  254
#define OCT_LABEL_MAX       63

typedef struct oct_object_info {
    uint32_t handle;
    uint32_t parent;           /* OCT_ROOT for root-level objects */
    uint32_t storage_id;
    uint16_t format;
    bool     is_directory;
    bool     write_protected;  /* PTP ProtectionStatus 0x0001 */
    uint64_t size;             /* bytes; 0 for directories */
    int64_t  created;          /* seconds since 1970-01-01 UTC */
    int64_t  modified;
    char     name[OCT_NAME_MAX + 1];
} oct_object_info;

typedef struct oct_storage_info {
    uint32_t storage_id;
    uint16_t storage_type;      /* PTP StorageType */
    uint16_t filesystem_type;   /* 0x0002 generic hierarchical */
    uint16_t access_capability; /* 0x0000 read-write, 0x0001 read-only */
    uint64_t max_capacity;
    uint64_t free_space;
    uint32_t object_count;      /* objects indexed so far */
    char     description[OCT_LABEL_MAX + 1];
    char     volume_label[OCT_LABEL_MAX + 1];
} oct_storage_info;

typedef struct oct_handle_list {
    uint32_t *handles;
    uint32_t  count;
    uint32_t  capacity;
} oct_handle_list;

void oct_handle_list_free(oct_handle_list *list);

/* ---- Storage backends -------------------------------------------------- */

/* Backend node identifiers are opaque to the store; 0 is the backend root. */
typedef struct oct_node_stat {
    bool     is_directory;
    bool     writable;          /* the backend permits modifying this node */
    uint64_t size;
    int64_t  created;
    int64_t  modified;
} oct_node_stat;

typedef struct oct_backend_info {
    uint64_t capacity;
    uint64_t available;
    bool     root_writable;     /* objects may be created at the root */
    bool     writable;          /* some part of the hierarchy accepts changes */
    uint16_t storage_type;      /* PTP StorageType */
} oct_backend_info;

typedef oct_status (*oct_node_visitor)(void *context, uint64_t node,
                                       const char *name,
                                       const oct_node_stat *stat);

typedef struct oct_backend_ops {
    const char *kind;
    oct_status (*describe)(void *ctx, oct_backend_info *info);
    oct_status (*list)(void *ctx, uint64_t directory, oct_node_visitor visit,
                       void *visit_context);
    oct_status (*stat)(void *ctx, uint64_t node, oct_node_stat *stat);
    oct_status (*read)(void *ctx, uint64_t node, uint64_t offset, void *buffer,
                       size_t length, size_t *read);
    oct_status (*create)(void *ctx, uint64_t parent, const char *name,
                         bool is_directory, uint64_t *node, oct_node_stat *stat);
    oct_status (*write)(void *ctx, uint64_t node, uint64_t offset,
                        const void *data, size_t length);
    oct_status (*truncate)(void *ctx, uint64_t node, uint64_t size);
    oct_status (*remove)(void *ctx, uint64_t node);  /* file or empty directory */
    oct_status (*rename)(void *ctx, uint64_t node, const char *name);
    void       (*destroy)(void *ctx);
} oct_backend_ops;

typedef struct oct_backend {
    const oct_backend_ops *ops;
    void                  *ctx;
} oct_backend;

/* In-memory backend. Nodes are added by the owner before or after the store
 * opens; only nodes marked writable can be modified through the store. */
oct_status oct_memory_backend_create(uint64_t capacity, oct_backend *out);
oct_status oct_memory_backend_add(oct_backend *backend, uint64_t parent,
                                  const char *name, bool is_directory,
                                  const void *data, size_t length,
                                  bool writable, int64_t time, uint64_t *node);
void       oct_memory_backend_set_root_writable(oct_backend *backend, bool writable);

/* The Octopus synthetic hierarchy:
 *   README.txt, Documents/{example.txt, sample.bin}, Information/version.txt
 * All seed objects are read-only. With `scratch`, an empty writable "Scratch"
 * directory is added: the only place hosts may create, rename or delete. */
typedef struct oct_synthetic_options {
    int64_t time;     /* timestamp for every seed object */
    bool    scratch;
} oct_synthetic_options;

#define OCT_SAMPLE_BIN_SIZE 4096u
uint8_t    oct_sample_byte(size_t offset);
oct_status oct_synthetic_backend_create(const oct_synthetic_options *options,
                                        oct_backend *out);

/* Directory backend rooted at an explicitly designated disposable directory.
 * The root must be a real directory (not a symlink) that contains a regular
 * file named OCT_DISPOSABLE_MARKER; anything else is refused. Symlinks and
 * special files inside it are not exported, and no path ever leaves it. */
#define OCT_DISPOSABLE_MARKER ".octopus-disposable"
oct_status oct_directory_backend_create(const char *root, oct_backend *out);

/* ---- Store -------------------------------------------------------------- */

typedef struct oct_store oct_store;

typedef struct oct_store_options {
    uint32_t    storage_id;    /* 0 selects OCT_DEFAULT_STORAGE_ID */
    uint32_t    first_handle;  /* 0 selects 1 */
    const char *description;   /* NULL selects "Octopus" */
    const char *volume_label;  /* NULL selects the description */
} oct_store_options;

/* Takes ownership of `backend` in all cases. */
oct_status oct_store_open(const oct_store_options *options, oct_backend backend,
                          oct_store **out);
void       oct_store_close(oct_store *store);

uint32_t   oct_store_storage_id(const oct_store *store);
uint32_t   oct_store_object_count(const oct_store *store);
const char *oct_store_backend_kind(const oct_store *store);
oct_status oct_store_storage_info(oct_store *store, oct_storage_info *out);
oct_status oct_store_index_all(oct_store *store);

oct_status oct_store_get(oct_store *store, uint32_t handle, oct_object_info *out);
oct_status oct_store_find(oct_store *store, uint32_t parent, const char *name,
                          uint32_t *handle);
/* Children of `parent` (OCT_ROOT or a directory handle) in backend order;
 * with `recursive`, every descendant, parents before children. */
oct_status oct_store_list(oct_store *store, uint32_t parent, bool recursive,
                          oct_handle_list *out);
oct_status oct_store_read(oct_store *store, uint32_t handle, uint64_t offset,
                          void *buffer, size_t length, size_t *read);

oct_status oct_store_create(oct_store *store, uint32_t parent, const char *name,
                            bool is_directory, uint32_t *handle);
oct_status oct_store_write(oct_store *store, uint32_t handle, uint64_t offset,
                           const void *data, size_t length);
oct_status oct_store_truncate(oct_store *store, uint32_t handle, uint64_t size);
/* Deletes an object; directories are deleted with their contents. Returns
 * OCT_E_PARTIAL_DELETION when only some descendants could be removed. */
oct_status oct_store_delete(oct_store *store, uint32_t handle, uint32_t *deleted);
oct_status oct_store_rename(oct_store *store, uint32_t handle, const char *name);

/* Name rules shared by the store and protocol: valid UTF-8, 1–255 bytes, at
 * most 254 UTF-16 units, no '/', no NUL, no control characters, not "." or "..". */
oct_status oct_name_validate(const char *name);
uint16_t   oct_format_for_name(const char *name, bool is_directory);
/* Decodes one UTF-8 sequence at position; returns its width, or 0 if invalid. */
size_t     oct_utf8_next(const char *text, size_t length, size_t position,
                         uint32_t *codepoint);
/* Copies at most capacity - 1 bytes of UTF-8, never splitting a sequence. */
void       oct_copy_utf8(char *destination, size_t capacity, const char *source);
/* Number of UTF-16 code units needed for valid UTF-8, or -1 if invalid. */
long       oct_utf8_utf16_length(const char *utf8, size_t length);

#ifdef __cplusplus
}
#endif

#endif /* OCT_STORE_H */
