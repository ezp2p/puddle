#include "storage/store.h"

#include <ftw.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <openssl/evp.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static char root[512];

static int rm_cb(const char *p, const struct stat *sb, int t, struct FTW *f)
{
    (void)sb; (void)t; (void)f;
    remove(p);
    return 0;
}

static void cleanup(void) { nftw(root, rm_cb, 16, FTW_DEPTH | FTW_PHYS); }

static char *dir_for(const char *name)
{
    static char path[600];
    snprintf(path, sizeof path, "%s/%s", root, name);
    return path;
}

/* UTC milliseconds for 2026-10-<day> <hour>:00 */
static int64_t ts_of(int day, int hour)
{
    struct tm tm = { .tm_year = 2026 - 1900, .tm_mon = 9, .tm_mday = day, .tm_hour = hour };
    return (int64_t)timegm(&tm) * 1000;
}

static int append(puddle_store *s, const char *chat, int64_t ts, const char *body, char *id)
{
    char ev[512], err[256];
    int n = snprintf(ev, sizeof ev,
                     "{\"type\":\"message\",\"chat\":\"%s\",\"sender\":\"u1\",\"ts\":%lld,\"body\":\"%s\"}",
                     chat, (long long)ts, body);
    return puddle_store_append(s, chat, ev, (size_t)n, "c2ln", 0, id, err, sizeof err);
}

static void sha256_hex(const char *data, size_t len, char out[65])
{
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned mdlen;
    EVP_Digest(data, len, md, &mdlen, EVP_sha256(), NULL);
    for (unsigned i = 0; i < mdlen; i++)
        sprintf(out + 2 * i, "%02x", md[i]);
}

static off_t file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? st.st_size : -1;
}

static void test_basics_and_persistence(void)
{
    char err[256], id[65], id2[65];
    puddle_store *s = puddle_store_open(dir_for("basic"), err, sizeof err);
    CHECK(s != NULL);

    char idA[65], idB[65], idC[65];
    CHECK(append(s, "c1", ts_of(14, 12), "A", idA) == PUDDLE_APPEND_NEW);
    CHECK(append(s, "c1", ts_of(14, 13), "B", idB) == PUDDLE_APPEND_NEW);
    CHECK(append(s, "c1", ts_of(13, 9), "late", idC) == PUDDLE_APPEND_NEW); /* old ts arrives last */
    CHECK(append(s, "c1", ts_of(14, 12), "A", id) == PUDDLE_APPEND_DUPLICATE);
    CHECK(strcmp(id, idA) == 0);

    /* the id is the SHA-256 of the event bytes */
    char ev[512], expect[65];
    int n = snprintf(ev, sizeof ev,
                     "{\"type\":\"message\",\"chat\":\"c1\",\"sender\":\"u1\",\"ts\":%lld,\"body\":\"A\"}",
                     (long long)ts_of(14, 12));
    sha256_hex(ev, (size_t)n, expect);
    CHECK(strcmp(idA, expect) == 0);

    /* each UTC day is its own file, placed by the sender's ts */
    char path[700];
    snprintf(path, sizeof path, "%s/chats/c1/2026-10-14.jsonl", dir_for("basic"));
    CHECK(file_size(path) > 0);
    snprintf(path, sizeof path, "%s/chats/c1/2026-10-13.jsonl", dir_for("basic"));
    CHECK(file_size(path) > 0);

    /* chat order is (ts, id), not arrival order */
    puddle_event_list l;
    CHECK(puddle_store_tail(s, "c1", 10, &l, err, sizeof err) == 0);
    CHECK(l.count == 3);
    CHECK(strcmp(l.items[0].id, idC) == 0 && strcmp(l.items[1].id, idA) == 0 && strcmp(l.items[2].id, idB) == 0);
    puddle_event_list_free(&l);
    CHECK(puddle_store_tail(s, "c1", 2, &l, err, sizeof err) == 0 && l.count == 2);
    CHECK(strcmp(l.items[0].id, idA) == 0 && strcmp(l.items[1].id, idB) == 0);
    puddle_event_list_free(&l);
    puddle_store_close(s);

    /* reopen: everything is still there, and tail does not load the chat */
    s = puddle_store_open(dir_for("basic"), err, sizeof err);
    CHECK(s != NULL);
    CHECK(puddle_store_tail(s, "c1", 100, &l, err, sizeof err) == 0 && l.count == 3);
    CHECK(strcmp(l.items[0].id, idC) == 0);
    CHECK(strcmp(l.items[0].sig, "c2ln") == 0 && l.items[0].rts > 0);
    puddle_event_list_free(&l);
    CHECK(puddle_store_loaded_chats(s) == 0);

    /* dedupe survives a reload */
    CHECK(append(s, "c1", ts_of(14, 13), "B", id2) == PUDDLE_APPEND_DUPLICATE);
    CHECK(puddle_store_loaded_chats(s) == 1);

    puddle_event e;
    CHECK(puddle_store_get(s, "c1", idB, &e, err, sizeof err) == 0);
    CHECK(strstr(e.event, "\"body\":\"B\"") != NULL);
    puddle_event_free(&e);
    CHECK(puddle_store_get(s, "c1", expect, &e, err, sizeof err) == 0); /* A again, by recomputed hash */
    puddle_event_free(&e);
    CHECK(puddle_store_get(s, "c1", "00000000000000000000000000000000000000000000000000000000000000ff",
                           &e, err, sizeof err) == 1);
    CHECK(puddle_store_get(s, "nochat", idB, &e, err, sizeof err) == 1);

    /* unknown chat: empty, and nothing is created */
    CHECK(puddle_store_tail(s, "nochat", 5, &l, err, sizeof err) == 0 && l.count == 0);
    puddle_chunk_list cl;
    CHECK(puddle_store_chunks(s, "nochat", &cl, err, sizeof err) == 0 && cl.count == 0);
    snprintf(path, sizeof path, "%s/chats/nochat", dir_for("basic"));
    CHECK(access(path, F_OK) != 0);
    puddle_store_close(s);
}

