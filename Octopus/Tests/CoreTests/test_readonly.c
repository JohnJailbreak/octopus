/*
 * test_readonly.c — the read-only filesystem backend (oct_backend_readonly.c).
 *
 * It is the path toward browsing a real filesystem over USB, so every safety property it claims is
 * exercised here through the real store rather than taken on trust:
 *
 *  1. It refuses to be created without an authorization callback, or on a symlinked root.
 *  2. Authorized, it lists a real tree and reads files at arbitrary offsets.
 *  3. Revocation is fail-closed: once authorization lapses, listing, metadata and reads all stop,
 *     including for objects already cached by the store.
 *  4. A recursive listing (PTP's whole-hierarchy request) is refused before any traversal.
 *  5. Symlinks inside the tree are never exported or followed.
 *  6. Every write operation is refused.
 *  7. A file replaced by another inode after listing is never served under the old handle.
 *  8. Per-directory entry and depth limits hold.
 */
#include "test_support.h"

#include "../../Core/oct_readonly.h"
#include "../../Protocol/oct_responder.h"
#include "../../Protocol/oct_ptp.h"

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>

#define ROOT_CAPACITY 1024
#define PATH_CAPACITY 2048

/*
 * The backend refuses a root with a symlink in any component, by design. On macOS /tmp is a
 * symlink to /private/tmp, as /var is to /private/var on iOS, so a temporary tree's path must be
 * resolved first, exactly as a real caller must resolve its root. Rewrites root in place.
 */
static bool real_path_of(char *root, size_t capacity)
{
    char real[4096];
    if (!realpath(root, real) || strlen(real) >= capacity) return false;
    memcpy(root, real, strlen(real) + 1);
    return true;
}

static bool grant = true;
static bool authorized(void *context) { (void)context; return grant; }

static void put(const char *path, const char *text)
{
    FILE *file = fopen(path, "w");
    CHECK(file != NULL);
    if (file) {
        CHECK(fputs(text, file) >= 0);
        CHECK_EQ(fclose(file), 0);
    }
}

/* The tree used throughout: a file, a nested file, a symlink to each kind, nothing else. */
static void make_tree(char *root)
{
    char path[PATH_CAPACITY];
    CHECK(mkdtemp(root) != NULL);
    CHECK(real_path_of(root, ROOT_CAPACITY));
    snprintf(path, sizeof path, "%s/readme.txt", root);      put(path, "0123456789abcdef");
    snprintf(path, sizeof path, "%s/sub", root);             CHECK_EQ(mkdir(path, 0755), 0);
    snprintf(path, sizeof path, "%s/sub/inner.txt", root);   put(path, "inner");
    snprintf(path, sizeof path, "%s/link-to-file", root);    CHECK_EQ(symlink("readme.txt", path), 0);
    snprintf(path, sizeof path, "%s/link-to-dir", root);     CHECK_EQ(symlink("sub", path), 0);
    snprintf(path, sizeof path, "%s/link-outside", root);    CHECK_EQ(symlink("/etc", path), 0);
}

static uint32_t named(oct_store *store, uint32_t parent, const char *name)
{
    oct_handle_list list = { 0 };
    uint32_t found = 0;
    if (oct_store_list(store, parent, false, &list) == OCT_OK) {
        for (uint32_t i = 0; i < list.count && !found; i++) {
            oct_object_info info;
            if (oct_store_get(store, list.handles[i], &info) == OCT_OK && strcmp(info.name, name) == 0) {
                found = list.handles[i];
            }
        }
    }
    oct_handle_list_free(&list);
    return found;
}

