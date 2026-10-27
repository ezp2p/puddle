#include "core/transfers.h"
#include "storage/files.h"

#include <ftw.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static char root[512];
static int rm_cb(const char *p, const struct stat *sb, int t, struct FTW *f) { (void)sb; (void)t; (void)f; remove(p); return 0; }

#define SIZE ((size_t)(2.5 * (1 << 20)))
#define BOB   "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define CAROL "cccccccccccccccccccccccccccccccccccccccccccccccccccc"
#define CHAT  "dddddddddddddddddddddddddddddddddddddddddddddddddddd"
#define MSG   "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

/* ---- the other Puddles: who is reachable and who has what ---- */
static puddle_files *theirs;      /* what bob and carol hold (one store will do) */
static int bob_up, carol_up, carol_has, bad_bytes;
static int asked_bob, asked_carol;
static int64_t clock_now = 1000000;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

static int64_t now_cb(void) { pthread_mutex_lock(&mu); int64_t t = clock_now; pthread_mutex_unlock(&mu); return t; }

static void get_cb(void *user, const char *peer, const char *path, size_t max, puddle_net_reply *r)
{
    (void)user; (void)max;
    memset(r, 0, sizeof *r);
    pthread_mutex_lock(&mu);
    int is_bob = !strcmp(peer, BOB), up = is_bob ? bob_up : carol_up, has = is_bob || carol_has, bad = bad_bytes;
    if (is_bob) asked_bob++; else asked_carol++;
    pthread_mutex_unlock(&mu);
    if (!up) {
        r->result = PUDDLE_NET_OFFLINE;
        snprintf(r->reason, sizeof r->reason, "nobody there");
        return;
    }
    r->result = PUDDLE_NET_OK;
    r->status = 404;
    char id[65];
    snprintf(id, sizeof id, "%.64s", path + strlen("/stream/v1/files/"));
    const char *rest = path + strlen("/stream/v1/files/") + 64;
    if (!has || !strstr(path, "?chat=" CHAT "&message=" MSG))
        return;
    if (!strncmp(rest, "/chunks?", 8)) {
        int64_t size; unsigned char *h; uint32_t n;
        if (puddle_files_chunk_list(theirs, id, &size, &h, &n) == 0) { r->status = 200; r->body = (char *)h; r->body_len = (size_t)n * 32; }
    } else {
        unsigned char *d; size_t len;
        if (puddle_files_read_chunk(theirs, id, (uint32_t)atoi(rest + 8), &d, &len) == 0) {
            if (bad) d[0] ^= 1;
            r->status = 200; r->body = (char *)d; r->body_len = len;
        }
    }
}

static int wait_state(puddle_transfers *t, const char *id, puddle_tr_state want, puddle_files *mine, int whole)
{
    for (int i = 0; i < 400; i++) {
        if (whole ? puddle_files_state(mine, id, NULL, NULL) == PUDDLE_FILE_WHOLE && puddle_transfers_state(t, id, NULL) == PUDDLE_TR_NONE
                  : puddle_transfers_state(t, id, NULL) == want)
            return 1;
        usleep(20000);
    }
    return 0;
}

static void upload(puddle_files *f, unsigned seed, char id[65])
{
    unsigned char *b = malloc(SIZE);
    for (size_t i = 0; i < SIZE; i++) b[i] = (unsigned char)(i * seed + (i >> 8));
    char tok[PUDDLE_UPLOAD_TOKEN_LEN + 1];
    puddle_files_upload_create(f, SIZE, tok);
    for (uint32_t k = 0; k < 3; k++) puddle_files_upload_put(f, tok, k, b + (size_t)k * PUDDLE_FILE_CHUNK, puddle_file_chunk_len(SIZE, k));
    puddle_files_upload_finish(f, tok, id);
    free(b);
}

