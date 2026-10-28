#include "storage/files.h"
#include "base/hash.h"

#include <fcntl.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static char root[512];
static int rm_cb(const char *p, const struct stat *sb, int t, struct FTW *f) { (void)sb; (void)t; (void)f; remove(p); return 0; }

#define SIZE ((size_t)(2.5 * (1 << 20))) /* three chunks, the last half a chunk */

static unsigned char *bytes(unsigned seed)
{
    unsigned char *b = malloc(SIZE);
    for (size_t i = 0; i < SIZE; i++) b[i] = (unsigned char)((i * 31 + seed) ^ (i >> 9));
    return b;
}

static int count_whole(void *u, const char *id, puddle_file_state st)
{
    struct { int whole; char id[65]; } *seen = u;
    if (st == PUDDLE_FILE_WHOLE) { seen->whole++; snprintf(seen->id, sizeof seen->id, "%s", id); }
    return 0;
}

static int count_note(void *u, const char *line) { (void)line; (*(int *)u)++; return 0; }

/* upload `b` whole, chunk by chunk, out of order: its id */
static int upload(puddle_files *f, const unsigned char *b, char id[65])
{
    char tok[PUDDLE_UPLOAD_TOKEN_LEN + 1];
    if (puddle_files_upload_create(f, SIZE, tok) != 0) return -1;
    for (int k = 2; k >= 0; k--)
        if (puddle_files_upload_put(f, tok, (uint32_t)k, b + (size_t)k * PUDDLE_FILE_CHUNK, puddle_file_chunk_len(SIZE, (uint32_t)k)) != 0) return -1;
    return puddle_files_upload_finish(f, tok, id);
}