static void test_creation_is_guarded(void)
{
    char root[ROOT_CAPACITY] = "/tmp/octopus-ro-XXXXXX", alias[PATH_CAPACITY];
    make_tree(root);
    oct_backend backend;
    oct_readonly_options options = { .root = root, .authorized = authorized };
    oct_readonly_options unguarded = { .root = root };
    CHECK(oct_readonly_backend_create(&unguarded, &backend) != OCT_OK); /* no authorization callback */
    CHECK(oct_readonly_backend_create(NULL, &backend) != OCT_OK);
    snprintf(alias, sizeof alias, "%s-alias", root);
    CHECK_EQ(symlink(root, alias), 0);
    oct_readonly_options through_link = { .root = alias, .authorized = authorized };
    CHECK(oct_readonly_backend_create(&through_link, &backend) != OCT_OK); /* symlinked root */
    oct_readonly_options relative = { .root = "tmp", .authorized = authorized };
    CHECK(oct_readonly_backend_create(&relative, &backend) != OCT_OK);     /* not absolute */
    /* A symlink in a middle component is refused too: this is what made /tmp fail on macOS. */
    char nested[PATH_CAPACITY + 8]; /* alias plus "/sub" always fits */
    snprintf(nested, sizeof nested, "%s/sub", alias);
    oct_readonly_options through_parent = { .root = nested, .authorized = authorized };
    CHECK(oct_readonly_backend_create(&through_parent, &backend) != OCT_OK);
    oct_status created = oct_readonly_backend_create(&options, &backend);
    CHECK_OK(created);
    if (created == OCT_OK) backend.ops->destroy(backend.ctx); /* never destroy what was not made */
    unlink(alias);
}

static void test_listing_reading_and_symlinks(void)
{
    char root[ROOT_CAPACITY] = "/tmp/octopus-ro-XXXXXX";
    make_tree(root);
    grant = true;
    oct_readonly_options options = { .root = root, .authorized = authorized };
    oct_store *store = NULL;
    CHECK_OK(oct_readonly_store_open(&options, NULL, &store));
    if (!store) return;

    oct_handle_list list = { 0 };
    CHECK_OK(oct_store_list(store, OCT_ROOT, false, &list));
    /* readme.txt, sub, and the two links whose targets are inside the root. The link out of the
     * root is still refused. Following links that stay inside is what makes /var visible on iOS,
     * where /var, /etc and /tmp are all symlinks and everything of interest is under them. */
    CHECK_EQ(list.count, 4u);
    oct_handle_list_free(&list);
    CHECK(named(store, OCT_ROOT, "link-to-file") != 0);
    CHECK(named(store, OCT_ROOT, "link-to-dir") != 0);
    CHECK_EQ(named(store, OCT_ROOT, "link-outside"), 0u); /* leaves the root: refused */

    /* A link to a directory is browsable, and a link to a file reads the target's bytes. */
    uint32_t linked_dir = named(store, OCT_ROOT, "link-to-dir");
    if (linked_dir) {
        CHECK(named(store, linked_dir, "inner.txt") != 0);
    }
    uint32_t linked_file = named(store, OCT_ROOT, "link-to-file");
    if (linked_file) {
        char through[32] = { 0 };
        size_t read_through = 0;
        CHECK_OK(oct_store_read(store, linked_file, 0, through, sizeof through, &read_through));
        CHECK_EQ(read_through, 16u);
        CHECK(memcmp(through, "0123456789abcdef", 16) == 0);
    }

    uint32_t readme = named(store, OCT_ROOT, "readme.txt");
    CHECK(readme != 0);
    char bytes[32] = { 0 };
    size_t got = 0;
    CHECK_OK(oct_store_read(store, readme, 0, bytes, sizeof bytes, &got));
    CHECK_EQ(got, 16u);
    CHECK(memcmp(bytes, "0123456789abcdef", 16) == 0);
    CHECK_OK(oct_store_read(store, readme, 10, bytes, 4, &got)); /* an offset read */
    CHECK_EQ(got, 4u);
    CHECK(memcmp(bytes, "abcd", 4) == 0);

    uint32_t sub = named(store, OCT_ROOT, "sub");
    uint32_t inner = sub ? named(store, sub, "inner.txt") : 0;
    CHECK(inner != 0);
    CHECK_OK(oct_store_read(store, inner, 0, bytes, sizeof bytes, &got));
    CHECK_EQ(got, 5u);

    /* The whole hierarchy is refused before any traversal. */
    CHECK(oct_store_list(store, OCT_ROOT, true, &list) != OCT_OK);
    CHECK_EQ(list.count, 0u);
    oct_handle_list_free(&list);
    oct_store_close(store);
}