int main(void)
{
    const char *base = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/puddle-transfers-XXXXXX", base ? base : "/tmp");
    if (!mkdtemp(root)) return 2;
    char err[300], dir[600], id1[65], id2[65], id3[65], why[200];
    snprintf(dir, sizeof dir, "%s/theirs", root);
    theirs = puddle_files_open(dir, err, sizeof err);
    upload(theirs, 3, id1);
    upload(theirs, 5, id2);
    upload(theirs, 7, id3);
    snprintf(dir, sizeof dir, "%s/mine", root);
    puddle_files *mine = puddle_files_open(dir, err, sizeof err);
    puddle_transfer_hooks hooks = { get_cb, NULL, now_cb };
    puddle_transfers *t = puddle_transfers_open(dir, mine, &hooks, err, sizeof err);
    CHECK(theirs && mine && t);
    const char *both[2] = { BOB, CAROL }, *carol_first[2] = { CAROL, BOB };

    /* ---- fetched whole from the first who can give it ---- */
    bob_up = carol_up = 1;
    CHECK(puddle_transfers_start(t, id1, SIZE, CHAT, MSG, both, 2) == 0);
    CHECK(wait_state(t, id1, PUDDLE_TR_NONE, mine, 1));
    CHECK(asked_bob == 4 && asked_carol == 0);                                                     /* the list, then three chunks */
    CHECK(puddle_transfers_start(t, id1, SIZE, CHAT, MSG, both, 2) == 0 && puddle_transfers_state(t, id1, NULL) == PUDDLE_TR_NONE);   /* here already */

    /* ---- carol is asked first but does not have it: bob does ---- */
    asked_bob = asked_carol = 0;
    CHECK(puddle_transfers_start(t, id2, SIZE, CHAT, MSG, carol_first, 2) == 0);
    CHECK(wait_state(t, id2, PUDDLE_TR_NONE, mine, 1));
    CHECK(asked_carol >= 1 && asked_bob == 4);

    /* ---- nobody reachable: it waits, says why, and tries again later ---- */
    pthread_mutex_lock(&mu); bob_up = carol_up = 0; pthread_mutex_unlock(&mu);
    CHECK(puddle_transfers_start(t, id3, SIZE, CHAT, MSG, both, 2) == 0);
    for (int i = 0; i < 200 && (puddle_transfers_state(t, id3, why) != PUDDLE_TR_QUEUED || !why[0]); i++) usleep(20000);
    CHECK(puddle_transfers_state(t, id3, why) == PUDDLE_TR_QUEUED && !strcmp(why, "nobody there"));
    CHECK(puddle_transfer_retry_delay_ms(1) == 15000 && puddle_transfer_retry_delay_ms(3) == 60000 && puddle_transfer_retry_delay_ms(30) == 3600000);
    /* a piece that is not this file's is thrown away, and it carries on from where it was after a restart */
    pthread_mutex_lock(&mu); bob_up = 1; bad_bytes = 1; pthread_mutex_unlock(&mu);
    puddle_transfers_retry_now(t);
    for (int i = 0; i < 200 && (puddle_transfers_state(t, id3, why) == PUDDLE_TR_ACTIVE || strcmp(why, "a piece they sent was not this file's") != 0); i++) usleep(20000);
    CHECK(!strcmp(why, "a piece they sent was not this file's") && puddle_files_state(mine, id3, NULL, NULL) == PUDDLE_FILE_PARTIAL);
    puddle_transfers_close(t);
    t = puddle_transfers_open(dir, mine, &hooks, err, sizeof err);
    CHECK(puddle_transfers_state(t, id3, NULL) == PUDDLE_TR_QUEUED);                               /* kept */
    pthread_mutex_lock(&mu); bad_bytes = 0; pthread_mutex_unlock(&mu);
    puddle_transfers_retry_now(t);
    CHECK(wait_state(t, id3, PUDDLE_TR_NONE, mine, 1));

    /* ---- nobody has it for a week: given up; asked again, it starts over ---- */
    puddle_files_remove(mine, id3);
    pthread_mutex_lock(&mu); bob_up = carol_up = 0; pthread_mutex_unlock(&mu);
    CHECK(puddle_transfers_start(t, id3, SIZE, CHAT, MSG, both, 2) == 0);
    for (int i = 0; i < 200 && puddle_transfers_state(t, id3, why) != PUDDLE_TR_QUEUED; i++) usleep(20000);
    usleep(100000);
    pthread_mutex_lock(&mu); clock_now += 8LL * 24 * 3600 * 1000; pthread_mutex_unlock(&mu);
    puddle_transfers_retry_now(t);
    CHECK(wait_state(t, id3, PUDDLE_TR_FAILED, mine, 0));
    pthread_mutex_lock(&mu); bob_up = 1; pthread_mutex_unlock(&mu);
    CHECK(puddle_transfers_start(t, id3, SIZE, CHAT, MSG, both, 2) == 0);
    CHECK(wait_state(t, id3, PUDDLE_TR_NONE, mine, 1));

    /* ---- cancelled: what came is thrown away ---- */
    puddle_files_remove(mine, id2);
    pthread_mutex_lock(&mu); bob_up = carol_up = 0; pthread_mutex_unlock(&mu);
    CHECK(puddle_transfers_start(t, id2, SIZE, CHAT, MSG, both, 2) == 0);
    for (int i = 0; i < 200 && puddle_transfers_state(t, id2, NULL) == PUDDLE_TR_ACTIVE; i++) usleep(20000);
    CHECK(puddle_transfers_cancel(t, id2) == 0 && puddle_transfers_state(t, id2, NULL) == PUDDLE_TR_NONE);
    CHECK(puddle_files_state(mine, id2, NULL, NULL) == PUDDLE_FILE_ABSENT);
    puddle_transfers_close(t);
    t = puddle_transfers_open(dir, mine, &hooks, err, sizeof err);
    CHECK(puddle_transfers_state(t, id2, NULL) == PUDDLE_TR_NONE);                                 /* and forgotten */
    CHECK(puddle_transfers_start(t, "nope", SIZE, CHAT, MSG, both, 2) == -1);

    puddle_transfers_close(t);
    puddle_files_close(mine);
    puddle_files_close(theirs);
    nftw(root, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    return failures ? 1 : 0;
}
