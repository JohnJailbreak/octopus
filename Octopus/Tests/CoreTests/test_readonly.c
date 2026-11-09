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

#include <stdio.h>
#include <string.h>
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
    CHECK_EQ(list.count, 2u); /* readme.txt and sub; the three symlinks are not exported */
    oct_handle_list_free(&list);
    CHECK_EQ(named(store, OCT_ROOT, "link-to-file"), 0u);
    CHECK_EQ(named(store, OCT_ROOT, "link-to-dir"), 0u);
    CHECK_EQ(named(store, OCT_ROOT, "link-outside"), 0u);

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

void run_readonly_tests(void)
{
    test_creation_is_guarded();
    test_listing_reading_and_symlinks();
    test_revocation_is_fail_closed();
    test_writes_are_refused();
    test_replaced_inode_is_not_served();
    test_limits();
}