static void test_revocation_is_fail_closed(void)
{
    char root[ROOT_CAPACITY] = "/tmp/octopus-ro-XXXXXX";
    make_tree(root);
    grant = true;
    oct_readonly_options options = { .root = root, .authorized = authorized };
    oct_store *store = NULL;
    CHECK_OK(oct_readonly_store_open(&options, NULL, &store));
    if (!store) return;
    uint32_t readme = named(store, OCT_ROOT, "readme.txt");   /* now cached by the store */
    CHECK(readme != 0);

    grant = false;
    oct_handle_list list = { 0 };
    oct_object_info info;
    char bytes[8];
    size_t got = 0;
    CHECK(oct_store_list(store, OCT_ROOT, false, &list) != OCT_OK);
    CHECK(oct_store_get(store, readme, &info) != OCT_OK);        /* cached metadata too */
    CHECK(oct_store_read(store, readme, 0, bytes, sizeof bytes, &got) != OCT_OK);
    CHECK_EQ(got, 0u);
    CHECK_EQ(oct_store_object_count(store), 0u);
    oct_handle_list_free(&list);

    grant = true; /* authorization restored: service resumes, same handle */
    CHECK_OK(oct_store_get(store, readme, &info));
    CHECK_OK(oct_store_read(store, readme, 0, bytes, sizeof bytes, &got));
    CHECK_EQ(got, 8u);
    oct_store_close(store);
}

static void test_writes_are_refused(void)
{
    char root[ROOT_CAPACITY] = "/tmp/octopus-ro-XXXXXX", path[PATH_CAPACITY];
    make_tree(root);
    grant = true;
    oct_readonly_options options = { .root = root, .authorized = authorized };
    oct_store *store = NULL;
    CHECK_OK(oct_readonly_store_open(&options, NULL, &store));
    if (!store) return;
    uint32_t readme = named(store, OCT_ROOT, "readme.txt");
    uint32_t created = 0;
    CHECK(oct_store_create(store, OCT_ROOT, "new.txt", false, &created) != OCT_OK);
    CHECK(oct_store_write(store, readme, 0, "X", 1) != OCT_OK);
    CHECK(oct_store_rename(store, readme, "renamed.txt") != OCT_OK);
    uint32_t deleted = 0;
    CHECK(oct_store_delete(store, readme, &deleted) != OCT_OK);
    oct_store_close(store);
    /* And nothing changed on disk. */
    snprintf(path, sizeof path, "%s/new.txt", root);
    CHECK(access(path, F_OK) != 0);
    snprintf(path, sizeof path, "%s/readme.txt", root);
    FILE *file = fopen(path, "r");
    char bytes[32] = { 0 };
    CHECK(file != NULL);
    if (file) {
        CHECK_EQ(fread(bytes, 1, sizeof bytes, file), 16u);
        CHECK_EQ(fclose(file), 0);
    }
    CHECK(memcmp(bytes, "0123456789abcdef", 16) == 0);
}

static void test_replaced_inode_is_not_served(void)
{
    char root[ROOT_CAPACITY] = "/tmp/octopus-ro-XXXXXX", path[PATH_CAPACITY], moved[PATH_CAPACITY];
    make_tree(root);
    grant = true;
    oct_readonly_options options = { .root = root, .authorized = authorized };
    oct_store *store = NULL;
    CHECK_OK(oct_readonly_store_open(&options, NULL, &store));
    if (!store) return;
    uint32_t readme = named(store, OCT_ROOT, "readme.txt");
    CHECK(readme != 0);
    /* Swap in a different inode under the same name: the old handle must not retarget to it. */
    snprintf(path, sizeof path, "%s/readme.txt", root);
    snprintf(moved, sizeof moved, "%s/readme.old", root);
    CHECK_EQ(rename(path, moved), 0);
    put(path, "SOMETHING ELSE ENTIRELY");
    char bytes[32] = { 0 };
    size_t got = 0;
    oct_status status = oct_store_read(store, readme, 0, bytes, sizeof bytes, &got);
    CHECK(status != OCT_OK || memcmp(bytes, "SOMETHING", 9) != 0);
    oct_store_close(store);
}

