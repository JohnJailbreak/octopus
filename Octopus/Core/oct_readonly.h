/* Independently testable, explicitly authorized, read-only filesystem provider.
 * Not enabled by the app/native observer. No write operation is implemented. */
#ifndef OCT_READONLY_H
#define OCT_READONLY_H
#include "oct_store.h"
#ifdef __cplusplus
extern "C" {
#endif

/* A ceiling for a real filesystem root, where the sample tree's budget is far too small: a phone
 * holds hundreds of thousands of objects under /private and /System alone, and a handle is only
 * created for an object a host has actually asked about. */
#define OCT_READONLY_MAX_NODES 262144u

typedef struct oct_readonly_options {
    const char *root; /* absolute real path; / is allowed, symlink components are not */
    bool (*authorized)(void *context); /* required; checked throughout each operation */
    void *authorization_context;
    uint32_t max_nodes;       /* 0 = 4096; maximum OCT_READONLY_MAX_NODES; root excluded */
    uint32_t max_entries;     /* 0 = 1024; maximum 4096 per directory, including skipped entries */
    uint32_t max_depth;       /* 0 = 64; maximum 256 */
    size_t max_name_bytes;    /* 0 = 1 MiB; maximum 16 MiB total retained name bytes */
    size_t max_read_bytes;    /* 0 = 1 MiB; maximum 16 MiB per read call */
    bool cross_filesystems;  /* false skips entries whose st_dev differs from the root */
} oct_readonly_options;

/* Output ownership follows oct_store_open. The backend's access gate prevents
 * cached names/metadata from bypassing revocation and rejects recursive listing.
 * Directory contents are snapshots for this store lifetime: open a new store to
 * refresh them. Existing IDs never retarget a replacement inode. Changed regular
 * files invalidate their snapshot (including size/mtime/ctime), rather than mix
 * versions across a download. Directory identity uses device/inode/type.
 * Serial caller required, including authorization changes. */
oct_status oct_readonly_backend_create(const oct_readonly_options *options, oct_backend *out);
oct_status oct_readonly_store_open(const oct_readonly_options *options,
                                   const oct_store_options *store_options, oct_store **out);
#ifdef __cplusplus
}
#endif
#endif