int main(void)
{
    const char *base = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/puddle-files-XXXXXX", base ? base : "/tmp");
    if (!mkdtemp(root)) return 2;
    char err[300], id[65], again[65], path[1100];
    unsigned char *b = bytes(7);

    CHECK(puddle_file_chunk_count(SIZE) == 3 && puddle_file_chunk_len(SIZE, 2) == SIZE - 2 * PUDDLE_FILE_CHUNK && puddle_file_chunk_len(SIZE, 3) == 0);
    CHECK(puddle_file_chunk_count(1) == 1 && puddle_file_chunk_count(PUDDLE_FILE_CHUNK) == 1 && puddle_file_chunk_count(PUDDLE_FILE_CHUNK + 1) == 2);

    char sender_dir[600], receiver_dir[600];
    snprintf(sender_dir, sizeof sender_dir, "%s/sender", root);
    snprintf(receiver_dir, sizeof receiver_dir, "%s/receiver", root);
    puddle_files *s = puddle_files_open(sender_dir, err, sizeof err), *r = puddle_files_open(receiver_dir, err, sizeof err);
    CHECK(s && r);
    if (!s || !r) return 1;

    /* ---- the owner uploads a file: its id comes from its chunks ---- */
    {
        char tok[PUDDLE_UPLOAD_TOKEN_LEN + 1];
        CHECK(puddle_files_upload_create(s, 0, tok) == -1 && puddle_files_upload_create(s, PUDDLE_FILE_HARD_MAX + 1, tok) == -1);
        CHECK(puddle_files_upload_create(s, SIZE, tok) == 0 && puddle_upload_token_valid(tok));
        CHECK(puddle_files_upload_put(s, tok, 3, b, 10) == -1);                                     /* no such chunk */
        CHECK(puddle_files_upload_put(s, tok, 0, b, 10) == -1);                                     /* not that chunk's length */
        CHECK(puddle_files_upload_put(s, "0123456789abcdef0123456789abcdef", 0, b, PUDDLE_FILE_CHUNK) == 1);
        CHECK(puddle_files_upload_put(s, tok, 0, b, PUDDLE_FILE_CHUNK) == 0);
        int64_t sz; uint32_t have, count;
        CHECK(puddle_files_upload_info(s, tok, &sz, &have, &count) == 0 && sz == (int64_t)SIZE && have == 1 && count == 3);
        CHECK(puddle_files_upload_finish(s, tok, id) == 2);                                        /* not all here */
        CHECK(puddle_files_upload_cancel(s, tok) == 0 && puddle_files_upload_info(s, tok, &sz, &have, &count) == 1);
    }
    CHECK(upload(s, b, id) == 0 && puddle_file_id_valid(id));
    {
        unsigned char hashes[3 * 32];
        for (uint32_t k = 0; k < 3; k++) puddle_sha256(b + (size_t)k * PUDDLE_FILE_CHUNK, puddle_file_chunk_len(SIZE, k), hashes + k * 32);
        char want[65];
        puddle_sha256_hex(hashes, sizeof hashes, want);
        CHECK(!strcmp(id, want));                                                                   /* the hash of its chunk hashes */
    }
    int64_t size, have;
    CHECK(puddle_files_state(s, id, &size, &have) == PUDDLE_FILE_WHOLE && size == (int64_t)SIZE && have == size);
    CHECK(puddle_files_path(s, id, path, sizeof path) == 0 && strstr(path, id));
    CHECK(upload(s, b, again) == 0 && !strcmp(again, id));                                          /* the same bytes: the same file, kept once */

    /* ---- notes: lines kept with a file, and thrown away with it ---- */
    {

        int n;
        CHECK(puddle_files_note_add(s, id, "served bob 5") == 0 && puddle_files_note_add(s, id, "served bob 5") == 0);   /* once */
        CHECK(puddle_files_note_add(s, id, "request bob c m 7") == 0 && puddle_files_note_add(s, id, "two\nlines") == -1);
        CHECK(puddle_files_note_add(s, "nope", "x") == -1);
        n = 0;
        puddle_files_note_each(s, id, "served ", count_note, &n);
        CHECK(n == 1);
        n = 0;
        puddle_files_note_each(s, id, "", count_note, &n);
        CHECK(n == 2);
        CHECK(puddle_files_note_remove(s, id, "served ") == 1 && puddle_files_note_remove(s, id, "served ") == 0);
        CHECK(puddle_files_note_remove(s, id, "request ") == 1);
    }

    /* ---- a file the owner picked on this computer: taken in whole, and the original never read again ---- */
    {
        char orig[600], tok[PUDDLE_UPLOAD_TOKEN_LEN + 1], picked[65], kept[1024];
        snprintf(orig, sizeof orig, "%s/picked.bin", root);
        FILE *o = fopen(orig, "wb");
        fwrite(b, 1, SIZE, o);
        fclose(o);
        int fd = open(orig, O_RDONLY), cloned = -1;
        CHECK(puddle_files_upload_adopt(s, fd, SIZE - 1, tok, NULL) == -2);                        /* over the owner's limit */
        CHECK(puddle_files_upload_adopt(s, fd, 0, tok, &cloned) == 0 && (cloned == 0 || cloned == 1));
#ifdef __APPLE__
        CHECK(cloned == 1); /* APFS: a clone, taking no space */
#endif
        close(fd);
        CHECK(puddle_files_upload_finish(s, tok, picked) == 0 && !strcmp(picked, id));            /* the same bytes, the same id */
        o = fopen(orig, "r+b");                                                                     /* edited afterwards */
        fputs("changed", o);
        fclose(o);
        remove(orig);                                                                               /* then thrown away */
        CHECK(puddle_files_path(s, id, kept, sizeof kept) == 0);
        unsigned char *c0 = NULL;
        size_t l0 = 0;
        CHECK(puddle_files_read_chunk(s, id, 0, &c0, &l0) == 0 && l0 == PUDDLE_FILE_CHUNK && !memcmp(c0, b, l0)); /* unchanged */
        free(c0);
        /* a fresh file of its own (no copy of it here yet), from a clone */
        unsigned char *b2 = bytes(77);
        snprintf(orig, sizeof orig, "%s/picked2.bin", root);
        o = fopen(orig, "wb");
        fwrite(b2, 1, SIZE, o);
        fclose(o);
        fd = open(orig, O_RDONLY);
        char id2[65];
        CHECK(puddle_files_upload_adopt(s, fd, 0, tok, NULL) == 0 && puddle_files_upload_finish(s, tok, id2) == 0 && strcmp(id2, id) != 0);
        close(fd);
        CHECK(puddle_files_state(s, id2, NULL, NULL) == PUDDLE_FILE_WHOLE);
        struct stat st;
        CHECK(puddle_files_path(s, id2, kept, sizeof kept) == 0 && stat(kept, &st) == 0 && (st.st_mode & 077) == 0); /* the owner's only */
        CHECK(puddle_files_remove(s, id2) == 0);
        remove(orig);
        free(b2);
        fd = open(root, O_RDONLY);                                                                  /* not a regular file */
        CHECK(puddle_files_upload_adopt(s, fd, 0, tok, NULL) == -1);
        close(fd);
    }

    /* ---- another Puddle fetches it a chunk at a time ---- */
    unsigned char *list;
    uint32_t count;
    CHECK(puddle_files_chunk_list(s, id, &size, &list, &count) == 0 && count == 3 && size == (int64_t)SIZE);
    CHECK(puddle_files_state(r, id, NULL, NULL) == PUDDLE_FILE_ABSENT && puddle_files_fetch_next(r, id) == -1);
    {
        unsigned char wrong[3 * 32];
        memcpy(wrong, list, sizeof wrong);
        wrong[40] ^= 1;
        CHECK(puddle_files_fetch_begin(r, id, SIZE, wrong, 3) == -1);                               /* a list that is not this file's */
        CHECK(puddle_files_fetch_begin(r, id, SIZE + PUDDLE_FILE_CHUNK, list, 3) == -1);            /* a size that does not fit it */
    }
    CHECK(puddle_files_fetch_begin(r, id, SIZE, list, 3) == 0);
    CHECK(puddle_files_fetch_begin(r, id, SIZE, list, 3) == 0);                                     /* again: nothing changes */
    CHECK(puddle_files_state(r, id, &size, &have) == PUDDLE_FILE_PARTIAL && size == (int64_t)SIZE && have == 0);
    CHECK(puddle_files_fetch_next(r, id) == 0);
    unsigned char *c; size_t clen;
    CHECK(puddle_files_read_chunk(s, id, 1, &c, &clen) == 0 && clen == PUDDLE_FILE_CHUNK);
    c[5] ^= 0xFF;
    CHECK(puddle_files_fetch_put(r, id, 1, c, clen) == -1);                                         /* not what the list says: thrown away */
    c[5] ^= 0xFF;
    CHECK(puddle_files_fetch_put(r, id, 1, c, clen - 1) == -1);
    CHECK(puddle_files_fetch_put(r, id, 1, c, clen) == 0);
    free(c);
    CHECK(puddle_files_read_chunk(r, id, 1, &c, &clen) == 0 && memcmp(c, b + PUDDLE_FILE_CHUNK, clen) == 0);   /* it can pass it on already */
    free(c);
    CHECK(puddle_files_read_chunk(r, id, 0, &c, &clen) == 1);                                       /* ...but not one it does not have */
    CHECK(puddle_files_state(r, id, &size, &have) == PUDDLE_FILE_PARTIAL && have == PUDDLE_FILE_CHUNK);
    CHECK(puddle_files_fetch_next(r, id) == 0 && puddle_files_fetch_finish(r, id) == 1);

    /* stopped here: a restart picks up where it was */
    puddle_files_close(r);
    r = puddle_files_open(receiver_dir, err, sizeof err);
    for (int64_t n; (n = puddle_files_fetch_next(r, id)) >= 0;) {
        CHECK(n == 0 || n == 2);
        CHECK(puddle_files_read_chunk(s, id, (uint32_t)n, &c, &clen) == 0 && puddle_files_fetch_put(r, id, (uint32_t)n, c, clen) == 0);
        free(c);
    }
    CHECK(puddle_files_fetch_finish(r, id) == 0);
    CHECK(puddle_files_state(r, id, &size, &have) == PUDDLE_FILE_WHOLE && have == (int64_t)SIZE);
    CHECK(puddle_files_path(r, id, path, sizeof path) == 0);
    {
        FILE *fp = fopen(path, "rb");
        unsigned char *got = malloc(SIZE + 1);
        size_t n = fp ? fread(got, 1, SIZE + 1, fp) : 0;
        if (fp) fclose(fp);
        CHECK(n == SIZE && memcmp(got, b, SIZE) == 0);                                              /* exactly what was sent */
        free(got);
    }
    unsigned char *list2;
    uint32_t count2;
    CHECK(puddle_files_chunk_list(r, id, &size, &list2, &count2) == 0 && count2 == 3 && memcmp(list, list2, 96) == 0);
    free(list2);
    free(list);

    /* ---- what is here, and letting go ---- */
    {
        struct seen_t { int whole; char id[65]; } seen = { 0, "" };
        puddle_files_each(r, count_whole, &seen);
        CHECK(seen.whole == 1 && !strcmp(seen.id, id));
    }
    CHECK(puddle_files_remove(r, id) == 0 && puddle_files_state(r, id, NULL, NULL) == PUDDLE_FILE_ABSENT);
    CHECK(puddle_files_remove(r, id) == 0);
    CHECK(puddle_files_state(r, "nothex", NULL, NULL) == PUDDLE_FILE_ABSENT && puddle_files_path(r, "../etc", path, sizeof path) == 1);

    /* ---- a file stays while a message carries it ---- */
    {
        char kept[65];
        CHECK(upload(s, b, kept) == 0 && !strcmp(kept, id));                                       /* the sender's copy is still here */
        const char *chat = "dddddddddddddddddddddddddddddddddddddddddddddddddddd";
        CHECK(puddle_files_ref(s, id, chat, "m1", 1000) == 0 && puddle_files_ref(s, id, chat, "m2", 1000) == 0);
        CHECK(puddle_files_ref(s, id, chat, "m1", 1000) == 0 && puddle_files_ref_count(s, id) == 2);   /* again: still two */
        CHECK(puddle_files_ref(s, id, "bad chat", "m1", 1000) == -1);
        int64_t nfiles, used;
        puddle_files_usage(s, &nfiles, &used);
        CHECK(nfiles == 1 && used == (int64_t)SIZE);
        CHECK(puddle_files_sweep(s, 1000LL * 24 * 3600 * 1000, 1000, NULL, NULL) == 0);            /* carried: never swept */
        CHECK(puddle_files_unref(s, id, chat, "m1") == 1 && puddle_files_unref(s, id, chat, "m1") == 1);
        CHECK(puddle_files_unref(s, id, chat, "m2") == 0 && puddle_files_ref_count(s, id) == 0);   /* the last: the caller lets it go */
        /* one uploaded and never sent is cleared away after its day */
        char lone[65];
        unsigned char *b2 = bytes(9);
        CHECK(upload(s, b2, lone) == 0);
        free(b2);
        CHECK(puddle_files_sweep(s, 5000, 1000, NULL, NULL) == 0);                                  /* first seen now: its day starts */
        CHECK(puddle_files_sweep(s, 5500, 1000, NULL, NULL) == 0 && puddle_files_state(s, lone, NULL, NULL) == PUDDLE_FILE_WHOLE);
        CHECK(puddle_files_ref(s, lone, chat, "m3", 5600) == 0);                                   /* sent after all: it stays */
        CHECK(puddle_files_sweep(s, 9000, 1000, NULL, NULL) == 1);                                 /* `id` went (nothing carries it); `lone` did not */
        CHECK(puddle_files_state(s, lone, NULL, NULL) == PUDDLE_FILE_WHOLE && puddle_files_state(s, id, NULL, NULL) == PUDDLE_FILE_ABSENT);
    }

    /* ---- an upload the owner never finished goes after a while ---- */
    {
        char tok[PUDDLE_UPLOAD_TOKEN_LEN + 1];
        int64_t sz; uint32_t hv, ct;
        CHECK(puddle_files_upload_create(s, 100, tok) == 0);
        CHECK(puddle_files_upload_expire(s, 1000, 500) == 0);                                       /* its time starts now */
        CHECK(puddle_files_upload_expire(s, 1400, 500) == 0 && puddle_files_upload_info(s, tok, &sz, &hv, &ct) == 0);
        CHECK(puddle_files_upload_expire(s, 1600, 500) == 1 && puddle_files_upload_info(s, tok, &sz, &hv, &ct) == 1);
    }

    puddle_files_close(s);
    puddle_files_close(r);
    free(b);
    nftw(root, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    return failures ? 1 : 0;
}