static void test_limits(void)
{
    char root[ROOT_CAPACITY] = "/tmp/octopus-ro-XXXXXX", path[PATH_CAPACITY];
    CHECK(mkdtemp(root) != NULL);
    CHECK(real_path_of(root, ROOT_CAPACITY));
    for (int i = 0; i < 12; i++) {
        snprintf(path, sizeof path, "%s/f%02d", root, i);
        put(path, "x");
    }
    grant = true;
    oct_readonly_options narrow = { .root = root, .authorized = authorized, .max_entries = 8 };
    oct_store *store = NULL;
    CHECK_OK(oct_readonly_store_open(&narrow, NULL, &store));
    if (store) {
        oct_handle_list list = { 0 };
        CHECK(oct_store_list(store, OCT_ROOT, false, &list) != OCT_OK); /* 12 entries, limit 8 */
        oct_handle_list_free(&list);
        oct_store_close(store);
    }
    oct_readonly_options wide = { .root = root, .authorized = authorized, .max_entries = 16 };
    store = NULL;
    CHECK_OK(oct_readonly_store_open(&wide, NULL, &store));
    if (store) {
        oct_handle_list list = { 0 };
        CHECK_OK(oct_store_list(store, OCT_ROOT, false, &list));
        CHECK_EQ(list.count, 12u);
        oct_handle_list_free(&list);
        oct_store_close(store);
    }
    /* Out-of-range limits are refused rather than silently clamped. */
    oct_backend backend;
    oct_readonly_options huge = { .root = root, .authorized = authorized, .max_entries = 5000 };
    CHECK(oct_readonly_backend_create(&huge, &backend) != OCT_OK);
}

/*
 * Symlink traversal, which is what makes a real iOS root usable: /etc, /tmp and /var are all
 * symlinks there, and refusing every one of them left a host seeing eleven of the fifteen entries
 * at / with nothing under the interesting ones. A link is followed only when its target is inside
 * the served root, established by device and inode rather than by comparing path strings.
 */
static void test_symlink_traversal(void)
{
    char root[ROOT_CAPACITY] = "/tmp/octopus-ro-XXXXXX", path[PATH_CAPACITY];
    CHECK(mkdtemp(root) != NULL);
    CHECK(real_path_of(root, ROOT_CAPACITY));
    /* Shaped like iOS: a real directory, and links standing in for /var and /etc. */
    snprintf(path, sizeof path, "%s/private", root);
    CHECK_EQ(mkdir(path, 0755), 0);
    snprintf(path, sizeof path, "%s/private/var", root);
    CHECK_EQ(mkdir(path, 0755), 0);
    snprintf(path, sizeof path, "%s/private/var/mobile", root);
    CHECK_EQ(mkdir(path, 0755), 0);
    snprintf(path, sizeof path, "%s/private/var/mobile/note.txt", root);
    put(path, "user data\n");
    snprintf(path, sizeof path, "%s/var", root);
    CHECK_EQ(symlink("private/var", path), 0);
    snprintf(path, sizeof path, "%s/etc", root);
    CHECK_EQ(symlink("private", path), 0);
    snprintf(path, sizeof path, "%s/escape", root);
    CHECK_EQ(symlink("/usr", path), 0);           /* leaves the root */
    snprintf(path, sizeof path, "%s/dangling", root);
    CHECK_EQ(symlink("nowhere", path), 0);
    snprintf(path, sizeof path, "%s/loop", root);
    CHECK_EQ(symlink("loop", path), 0);

    grant = true;
    oct_readonly_options options = { .root = root, .authorized = authorized, .cross_filesystems = true };
    oct_store *store = NULL;
    CHECK_OK(oct_readonly_store_open(&options, NULL, &store));
    if (!store) return;

    /* The two links inside the root are listed; the escaping, dangling and looping ones are not. */
    uint32_t var = named(store, OCT_ROOT, "var");
    uint32_t etc = named(store, OCT_ROOT, "etc");
    CHECK(var != 0);
    CHECK(etc != 0);
    CHECK_EQ(named(store, OCT_ROOT, "escape"), 0u);
    CHECK_EQ(named(store, OCT_ROOT, "dangling"), 0u);
    CHECK_EQ(named(store, OCT_ROOT, "loop"), 0u);

    /* And a link is browsable through, not merely visible: this is the /var/mobile case. */
    uint32_t mobile = var ? named(store, var, "mobile") : 0;
    CHECK(mobile != 0);
    uint32_t note = mobile ? named(store, mobile, "note.txt") : 0;
    CHECK(note != 0);
    if (note) {
        char bytes[32] = { 0 };
        size_t got = 0;
        CHECK_OK(oct_store_read(store, note, 0, bytes, sizeof bytes, &got));
        CHECK_EQ(got, 10u);
        CHECK(memcmp(bytes, "user data\n", 10) == 0);
    }
    /* Revocation still stops a walk that reaches through a link. */
    grant = false;
    CHECK(oct_store_read(store, note, 0, path, 8, NULL) != OCT_OK);
    grant = true;
    oct_store_close(store);
}

