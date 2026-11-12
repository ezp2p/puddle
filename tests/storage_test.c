#include "storage/storage.h"

#include "platform/fsutil.h"

#include <errno.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s [%s]\n", __FILE__, __LINE__, #cond, driver); failures++; } } while (0)

static const char *driver;
static int rm_cb(const char *p, const struct stat *sb, int t, struct FTW *f) { (void)sb; (void)t; (void)f; remove(p); return 0; }

typedef struct { int n; char keys[8][64]; } seen;
static int collect(void *user, const char *key, const char *data, size_t len)
{
    seen *s = user;
    (void)data; (void)len;
    if (s->n < 8) snprintf(s->keys[s->n++], 64, "%s", key);
    return 0;
}
static int stop_at_first(void *user, const char *key, const char *data, size_t len) { (void)key; (void)data; (void)len; ++*(int *)user; return 1; }
static int has(const seen *s, const char *key) { for (int i = 0; i < s->n; i++) if (!strcmp(s->keys[i], key)) return 1; return 0; }

/* The same checks for every driver: what one keeps, the other keeps the same way. */
static void contract(puddle_storage *s)
{
    char *data;
    size_t len;
    CHECK(puddle_record_get(s, "streams", "alice", &data, &len) == 1 && data == NULL);           /* nothing yet */
    CHECK(puddle_record_put(s, "streams", "alice", "{\"a\":1}", 7) == 0);
    CHECK(puddle_record_get(s, "streams", "alice", &data, &len) == 0 && len == 7 && !strcmp(data, "{\"a\":1}"));
    free(data);
    CHECK(puddle_record_put(s, "streams", "alice", "{\"a\":2}", 7) == 0);                         /* replaced */
    CHECK(puddle_record_get(s, "streams", "alice", &data, &len) == 0 && !strcmp(data, "{\"a\":2}"));
    free(data);
    const char bin[5] = { 'x', 0, 'y', 0, 'z' };                                                 /* bytes, not text */
    CHECK(puddle_record_put(s, "groups", "g1", bin, 5) == 0);
    CHECK(puddle_record_get(s, "groups", "g1", &data, &len) == 0 && len == 5 && !memcmp(data, bin, 5) && data[5] == 0);
    free(data);
    CHECK(puddle_record_put(s, "streams", "bob", "{}", 2) == 0);
    seen all = { 0 };
    CHECK(puddle_record_each(s, "streams", collect, &all) == 0 && all.n == 2 && has(&all, "alice") && has(&all, "bob"));
    int calls = 0;
    CHECK(puddle_record_each(s, "streams", stop_at_first, &calls) == 0 && calls == 1);         /* stops when asked */
    seen none = { 0 };
    CHECK(puddle_record_each(s, "never_used", collect, &none) == 0 && none.n == 0);
    CHECK(puddle_record_remove(s, "streams", "bob") == 0 && puddle_record_get(s, "streams", "bob", &data, &len) == 1);
    CHECK(puddle_record_remove(s, "streams", "bob") == 0);                                      /* again: fine */
    /* single documents */
    CHECK(puddle_record_put(s, "state", "blocks.tsv", "a\t1\n", 4) == 0);
    CHECK(puddle_record_get(s, "state", "blocks.tsv", &data, &len) == 0 && len == 4);
    free(data);
    /* names that could reach outside their place are refused */
    const char *bad[] = { "", "../x", "a/b", ".hidden", "a b", "x\\y" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        errno = 0;
        CHECK(puddle_record_put(s, "streams", bad[i], "x", 1) == -1 && errno == EINVAL);
        CHECK(puddle_record_put(s, bad[i], "k", "x", 1) == -1);
    }
    /* secrets */
    CHECK(puddle_secret_get(s, "owner-token", &data, &len) == 1);
    CHECK(puddle_secret_put(s, "owner-token", "abc", 3) == 0);
    CHECK(puddle_secret_get(s, "owner-token", &data, &len) == 0 && !strcmp(data, "abc"));
    free(data);
}

int main(void)
{
    const char *base = getenv("TMPDIR");
    char root[512], path[700], err[300];
    snprintf(root, sizeof root, "%s/puddle-storage-XXXXXX", base ? base : "/tmp");
    if (!mkdtemp(root)) return 2;

    driver = "memory";
    puddle_storage *m = puddle_storage_open_memory();
    contract(m);
    puddle_storage_close(m);

    driver = "files";
    puddle_storage *f = puddle_storage_open_files(root, err, sizeof err);
    CHECK(f != NULL);
    contract(f);
    /* where the files driver puts things: the layout docs/storage.md describes */
    snprintf(path, sizeof path, "%s/state/streams/alice.json", root);
    CHECK(puddle_path_is_file(path));
    snprintf(path, sizeof path, "%s/state/groups/g1.json", root);
    CHECK(puddle_path_is_file(path));
    snprintf(path, sizeof path, "%s/state/blocks.tsv", root);
    CHECK(puddle_path_is_file(path));
    snprintf(path, sizeof path, "%s/state/owner-token", root);
    struct stat st;
    CHECK(stat(path, &st) == 0 && (st.st_mode & 077) == 0);                                     /* owner-only */
    /* what is kept survives closing and opening again */
    puddle_storage_close(f);
    f = puddle_storage_open_files(root, err, sizeof err);
    char *data;
    size_t len;
    CHECK(puddle_record_get(f, "streams", "alice", &data, &len) == 0 && !strcmp(data, "{\"a\":2}"));
    free(data);
    /* a stray temp file is not a record */
    snprintf(path, sizeof path, "%s/state/streams/.tmp-1-1", root);
    FILE *t = fopen(path, "w");
    if (t) { fputs("junk", t); fclose(t); }
    seen all = { 0 };
    CHECK(puddle_record_each(f, "streams", collect, &all) == 0 && all.n == 1 && has(&all, "alice"));
    puddle_storage_close(f);

    nftw(root, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    if (failures) fprintf(stderr, "%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