static void test_lock(void)
{
    char err[256];
    puddle_store *a = puddle_store_open(dir_for("lock"), err, sizeof err);
    CHECK(a != NULL);
    puddle_store *b = puddle_store_open(dir_for("lock"), err, sizeof err);
    CHECK(b == NULL && strstr(err, "in use") != NULL);
    puddle_store_close(a);
    b = puddle_store_open(dir_for("lock"), err, sizeof err); /* released on close */
    CHECK(b != NULL);
    puddle_store_close(b);
}

static void test_torn_tail(void)
{
    char err[256], id[65];
    puddle_store *s = puddle_store_open(dir_for("torn"), err, sizeof err);
    CHECK(append(s, "t1", ts_of(14, 1), "one", id) == PUDDLE_APPEND_NEW);
    CHECK(append(s, "t1", ts_of(14, 2), "two", id) == PUDDLE_APPEND_NEW);
    puddle_store_close(s);

    char path[700];
    snprintf(path, sizeof path, "%s/chats/t1/2026-10-14.jsonl", dir_for("torn"));
    off_t good = file_size(path);
    FILE *f = fopen(path, "a");
    fputs("{\"id\":\"deadbeef\",\"rts\":1,\"event\":{\"cha", f); /* a write cut short by a crash */
    fclose(f);
    CHECK(file_size(path) > good);

    /* tail reads around the damage without touching it */
    puddle_event_list l;
    s = puddle_store_open(dir_for("torn"), err, sizeof err);
    CHECK(puddle_store_tail(s, "t1", 10, &l, err, sizeof err) == 0 && l.count == 2);
    puddle_event_list_free(&l);
    CHECK(file_size(path) > good);

    /* loading the chat repairs it, and appending afterwards works */
    CHECK(append(s, "t1", ts_of(14, 3), "three", id) == PUDDLE_APPEND_NEW);
    CHECK(puddle_store_tail(s, "t1", 10, &l, err, sizeof err) == 0 && l.count == 3);
    puddle_event_list_free(&l);
    puddle_store_close(s);

    s = puddle_store_open(dir_for("torn"), err, sizeof err);
    CHECK(puddle_store_tail(s, "t1", 10, &l, err, sizeof err) == 0 && l.count == 3);
    puddle_event_list_free(&l);
    puddle_store_close(s);
}

static void test_corrupt_middle(void)
{
    char err[256], id[65];
    puddle_store *s = puddle_store_open(dir_for("corrupt"), err, sizeof err);
    append(s, "k1", ts_of(14, 1), "first", id);
    append(s, "k1", ts_of(14, 2), "second", id);
    append(s, "k1", ts_of(14, 3), "third", id);
    puddle_store_close(s);

    char path[700];
    snprintf(path, sizeof path, "%s/chats/k1/2026-10-14.jsonl", dir_for("corrupt"));
    FILE *f = fopen(path, "r+");
    char buf[4096];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = '\0';
    char *p = strstr(buf, "second");
    CHECK(p != NULL);
    memcpy(p, "seXond", 6); /* same length, so only the hash notices */
    rewind(f);
    fwrite(buf, 1, n, f);
    fclose(f);

    s = puddle_store_open(dir_for("corrupt"), err, sizeof err);
    puddle_event_list l;
    CHECK(puddle_store_tail(s, "k1", 10, &l, err, sizeof err) == 0 && l.count == 2);
    CHECK(strstr(l.items[0].event, "first") && strstr(l.items[1].event, "third"));
    puddle_event_list_free(&l);
    CHECK(file_size(path) == (off_t)n); /* the bad record is skipped, never deleted */
    puddle_store_close(s);
}