/* The node ceiling a real root needs. The old 65535 was passed within a few directories on a phone
 * holding hundreds of thousands of objects, after which every further listing failed. */
static void test_large_node_ceiling(void)
{
    char root[ROOT_CAPACITY] = "/tmp/octopus-ro-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    CHECK(real_path_of(root, ROOT_CAPACITY));
    grant = true;
    oct_backend backend;
    oct_readonly_options big = { .root = root, .authorized = authorized,
                                 .max_nodes = OCT_READONLY_MAX_NODES };
    CHECK_OK(oct_readonly_backend_create(&big, &backend));
    backend.ops->destroy(backend.ctx);
    /* One past the ceiling is still refused rather than silently clamped. */
    oct_readonly_options past = { .root = root, .authorized = authorized,
                                  .max_nodes = OCT_READONLY_MAX_NODES + 1 };
    CHECK(oct_readonly_backend_create(&past, &backend) != OCT_OK);
}

/*
 * A real file downloaded the way a host does it: GetObjectInfo then GetObject, over the responder,
 * from a file reached through a symlinked directory. Listing a real root was proven on hardware;
 * reading one over the protocol was not, and it travels a different path, pread on a real
 * descriptor rather than a copy of bytes already in memory.
 */
static void test_real_file_downloads_over_the_protocol(void)
{
    char root[ROOT_CAPACITY] = "/tmp/octopus-ro-XXXXXX", path[PATH_CAPACITY];
    CHECK(mkdtemp(root) != NULL);
    CHECK(real_path_of(root, ROOT_CAPACITY));
    snprintf(path, sizeof path, "%s/private", root);
    CHECK_EQ(mkdir(path, 0755), 0);
    snprintf(path, sizeof path, "%s/private/var", root);
    CHECK_EQ(mkdir(path, 0755), 0);
    /* Larger than one transfer chunk, so the streaming path is exercised rather than a single read. */
    const size_t size = 300u * 1024u;
    snprintf(path, sizeof path, "%s/private/var/photo.jpg", root);
    FILE *file = fopen(path, "wb");
    CHECK(file != NULL);
    if (file) {
        bool written = true;
        for (size_t i = 0; i < size; i++) {
            if (fputc((int)((i * 31u + (i >> 11)) & 0xFF), file) == EOF) { written = false; break; }
        }
        CHECK(written);
        CHECK_EQ(fclose(file), 0);
    }
    snprintf(path, sizeof path, "%s/var", root);
    CHECK_EQ(symlink("private/var", path), 0);

    grant = true;
    oct_store_options storeOptions = { .storage_id = 0x4f430001u, .first_handle = 0x7f000001u,
                                       .description = "Octopus (read-only)", .volume_label = "Octopus",
                                       .max_objects = OCT_READONLY_MAX_NODES };
    oct_readonly_options options = { .root = root, .authorized = authorized,
                                     .max_nodes = OCT_READONLY_MAX_NODES, .cross_filesystems = true };
    oct_store *store = NULL;
    oct_responder *responder = NULL;
    CHECK_OK(oct_readonly_store_open(&options, &storeOptions, &store));
    if (!store) return;
    CHECK_OK(oct_responder_create(store, NULL, &responder));
    if (!responder) { oct_store_close(store); return; }

    oct_buf reply;
    oct_buf_init(&reply, 1u << 20);
    oct_exchange exchange = { 0 };
    uint32_t session = 1;
    CHECK_OK(oct_exchange_run(responder, OCT_OP_OPEN_SESSION, 1, &session, 1, false, NULL, 0,
                              &reply, &exchange));

    uint32_t var = named(store, OCT_ROOT, "var");
    uint32_t photo = var ? named(store, var, "photo.jpg") : 0;
    CHECK(photo != 0);
    if (photo) {
        CHECK_OK(oct_exchange_run(responder, OCT_OP_GET_OBJECT_INFO, 2, &photo, 1, false, NULL, 0,
                                  &reply, &exchange));
        CHECK_EQ(exchange.response_code, OCT_RC_OK);
        oct_object_info_request decoded = { 0 };
        CHECK(exchange.has_data);
        if (exchange.has_data) {
            CHECK_OK(oct_dataset_parse_object_info(exchange.data, exchange.data_length, &decoded));
            CHECK_EQ(decoded.compressed_size, (uint32_t)size);
            CHECK_EQ(decoded.format, OCT_FORMAT_UNDEFINED);
            CHECK(strcmp(decoded.name, "photo.jpg") == 0);
        }
        CHECK_OK(oct_exchange_run(responder, OCT_OP_GET_OBJECT, 3, &photo, 1, false, NULL, 0,
                                  &reply, &exchange));
        CHECK_EQ(exchange.response_code, OCT_RC_OK);
        CHECK(exchange.has_data);
        CHECK_EQ(exchange.data_length, size);
        if (exchange.has_data && exchange.data_length == size) {
            uint32_t wrong = 0;
            for (size_t i = 0; i < size; i++) {
                if (exchange.data[i] != (uint8_t)((i * 31u + (i >> 11)) & 0xFF)) wrong++;
            }
            CHECK_EQ(wrong, 0u); /* byte-exact, not merely the right length */
        }
        /* Revocation stops a download as surely as a listing. */
        grant = false;
        oct_exchange denied = { 0 };
        oct_exchange_run(responder, OCT_OP_GET_OBJECT, 4, &photo, 1, false, NULL, 0, &reply, &denied);
        CHECK(denied.response_code != OCT_RC_OK);
        grant = true;
    }
    oct_buf_free(&reply);
    oct_responder_destroy(responder);
    oct_store_close(store);
}

/*
 * A directory the sandbox will not list, with readable children beneath it. On iOS /var/mobile is
 * exactly this: listing it is refused while /var/mobile/Media is readable, so a host browsing from /
 * found it empty and never reached the photos under it. Its reachable children are now grafted in.
 *
 * Reproduced with a mode 0111 directory, which can be searched but not read, as a user who is
 * subject to permissions. Root is not, so as root the check runs in a child that has given them up.
 */
static int graft_scenario(const char *root)
{
    int failures = 0;
#define GRAFT_EXPECT(c) do { if (!(c)) { fprintf(stderr, "graft: %s failed\n", #c); failures++; } } while (0)
    char path[PATH_CAPACITY];
    snprintf(path, sizeof path, "%s/private/var/mobile", root);
    int direct = open(path, O_RDONLY | O_DIRECTORY);
    GRAFT_EXPECT(direct < 0);                     /* the precondition: not listable */
    if (direct >= 0) close(direct);

    grant = true;
    oct_readonly_options options = { .root = root, .authorized = authorized, .cross_filesystems = true };
    oct_store *store = NULL;
    if (oct_readonly_store_open(&options, NULL, &store) != OCT_OK || !store) return failures + 1;
    uint32_t var = named(store, OCT_ROOT, "var");
    uint32_t mobile = var ? named(store, var, "mobile") : 0;
    GRAFT_EXPECT(mobile != 0);
    uint32_t media = mobile ? named(store, mobile, "Media") : 0;
    GRAFT_EXPECT(media != 0);                     /* grafted in */
    GRAFT_EXPECT(!mobile || named(store, mobile, "Secret") == 0); /* not a candidate: stays hidden */
    uint32_t dcim = media ? named(store, media, "DCIM") : 0;
    GRAFT_EXPECT(dcim != 0);                      /* and browsable below the graft */
    uint32_t photo = dcim ? named(store, dcim, "IMG_0001.JPG") : 0;
    GRAFT_EXPECT(photo != 0);
    if (photo) {
        char bytes[32] = { 0 };
        size_t got = 0;
        GRAFT_EXPECT(oct_store_read(store, photo, 0, bytes, sizeof bytes, &got) == OCT_OK);
        GRAFT_EXPECT(got == 11 && memcmp(bytes, "jpeg bytes\n", 11) == 0);
    }
    oct_store_close(store);
#undef GRAFT_EXPECT
    return failures;
}