static void test_chunks(void)
{
    char err[256], id[65];
    puddle_store *a = puddle_store_open(dir_for("chunk-a"), err, sizeof err);
    puddle_store *b = puddle_store_open(dir_for("chunk-b"), err, sizeof err);
    puddle_store *c = puddle_store_open(dir_for("chunk-c"), err, sizeof err);

    /* same events, different arrival order and receive times */
    append(a, "x1", ts_of(13, 5), "m1", id);
    append(a, "x1", ts_of(14, 5), "m2", id);
    append(a, "x1", ts_of(14, 6), "m3", id);
    append(b, "x1", ts_of(14, 6), "m3", id);
    append(b, "x1", ts_of(13, 5), "m1", id);
    append(b, "x1", ts_of(14, 5), "m2", id);
    /* c is missing m3 */
    append(c, "x1", ts_of(13, 5), "m1", id);
    append(c, "x1", ts_of(14, 5), "m2", id);

    puddle_chunk_list la, lb, lc;
    CHECK(puddle_store_chunks(a, "x1", &la, err, sizeof err) == 0);
    CHECK(puddle_store_chunks(b, "x1", &lb, err, sizeof err) == 0);
    CHECK(puddle_store_chunks(c, "x1", &lc, err, sizeof err) == 0);
    CHECK(la.count == 2 && lb.count == 2 && lc.count == 2);
    CHECK(strcmp(la.items[0].day, "2026-10-13") == 0 && strcmp(la.items[1].day, "2026-10-14") == 0);
    CHECK(la.items[0].count == 1 && la.items[1].count == 2 && lc.items[1].count == 1);
    for (size_t i = 0; i < 2; i++)
        CHECK(strcmp(la.items[i].hash, lb.items[i].hash) == 0); /* servers agree */
    CHECK(strcmp(la.items[0].hash, lc.items[0].hash) == 0);     /* untouched day matches */
    CHECK(strcmp(la.items[1].hash, lc.items[1].hash) != 0);     /* only the differing day differs */

    puddle_chunk_list_free(&la);
    puddle_chunk_list_free(&lb);
    puddle_chunk_list_free(&lc);
    puddle_store_close(a);
    puddle_store_close(b);
    puddle_store_close(c);
}

static void test_validation(void)
{
    char err[256], id[65], ev[1200];
    puddle_store *s = puddle_store_open(dir_for("valid"), err, sizeof err);
    int64_t ts = ts_of(14, 1);
#define APPEND_RAW(chat_arg, ...) (snprintf(ev, sizeof ev, __VA_ARGS__), \
        puddle_store_append(s, chat_arg, ev, strlen(ev), NULL, 0, id, err, sizeof err))

    CHECK(APPEND_RAW("v1", "{\"chat\":\"v1\",\"ts\":%lld}", (long long)ts) == PUDDLE_APPEND_NEW);

    /* chat ids become directory names */
    CHECK(APPEND_RAW("../evil", "{\"chat\":\"../evil\",\"ts\":%lld}", (long long)ts) == -1);
    CHECK(APPEND_RAW("a/b", "{\"chat\":\"a/b\",\"ts\":%lld}", (long long)ts) == -1);
    CHECK(APPEND_RAW("", "{\"chat\":\"\",\"ts\":%lld}", (long long)ts) == -1);
    CHECK(APPEND_RAW(".", "{\"chat\":\".\",\"ts\":%lld}", (long long)ts) == -1);

    /* shape of the event */
    CHECK(APPEND_RAW("v1", "[1]") == -1);
    CHECK(APPEND_RAW("v1", "{\"chat\":\"v1\"}") == -1);                              /* no ts */
    CHECK(APPEND_RAW("v1", "{\"ts\":%lld}", (long long)ts) == -1);                   /* no chat */
    CHECK(APPEND_RAW("v1", "{\"chat\":\"other\",\"ts\":%lld}", (long long)ts) == -1); /* wrong chat */
    CHECK(APPEND_RAW("v1", "{\"chat\":\"v1\",\"ts\":0}") == -1);                     /* year 1970 */
    CHECK(APPEND_RAW("v1", "{\"chat\":\"v1\",\"ts\":99999999999999}") == -1);        /* year 5000+ */
    CHECK(APPEND_RAW("v1", "{\"chat\":\"v1\",\"ts\":\"%lld\"}", (long long)ts) == -1); /* ts as string */
    CHECK(APPEND_RAW("v1", "{\"chat\":\"v1\",\"ts\":%lld,\"ts\":%lld}", (long long)ts, (long long)ts + 1) == -1);
    CHECK(APPEND_RAW("v1", "{\"chat\":\"v1\",\"ts\":%lld,\n\"x\":1}", (long long)ts) == -1); /* line break */

    /* text inside a string must not be mistaken for fields */
    CHECK(APPEND_RAW("v1", "{\"body\":\"\\\"chat\\\":\\\"zzz\\\",\\\"ts\\\":5\",\"chat\":\"v1\",\"ts\":%lld}",
                     (long long)ts + 5) == PUDDLE_APPEND_NEW);

    /* signature must be base64 */
    snprintf(ev, sizeof ev, "{\"chat\":\"v1\",\"ts\":%lld,\"n\":1}", (long long)ts);
    CHECK(puddle_store_append(s, "v1", ev, strlen(ev), "bad\"sig", 0, id, err, sizeof err) == -1);

    puddle_event_list l;
    CHECK(puddle_store_tail(s, "v1", 100, &l, err, sizeof err) == 0 && l.count == 2);
    puddle_event_list_free(&l);
    CHECK(puddle_store_tail(s, "../x", 5, &l, err, sizeof err) == -1);
    puddle_store_close(s);
}