static void test_graft_beneath_denied_directory(void)
{
    char root[ROOT_CAPACITY] = "/tmp/octopus-ro-XXXXXX", path[PATH_CAPACITY];
    CHECK(mkdtemp(root) != NULL);
    CHECK(real_path_of(root, ROOT_CAPACITY));
    static const char *const dirs[] = { "private", "private/var", "private/var/mobile",
                                        "private/var/mobile/Media", "private/var/mobile/Media/DCIM",
                                        "private/var/mobile/Secret" };
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0]; i++) {
        snprintf(path, sizeof path, "%s/%s", root, dirs[i]);
        CHECK_EQ(mkdir(path, 0755), 0);
    }
    snprintf(path, sizeof path, "%s/private/var/mobile/Media/DCIM/IMG_0001.JPG", root);
    put(path, "jpeg bytes\n");
    snprintf(path, sizeof path, "%s/var", root);
    CHECK_EQ(symlink("private/var", path), 0);
    /* Everyone may read the tree, then mobile becomes search-only: the iOS shape. */
    CHECK_EQ(chmod(root, 0755), 0);
    snprintf(path, sizeof path, "%s/private/var/mobile", root);
    CHECK_EQ(chmod(path, 0111), 0);

    if (geteuid() != 0) {
        CHECK_EQ(graft_scenario(root), 0);
    } else {
        pid_t child = fork();
        CHECK(child >= 0);
        if (child == 0) {
            /* Become someone permissions apply to; without that the precondition cannot hold. */
            if (setgid(65534) != 0 || setuid(65534) != 0) _exit(2);
            _exit(graft_scenario(root) == 0 ? 0 : 1);
        }
        int status = 0;
        CHECK_EQ(waitpid(child, &status, 0), child);
        CHECK(WIFEXITED(status));
        CHECK_EQ(WEXITSTATUS(status), 0);
    }
    snprintf(path, sizeof path, "%s/private/var/mobile", root);
    CHECK_EQ(chmod(path, 0755), 0); /* so the temporary tree can be removed */
}

/*
 * Writes, which are off unless asked for. A host copying a file onto the phone needs create, then
 * write, and Explorer deletes through DeleteObject. Rename works too, once the identity it
 * changes is re-recorded.
 */