static void test_many_chats(void)
{
    char err[256], id[65], chat[32];
    puddle_store *s = puddle_store_open(dir_for("many"), err, sizeof err);
    for (int i = 0; i < 70; i++) {
        snprintf(chat, sizeof chat, "chat%d", i);
        CHECK(append(s, chat, ts_of(14, 1), "hi", id) == PUDDLE_APPEND_NEW);
    }
    CHECK(puddle_store_loaded_chats(s) <= 64); /* least recently used chats are dropped from memory */
    puddle_event_list l;
    CHECK(puddle_store_tail(s, "chat0", 5, &l, err, sizeof err) == 0 && l.count == 1); /* still on disk */
    puddle_event_list_free(&l);
    CHECK(append(s, "chat0", ts_of(14, 1), "hi", id) == PUDDLE_APPEND_DUPLICATE);
    puddle_store_close(s);
}

static puddle_store *g_shared;

static void *writer(void *arg)
{
    long who = (long)arg;
    char id[65], body[32];
    for (int i = 0; i < 50; i++) {
        snprintf(body, sizeof body, "t%ld-%d", who, i);
        if (append(g_shared, "race", ts_of(14, 1) + i, body, id) != PUDDLE_APPEND_NEW)
            failures++;
    }
    return NULL;
}

static void test_concurrent(void)
{
    char err[256];
    g_shared = puddle_store_open(dir_for("race"), err, sizeof err);
    pthread_t t[4];
    for (long i = 0; i < 4; i++)
        pthread_create(&t[i], NULL, writer, (void *)i);
    for (int i = 0; i < 4; i++)
        pthread_join(t[i], NULL);
    puddle_event_list l;
    CHECK(puddle_store_tail(g_shared, "race", 1000, &l, err, sizeof err) == 0 && l.count == 200);
    for (size_t i = 1; i < l.count; i++)
        CHECK(l.items[i - 1].ts < l.items[i].ts ||
              (l.items[i - 1].ts == l.items[i].ts && strcmp(l.items[i - 1].id, l.items[i].id) < 0));
    puddle_event_list_free(&l);
    puddle_store_close(g_shared);

    g_shared = puddle_store_open(dir_for("race"), err, sizeof err); /* and it all reloads from disk */
    CHECK(puddle_store_tail(g_shared, "race", 1000, &l, err, sizeof err) == 0 && l.count == 200);
    puddle_event_list_free(&l);
    puddle_store_close(g_shared);
}

int main(void)
{
    const char *base = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/puddle-store-XXXXXX", base ? base : "/tmp");
    if (!mkdtemp(root))
        return 2;

    test_basics_and_persistence();
    test_lock();
    test_torn_tail();
    test_corrupt_middle();
    test_chunks();
    test_validation();
    test_many_chats();
    test_concurrent();

    cleanup();
    return failures ? 1 : 0;
}