static void test_writes(void)
{
    char root[ROOT_CAPACITY] = "/tmp/octopus-ro-XXXXXX", path[PATH_CAPACITY];
    CHECK(mkdtemp(root) != NULL);
    CHECK(real_path_of(root, ROOT_CAPACITY));
    grant = true;

    /* Read-only by default, so no existing caller starts writing by accident. */
    oct_readonly_options readonly = { .root = root, .authorized = authorized };
    oct_store *store = NULL;
    CHECK_OK(oct_readonly_store_open(&readonly, NULL, &store));
    if (store) {
        uint32_t refused = 0;
        CHECK_EQ(oct_store_create(store, OCT_ROOT, "nope.txt", false, &refused), OCT_E_WRITE_PROTECTED);
        oct_store_close(store);
        store = NULL;
    }

    oct_readonly_options writable = { .root = root, .authorized = authorized, .writable = true,
                                     .max_read_bytes = 1u << 20 };
    CHECK_OK(oct_readonly_store_open(&writable, NULL, &store));
    if (!store) return;

    /* The storage says read-write, which is what a host reads in GetStorageInfo. */
    oct_storage_info storage = { 0 };
    CHECK_OK(oct_store_storage_info(store, &storage));
    CHECK_EQ(storage.access_capability, 0x0000);

    uint32_t file = 0;
    CHECK_OK(oct_store_create(store, OCT_ROOT, "hello.txt", false, &file));
    CHECK(file != 0);
    const char *text = "written over USB\n";
    const size_t length = strlen(text);
    CHECK_OK(oct_store_write(store, file, 0, text, length));

    /* Read back through the store, and independently off the filesystem. */
    char back[64] = { 0 };
    size_t got = 0;
    CHECK_OK(oct_store_read(store, file, 0, back, sizeof back, &got));
    CHECK_EQ(got, length);
    if (got == length) CHECK(memcmp(back, text, length) == 0);
    snprintf(path, sizeof path, "%s/hello.txt", root);
    FILE *opened = fopen(path, "rb");
    CHECK(opened != NULL);
    if (opened) {
        char disk[64] = { 0 };
        size_t read_bytes = fread(disk, 1, sizeof disk, opened);
        fclose(opened);
        CHECK_EQ(read_bytes, length);
        if (read_bytes == length) CHECK(memcmp(disk, text, length) == 0);
    }

    /* A write at an offset, which a host does when sending a file in pieces. */
    CHECK_OK(oct_store_write(store, file, 4, "XXXX", 4));
    CHECK_OK(oct_store_read(store, file, 0, back, sizeof back, &got));
    CHECK_EQ(got, length);
    if (got == length) CHECK(memcmp(back, "writXXXXover USB\n", length) == 0);
    /* Truncating, then growing again, leaves the handle usable: our own change is re-recorded. */
    CHECK_OK(oct_store_truncate(store, file, 4));
    CHECK_OK(oct_store_read(store, file, 0, back, sizeof back, &got));
    CHECK_EQ(got, 4u);

    /* Directories, nesting, and delete. */
    uint32_t folder = 0, inner = 0;
    CHECK_OK(oct_store_create(store, OCT_ROOT, "folder", true, &folder));
    CHECK_OK(oct_store_create(store, folder, "inner.bin", false, &inner));
    CHECK_OK(oct_store_write(store, inner, 0, "abc", 3));
    uint32_t deleted = 0;
    CHECK_OK(oct_store_delete(store, inner, &deleted));
    snprintf(path, sizeof path, "%s/folder/inner.bin", root);
    CHECK(access(path, F_OK) != 0); /* really gone from the filesystem */

    /* Refusals. A duplicate, a traversal, a separator, writing to a directory, and a lapsed
     * lease, which must stop a write as surely as it stops a read. */
    uint32_t again = 0;
    CHECK_EQ(oct_store_create(store, OCT_ROOT, "hello.txt", false, &again), OCT_E_EXISTS);
    CHECK(oct_store_create(store, OCT_ROOT, "../escape", false, &again) != OCT_OK);
    CHECK(oct_store_create(store, OCT_ROOT, "a/b", false, &again) != OCT_OK);
    CHECK_EQ(oct_store_write(store, folder, 0, "x", 1), OCT_E_IS_DIRECTORY);
    /* Rename, which changes ctime and so needs the same identity re-recording a write does: the
     * rename succeeded and every later call on the handle reported it invalid until that was
     * fixed. The handle must keep working and the listing must show the new name, not lose both. */
    CHECK_OK(oct_store_rename(store, file, "other.txt"));
    oct_object_info renamed = { 0 };
    CHECK_OK(oct_store_get(store, file, &renamed));
    CHECK(strcmp(renamed.name, "other.txt") == 0);
    CHECK(named(store, OCT_ROOT, "other.txt") != 0);
    CHECK_EQ(named(store, OCT_ROOT, "hello.txt"), 0u);
    CHECK_OK(oct_store_read(store, file, 0, back, sizeof back, &got)); /* still readable */
    /* Renaming onto a name already taken is refused rather than replacing it silently. */
    uint32_t other = 0;
    CHECK_OK(oct_store_create(store, OCT_ROOT, "taken.txt", false, &other));
    CHECK(oct_store_rename(store, file, "taken.txt") != OCT_OK);
    CHECK(named(store, OCT_ROOT, "other.txt") != 0); /* unchanged after the refusal */
    CHECK(oct_store_rename(store, file, "a/b") != OCT_OK);
    CHECK(oct_store_rename(store, file, "..") != OCT_OK);
    grant = false;
    CHECK_EQ(oct_store_create(store, OCT_ROOT, "after-revoke", false, &again), OCT_E_STATE);
    CHECK(oct_store_write(store, file, 0, "x", 1) != OCT_OK);
    CHECK(oct_store_delete(store, file, &deleted) != OCT_OK);
    grant = true;
    oct_store_close(store);
}

void run_readonly_tests(void)
{
    test_creation_is_guarded();
    test_listing_reading_and_symlinks();
    test_revocation_is_fail_closed();
    test_writes_are_refused();
    test_replaced_inode_is_not_served();
    test_limits();
    test_symlink_traversal();
    test_large_node_ceiling();
    test_real_file_downloads_over_the_protocol();
    test_graft_beneath_denied_directory();
    test_writes();
}
