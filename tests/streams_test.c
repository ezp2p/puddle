#include "core/streams.h"

#include "core/phrase.h"

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

#define T0 1760000000000LL

typedef struct {
    puddle_identity *id;
    char uid[PUDDLE_USER_ID_LEN + 1];
    char phrase[PUDDLE_PHRASE_LEN + 1];
} person;

static void make(person *p, const char *name)
{
    char dir[700], err[300];
    snprintf(dir, sizeof dir, "%s/%s", root, name);
    p->id = puddle_identity_create(dir, p->phrase, err, sizeof err);
    if (!p->id) { fprintf(stderr, "cannot make %s: %s\n", name, err); exit(2); }
    puddle_identity_confirm(dir);
    puddle_identity_id(p->id, p->uid);
}

static void give_chain(puddle_stream *st, person *p)
{
    size_t n;
    char *chain = puddle_identity_chain(p->id, &n);
    CHECK(puddle_stream_set_chain(st, chain, n) == 0);
    free(chain);
}

static void key_of(person *p, char kid[65], unsigned char pub[32])
{
    puddle_identity_public(p->id, pub, NULL, NULL);
    puddle_key_id(pub, kid);
}

static void roll(person *p)
{
    char fresh[PUDDLE_PHRASE_LEN + 1], err[300];
    puddle_roll *r = puddle_identity_roll_prepare(p->id, p->phrase, fresh, err, sizeof err);
    if (!r || puddle_identity_roll_commit(p->id, r, err, sizeof err) != 0) { fprintf(stderr, "roll: %s\n", err); exit(2); }
    memcpy(p->phrase, fresh, sizeof p->phrase);
}

static const char *dave_uid_placeholder(void)
{
    static person d;
    if (!d.id) make(&d, "dave");
    return d.uid;
}

static char dd[800];
static const char *data_dir(void) { snprintf(dd, sizeof dd, "%s/data", root); return dd; }

static puddle_streams *open_streams(void)
{
    char err[300];
    puddle_streams *s = puddle_streams_open(data_dir(), err, sizeof err);
    if (!s) fprintf(stderr, "open: %s\n", err);
    return s;
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    fputs(text, f);
    fclose(f);
}

/* ---- update ---- */
static int bump(void *u, puddle_stream *st, int exists)
{
    (void)u;
    st->addr_seq = exists ? st->addr_seq + 1 : 1000;
    return 0;
}
static int seen_exists;
static int decline(void *u, puddle_stream *st, int exists) { (void)u; seen_exists = exists; st->addr_seq = 99; return 7; }
static int fail_it(void *u, puddle_stream *st, int exists) { (void)u; (void)exists; st->addr_seq = 99; return -5; }
static void *counter(void *arg)
{
    puddle_streams *s = ((void **)arg)[0];
    const char *id = ((void **)arg)[1];
    for (int i = 0; i < 12; i++)
        puddle_streams_update(s, id, bump, NULL, NULL, 0);
    return NULL;
}

static puddle_streams *shared;
static person alice, bob, carol;
static void *hammer(void *arg)
{
    int n = *(int *)arg;
    for (int i = 0; i < 10; i++) { /* every write is flushed to disk, which is slow on purpose */
        puddle_stream st;
        char kid[65];
        unsigned char pub[32], got[32];
        key_of(&alice, kid, pub);
        if (n % 2) {
            puddle_streams_block(shared, bob.uid, T0 + i, NULL, 0);
            puddle_streams_unblock(shared, bob.uid, NULL, 0);
        } else {
            puddle_stream_init(&st, bob.uid, PUDDLE_STREAM_PENDING_IN, T0);
            puddle_streams_put(shared, &st, NULL, 0);
            if (puddle_streams_get(shared, bob.uid, &st) == 0)
                puddle_stream_free(&st);
        }
        puddle_streams_peer_key(shared, alice.uid, kid, got);
        puddle_streams_count(shared, PUDDLE_STREAM_ACTIVE);
        puddle_invite inv;
        if (puddle_streams_invite_create(shared, T0, 60000, 1, &inv, NULL, 0) == 0)
            puddle_streams_invite_consume(shared, inv.token, T0);
    }
    return NULL;
}

int main(void)
{
    const char *base = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/puddle-streams-XXXXXX", base ? base : "/tmp");
    if (!mkdtemp(root)) return 2;
    make(&alice, "alice");
    make(&bob, "bob");
    make(&carol, "carol");

    char err[300];
    puddle_stream st, got;
    char kid[65];
    unsigned char pub[32], found[32];

    puddle_streams *s = open_streams();
    CHECK(s != NULL);
    if (!s) return 2;

    /* nothing yet */
    CHECK(puddle_streams_get(s, alice.uid, &got) == 1);
    CHECK(puddle_streams_get(s, "../../etc/passwd", &got) == -1);
    CHECK(puddle_streams_get(s, "", &got) == -1);
    CHECK(puddle_streams_access(s) == PUDDLE_ACCESS_MANUAL);
    CHECK(puddle_streams_count(s, PUDDLE_STREAM_ACTIVE) == 0);
    CHECK(puddle_stream_init(&st, "not an id", PUDDLE_STREAM_ACTIVE, T0) == -1);

    /* a request we sent, not yet answered: we know their id and address, but not their keys */
    CHECK(puddle_stream_init(&st, alice.uid, PUDDLE_STREAM_PENDING_OUT, T0) == 0);
    CHECK(puddle_stream_add_address(&st, "203.0.113.7:8443") == 0);
    CHECK(puddle_stream_add_address(&st, "[2001:db8::1]:8443") == 0);
    CHECK(puddle_stream_add_address(&st, "203.0.113.7:8443") == 0 && st.naddr == 2); /* no duplicates */
    CHECK(puddle_stream_add_address(&st, "bad address") == -1 && st.naddr == 2);
    snprintf(st.note, sizeof st.note, "hey it's \"Jayden\" from work\nline two \xC3\xA9 \xF0\x9F\x99\x82 \\ end");
    st.addr_seq = 7;
    CHECK(puddle_streams_put(s, &st, err, sizeof err) == 0);
    CHECK(puddle_streams_get(s, alice.uid, &got) == 0);
    CHECK(got.state == PUDDLE_STREAM_PENDING_OUT && got.chain == NULL && got.naddr == 2 && got.addr_seq == 7);
    CHECK(!strcmp(got.addresses[0], "203.0.113.7:8443") && !strcmp(got.addresses[1], "[2001:db8::1]:8443"));
    CHECK(!strcmp(got.note, st.note) && got.created_ms == T0 && got.updated_ms == T0);
    puddle_stream_free(&got);
    key_of(&alice, kid, pub);
    CHECK(puddle_streams_peer_key(s, alice.uid, kid, found) == 0); /* no keys yet, so no signature can be checked */
    CHECK(puddle_streams_peer_key(s, alice.uid, "", found) == 0);  /* an empty key id is not "the key we have none of" */
    CHECK(puddle_streams_count(s, PUDDLE_STREAM_PENDING_OUT) == 1 && puddle_streams_count(s, PUDDLE_STREAM_ACTIVE) == 0);

    /* they answered: now we hold their chain */
    give_chain(&st, &alice);
    st.state = PUDDLE_STREAM_ACTIVE;
    st.updated_ms = T0 + 5;
    CHECK(puddle_streams_put(s, &st, err, sizeof err) == 0);
    CHECK(puddle_streams_peer_key(s, alice.uid, kid, found) == 1 && !memcmp(found, pub, 32));
    CHECK(puddle_streams_peer_key(s, bob.uid, kid, found) == 0);                    /* someone else's id, her key id */
    CHECK(puddle_streams_peer_key(s, alice.uid, "00", found) == 0);                  /* a key id that is not hers */
    CHECK(puddle_streams_peer_key(s, alice.uid, "", found) == 0);
    CHECK(puddle_streams_peer_key(s, alice.uid, NULL, found) == 0);
    CHECK(puddle_streams_peer_key(s, "../etc", kid, found) == 0);
    CHECK(puddle_streams_peer_key(s, NULL, kid, found) == 0);
    CHECK(puddle_streams_get(s, alice.uid, &got) == 0 && got.state == PUDDLE_STREAM_ACTIVE && got.chain_len == st.chain_len &&
          !strcmp(got.chain, st.chain) && got.updated_ms == T0 + 5);
    puddle_stream_free(&got);
    CHECK(puddle_streams_count(s, PUDDLE_STREAM_ACTIVE) == 1 && puddle_streams_count(s, PUDDLE_STREAM_PENDING_OUT) == 0);

    /* nothing unproven goes in */
    puddle_stream bad;
    CHECK(puddle_stream_init(&bad, bob.uid, PUDDLE_STREAM_ACTIVE, T0) == 0);
    give_chain(&bad, &alice); /* Alice's chain filed under Bob's id */
    CHECK(puddle_streams_put(s, &bad, err, sizeof err) == -1 && err[0]);
    CHECK(puddle_streams_get(s, bob.uid, &got) == 1);
    CHECK(puddle_stream_set_chain(&bad, st.chain, st.chain_len) == 0);
    bad.chain[40] = bad.chain[40] == 'a' ? 'b' : 'a'; /* tampered */
    memcpy(bad.id, alice.uid, sizeof bad.id);
    CHECK(puddle_streams_put(s, &bad, err, sizeof err) == -1);
    CHECK(puddle_streams_get(s, alice.uid, &got) == 0 && !strcmp(got.chain, st.chain)); /* the good record is untouched */
    puddle_stream_free(&got);
    CHECK(puddle_stream_set_chain(&bad, "{\"nonsense\":1}\n", 15) == 0 && puddle_streams_put(s, &bad, err, sizeof err) == -1);
    memcpy(bad.id, "../../../../x", 14);
    CHECK(puddle_streams_put(s, &bad, err, sizeof err) == -1);
    puddle_stream_free(&bad);
    {
        puddle_stream big;
        puddle_stream_init(&big, carol.uid, PUDDLE_STREAM_PENDING_IN, T0);
        char *huge = malloc(PUDDLE_CHAIN_MAX_BYTES + 2);
        memset(huge, 'x', PUDDLE_CHAIN_MAX_BYTES + 1);
        CHECK(puddle_stream_set_chain(&big, huge, PUDDLE_CHAIN_MAX_BYTES + 1) == -1 && big.chain == NULL);
        free(huge);
        for (int i = 0; i < PUDDLE_STREAM_MAX_ADDRS; i++) {
            char a[40];
            snprintf(a, sizeof a, "10.0.0.%d:1", i + 1);
            CHECK(puddle_stream_add_address(&big, a) == 0);
        }
        CHECK(puddle_stream_add_address(&big, "10.0.0.99:1") == -1);
        puddle_stream_free(&big);
    }

    /* it survives a restart, keys included */
    puddle_streams_close(s);
    s = open_streams();
    CHECK(s && puddle_streams_peer_key(s, alice.uid, kid, found) == 1 && !memcmp(found, pub, 32));
    CHECK(puddle_streams_count(s, PUDDLE_STREAM_ACTIVE) == 1);

    /* a roll: the new chain replaces the old, and the old key stops being accepted */
    char kid_old[65];
    memcpy(kid_old, kid, sizeof kid);
    roll(&alice);
    unsigned char pub2[32];
    char kid2[65];
    key_of(&alice, kid2, pub2);
    CHECK(strcmp(kid_old, kid2) != 0);
    CHECK(puddle_streams_get(s, alice.uid, &st) == 0);
    give_chain(&st, &alice);
    CHECK(puddle_streams_put(s, &st, err, sizeof err) == 0);
    CHECK(puddle_streams_peer_key(s, alice.uid, kid_old, found) == 0); /* retired keys can't start new requests */
    CHECK(puddle_streams_peer_key(s, alice.uid, kid2, found) == 1 && !memcmp(found, pub2, 32));
    puddle_stream_free(&st);

    /* a list */
    CHECK(puddle_stream_init(&st, bob.uid, PUDDLE_STREAM_PENDING_IN, T0 + 9) == 0);
    snprintf(st.note, sizeof st.note, "from bob");
    CHECK(puddle_streams_put(s, &st, err, sizeof err) == 0);
    puddle_stream_free(&st);
    puddle_stream *list;
    size_t count;
    CHECK(puddle_streams_list(s, &list, &count) == 0 && count == 2);
    int saw_alice = 0, saw_bob = 0;
    for (size_t i = 0; i < count; i++) {
        if (!strcmp(list[i].id, alice.uid)) saw_alice = list[i].state == PUDDLE_STREAM_ACTIVE && list[i].chain;
        if (!strcmp(list[i].id, bob.uid)) saw_bob = list[i].state == PUDDLE_STREAM_PENDING_IN && !strcmp(list[i].note, "from bob");
    }
    CHECK(saw_alice && saw_bob);
    puddle_stream_list_free(list, count);
    CHECK(puddle_streams_count(s, PUDDLE_STREAM_PENDING_IN) == 1);

    /* ---- the certificate we know for a peer ---- */
    {
        const char *fp1 = "1111111111111111111111111111111111111111111111111111111111111111";
        const char *fp2 = "2222222222222222222222222222222222222222222222222222222222222222";
        const char *fp3 = "3333333333333333333333333333333333333333333333333333333333333333";
        CHECK(puddle_streams_check_fingerprint(s, dave_uid_placeholder(), fp1) == 1); /* no record of them: nobody's to judge */
        CHECK(puddle_streams_check_fingerprint(s, alice.uid, "short") == 0);
        CHECK(puddle_streams_check_fingerprint(s, alice.uid, "zzzz111111111111111111111111111111111111111111111111111111111111") == 0);
        CHECK(puddle_streams_check_fingerprint(s, "junk", fp1) == 0);
        CHECK(puddle_streams_get(s, alice.uid, &got) == 0 && !got.tls_fp[0]);
        puddle_stream_free(&got);
        CHECK(puddle_streams_check_fingerprint(s, alice.uid, fp1) == 1);              /* the first one seen is the one we hold */
        CHECK(puddle_streams_get(s, alice.uid, &got) == 0 && !strcmp(got.tls_fp, fp1) && !got.tls_fp_seen[0]);
        puddle_stream_free(&got);
        CHECK(puddle_streams_check_fingerprint(s, alice.uid, fp1) == 1);
        CHECK(puddle_streams_check_fingerprint(s, alice.uid, fp2) == 0);              /* a different one: refused, and noted */
        CHECK(puddle_streams_get(s, alice.uid, &got) == 0 && !strcmp(got.tls_fp, fp1) && !strcmp(got.tls_fp_seen, fp2));
        puddle_stream_free(&got);
        CHECK(puddle_streams_check_fingerprint(s, alice.uid, fp2) == 0);
        CHECK(puddle_streams_check_fingerprint(s, alice.uid, fp3) == 0);
        CHECK(puddle_streams_get(s, alice.uid, &got) == 0 && !strcmp(got.tls_fp, fp1) && !strcmp(got.tls_fp_seen, fp3));
        puddle_stream_free(&got);
        puddle_streams_close(s);                                                       /* waiting for the owner survives a restart */
        s = open_streams();
        CHECK(puddle_streams_get(s, alice.uid, &got) == 0 && !strcmp(got.tls_fp, fp1) && !strcmp(got.tls_fp_seen, fp3));
        puddle_stream_free(&got);
        CHECK(puddle_streams_check_fingerprint(s, alice.uid, fp1) == 1);              /* the old one is back: nothing to decide */
        CHECK(puddle_streams_get(s, alice.uid, &got) == 0 && !got.tls_fp_seen[0]);
        puddle_stream_free(&got);
        CHECK(puddle_streams_trust_fingerprint(s, alice.uid, err, sizeof err) == 1);   /* nothing was waiting */
        CHECK(puddle_streams_check_fingerprint(s, alice.uid, fp2) == 0);
        CHECK(puddle_streams_trust_fingerprint(s, alice.uid, err, sizeof err) == 0);
        CHECK(puddle_streams_get(s, alice.uid, &got) == 0 && !strcmp(got.tls_fp, fp2) && !got.tls_fp_seen[0]);
        puddle_stream_free(&got);
        CHECK(puddle_streams_check_fingerprint(s, alice.uid, fp2) == 1 && puddle_streams_check_fingerprint(s, alice.uid, fp1) == 0);
        CHECK(puddle_streams_trust_fingerprint(s, "junk", err, sizeof err) == -1 && puddle_streams_trust_fingerprint(s, carol.uid, err, sizeof err) == -1);
    }

    /* ---- blocks ---- */
    CHECK(!puddle_streams_is_blocked(s, alice.uid) && !puddle_streams_is_blocked(s, "junk"));
    CHECK(puddle_streams_block(s, alice.uid, T0 + 20, err, sizeof err) == 0);
    CHECK(puddle_streams_is_blocked(s, alice.uid));
    CHECK(puddle_streams_peer_key(s, alice.uid, kid2, found) == 0);           /* their signature is no longer accepted */
    CHECK(puddle_streams_get(s, alice.uid, &got) == 0 && got.state == PUDDLE_STREAM_CLOSED && got.updated_ms == T0 + 20 && got.chain);
    puddle_stream_free(&got);
    CHECK(puddle_streams_count(s, PUDDLE_STREAM_ACTIVE) == 0);
    CHECK(puddle_stream_init(&st, alice.uid, PUDDLE_STREAM_PENDING_IN, T0 + 21) == 0);
    CHECK(puddle_streams_put(s, &st, err, sizeof err) == -1 && strstr(err, "blocked")); /* nothing new is recorded about them */
    puddle_stream_free(&st);
    CHECK(puddle_streams_block(s, alice.uid, T0 + 30, err, sizeof err) == 0); /* again: nothing changes */
    CHECK(puddle_streams_block(s, carol.uid, T0 + 31, err, sizeof err) == 0); /* someone with no stream at all */
    CHECK(puddle_streams_get(s, carol.uid, &got) == 1);
    CHECK(puddle_streams_block(s, "nope", T0, err, sizeof err) == -1);
    puddle_block ids[4];
    CHECK(puddle_streams_blocked(s, ids, 4) == 2 && !strcmp(ids[0].id, alice.uid) && !strcmp(ids[1].id, carol.uid));
    CHECK(ids[0].ts_ms == T0 + 20 && ids[1].ts_ms == T0 + 31); /* blocking again keeps the first time */
    CHECK(puddle_streams_blocked(s, ids, 1) == 2); /* reports the true count even when the room is short */
    puddle_streams_close(s);
    s = open_streams();
    CHECK(s && puddle_streams_is_blocked(s, alice.uid) && puddle_streams_is_blocked(s, carol.uid));
    CHECK(puddle_streams_peer_key(s, alice.uid, kid2, found) == 0);
    CHECK(puddle_streams_unblock(s, alice.uid, err, sizeof err) == 0);
    CHECK(puddle_streams_unblock(s, alice.uid, err, sizeof err) == 1);
    CHECK(puddle_streams_unblock(s, "junk", err, sizeof err) == -1);
    CHECK(!puddle_streams_is_blocked(s, alice.uid) && puddle_streams_is_blocked(s, carol.uid));
    CHECK(puddle_streams_peer_key(s, alice.uid, kid2, found) == 1); /* back to how it was, still closed until she asks again */
    CHECK(puddle_streams_get(s, alice.uid, &got) == 0 && got.state == PUDDLE_STREAM_CLOSED);
    puddle_stream_free(&got);

    /* ---- the access setting ---- */
    CHECK(puddle_streams_set_access(s, PUDDLE_ACCESS_OPEN, err, sizeof err) == 0 && puddle_streams_access(s) == PUDDLE_ACCESS_OPEN);
    puddle_streams_close(s);
    s = open_streams();
    CHECK(puddle_streams_access(s) == PUDDLE_ACCESS_OPEN);
    CHECK(puddle_streams_set_access(s, PUDDLE_ACCESS_CLOSED, err, sizeof err) == 0);
    puddle_streams_close(s);
    s = open_streams();
    CHECK(puddle_streams_access(s) == PUDDLE_ACCESS_CLOSED);
    {
        char path[900];
        snprintf(path, sizeof path, "%s/state/access-policy", data_dir());
        write_file(path, "banana\n");
        puddle_streams_close(s);
        s = open_streams();
        CHECK(puddle_streams_access(s) == PUDDLE_ACCESS_MANUAL); /* garbage means the default */
        puddle_access a;
        CHECK(puddle_access_parse("open", &a) == 0 && a == PUDDLE_ACCESS_OPEN && puddle_access_parse("Open", &a) == -1 && puddle_access_parse(NULL, &a) == -1);
        puddle_stream_state ss;
        CHECK(puddle_stream_state_parse("pending_in", &ss) == 0 && ss == PUDDLE_STREAM_PENDING_IN && puddle_stream_state_parse("x", &ss) == -1);
        CHECK(!strcmp(puddle_stream_state_name(PUDDLE_STREAM_CLOSED), "closed"));
    }

    /* ---- invites ---- */
    puddle_invite inv, inv2, many[PUDDLE_MAX_INVITES + 2];
    CHECK(puddle_streams_invite_create(s, T0, 60000, 1, &inv, err, sizeof err) == 0);
    CHECK(puddle_invite_token_valid(inv.token) && inv.uses == 1 && inv.expires_ms == T0 + 60000);
    CHECK(puddle_streams_invite_consume(s, inv.token, T0 + 10) == 1);
    CHECK(puddle_streams_invite_consume(s, inv.token, T0 + 10) == 0);              /* used up */
    CHECK(puddle_streams_invites(s, T0 + 10, many, PUDDLE_MAX_INVITES) == 0);
    CHECK(puddle_streams_invite_create(s, T0, 60000, 2, &inv, err, sizeof err) == 0);
    CHECK(puddle_streams_invite_create(s, T0, 60000, 1, &inv2, err, sizeof err) == 0 && strcmp(inv.token, inv2.token) != 0);
    puddle_streams_close(s);
    s = open_streams();                                                             /* uses left survive a restart */
    CHECK(puddle_streams_invites(s, T0, many, PUDDLE_MAX_INVITES) == 2);
    CHECK(puddle_streams_invite_consume(s, inv.token, T0 + 1) == 1);
    puddle_streams_close(s);
    s = open_streams();
    CHECK(puddle_streams_invite_consume(s, inv.token, T0 + 2) == 1);
    CHECK(puddle_streams_invite_consume(s, inv.token, T0 + 3) == 0);                /* two uses */
    CHECK(puddle_streams_invite_consume(s, inv2.token, T0 + 59999) == 1);           /* the last moment */
    CHECK(puddle_streams_invite_create(s, T0, 60000, 3, &inv, err, sizeof err) == 0);
    CHECK(puddle_streams_invite_consume(s, inv.token, T0 + 60000) == 0);            /* expired */
    CHECK(puddle_streams_invite_consume(s, "abcdefghijklmnopqrstuvwxy2", T0) == 0);
    CHECK(puddle_streams_invite_consume(s, "junk", T0) == 0 && puddle_streams_invite_consume(s, NULL, T0) == 0);
    CHECK(puddle_streams_invite_create(s, T0, 60000, 1, &inv, err, sizeof err) == 0);
    CHECK(puddle_streams_invite_revoke(s, inv.token) == 0 && puddle_streams_invite_revoke(s, inv.token) == 1);
    CHECK(puddle_streams_invite_consume(s, inv.token, T0) == 0);
    CHECK(puddle_streams_invite_create(s, T0, 0, 1, &inv, err, sizeof err) == -1);
    CHECK(puddle_streams_invite_create(s, T0, 60000, 0, &inv, err, sizeof err) == -1);
    CHECK(puddle_streams_invite_create(s, T0, 60000, 1001, &inv, err, sizeof err) == -1);
    CHECK(puddle_streams_invite_create(s, T0, 31LL * 24 * 3600 * 1000, 1, &inv, err, sizeof err) == -1);
    CHECK(puddle_streams_invite_create(s, T0, 30LL * 24 * 3600 * 1000, 1, &inv, err, sizeof err) == 0);
    for (size_t n = puddle_streams_invites(s, T0, many, PUDDLE_MAX_INVITES); n < PUDDLE_MAX_INVITES; n++)
        CHECK(puddle_streams_invite_create(s, T0, 60000, 1, &inv, err, sizeof err) == 0);
    CHECK(puddle_streams_invite_create(s, T0, 60000, 1, &inv, err, sizeof err) == -1 && strstr(err, "too many"));
    CHECK(puddle_streams_invite_create(s, T0 + 70000, 60000, 1, &inv, err, sizeof err) == 0); /* expired ones make room */
    CHECK(puddle_streams_invites(s, T0 + 70000, many, 0) >= 1);

    /* ---- damaged files cost one peer, not the Puddle ---- */
    puddle_streams_close(s);
    {
        char path[900], other[900];
        snprintf(path, sizeof path, "%s/state/streams/%s.json", data_dir(), carol.uid);
        write_file(path, "{this is not json");
        snprintf(path, sizeof path, "%s/state/streams/%s.json", data_dir(), bob.uid);
        size_t n = 0;
        FILE *f = fopen(path, "rb");
        static char text[200000];
        n = fread(text, 1, sizeof text - 1, f);
        fclose(f);
        text[n] = 0;
        snprintf(other, sizeof other, "%s/state/streams/%s.json", data_dir(), alice.uid);
        /* Alice's file with her id swapped for Bob's: the file name no longer matches what is inside */
        FILE *g = fopen(other, "rb");
        static char atext[300000];
        size_t an = fread(atext, 1, sizeof atext - 1, g);
        fclose(g);
        atext[an] = 0;
        char *p = strstr(atext, alice.uid);
        CHECK(p != NULL);
        if (p) memcpy(p, bob.uid, PUDDLE_USER_ID_LEN);
        snprintf(path, sizeof path, "%s/state/streams/%s.json", data_dir(), alice.uid);
        write_file(path, atext);
        snprintf(path, sizeof path, "%s/state/streams/notanid.json", data_dir());
        write_file(path, "{}");
        snprintf(path, sizeof path, "%s/state/streams/.tmp-123", data_dir());
        write_file(path, "garbage");
    }
    s = open_streams();
    CHECK(s != NULL);
    CHECK(puddle_streams_get(s, carol.uid, &got) == 1);   /* damaged: gone, not a crash */
    CHECK(puddle_streams_get(s, alice.uid, &got) == 1);   /* inside says someone else */
    CHECK(puddle_streams_get(s, bob.uid, &got) == 0);     /* the rest is fine */
    puddle_stream_free(&got);
    CHECK(puddle_streams_peer_key(s, alice.uid, kid2, found) == 0);

    /* a record whose chain was swapped for another person's chain is not trusted on load either */
    puddle_streams_close(s);
    {
        char path[900];
        snprintf(path, sizeof path, "%s/state/streams/%s.json", data_dir(), bob.uid);
        puddle_stream rec;
        CHECK(puddle_stream_init(&rec, bob.uid, PUDDLE_STREAM_ACTIVE, T0) == 0);
        give_chain(&rec, &bob);
        s = open_streams();
        CHECK(puddle_streams_put(s, &rec, err, sizeof err) == 0);
        puddle_streams_close(s);
        size_t n;
        FILE *f = fopen(path, "rb");
        static char text[300000];
        n = fread(text, 1, sizeof text - 1, f);
        fclose(f);
        text[n] = 0;
        puddle_identity_chain(carol.id, &n);
        size_t cl;
        char *cc = puddle_identity_chain(carol.id, &cl);
        /* replace the first record's id inside the chain text: the chain no longer proves Bob's id */
        char *q = strstr(text, "\"chain\"");
        CHECK(q != NULL);
        char *r = q ? strstr(q, "inception") : NULL;
        if (r) r[0] = 'I';
        write_file(path, text);
        free(cc);
        puddle_stream_free(&rec);
        s = open_streams();
        CHECK(puddle_streams_get(s, bob.uid, &got) == 1);
        char kidb[65];
        unsigned char pubb[32];
        key_of(&bob, kidb, pubb);
        CHECK(puddle_streams_peer_key(s, bob.uid, kidb, found) == 0);
    }

    /* ---- update: read, change and write as one step ---- */
    {
        char e2[200];
        puddle_streams *u = open_streams();
        const char *dv = dave_uid_placeholder();
        CHECK(puddle_streams_update(u, "junk", bump, NULL, e2, sizeof e2) == -1);
        CHECK(puddle_streams_update(u, dv, bump, NULL, e2, sizeof e2) == 0);      /* a peer we had no record of */
        CHECK(puddle_streams_get(u, dv, &got) == 0 && got.addr_seq == 1000 && got.state == PUDDLE_STREAM_PENDING_IN);
        puddle_stream_free(&got);
        CHECK(puddle_streams_update(u, dv, bump, NULL, e2, sizeof e2) == 0);
        CHECK(puddle_streams_get(u, dv, &got) == 0 && got.addr_seq == 1001);
        puddle_stream_free(&got);
        seen_exists = -1;
        CHECK(puddle_streams_update(u, dv, decline, NULL, e2, sizeof e2) == 7 && seen_exists == 1);   /* declined: its value comes back */
        CHECK(puddle_streams_update(u, dv, fail_it, NULL, e2, sizeof e2) == -1);
        CHECK(puddle_streams_get(u, dv, &got) == 0 && got.addr_seq == 1001);                        /* neither wrote anything */
        puddle_stream_free(&got);
        CHECK(puddle_streams_block(u, dv, T0, e2, sizeof e2) == 0);
        CHECK(puddle_streams_update(u, dv, bump, NULL, e2, sizeof e2) == -1 && strstr(e2, "blocked"));
        CHECK(puddle_streams_update(u, dv, decline, NULL, e2, sizeof e2) == 7);                    /* a no-change needs no write */
        CHECK(puddle_streams_unblock(u, dv, e2, sizeof e2) == 0);
        /* four threads add to one record: none is lost */
        void *args[4][2];
        pthread_t th[4];
        for (int i = 0; i < 4; i++) { args[i][0] = u; args[i][1] = (void *)dv; pthread_create(&th[i], NULL, counter, args[i]); }
        for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
        CHECK(puddle_streams_get(u, dv, &got) == 0 && got.addr_seq == 1001 + 48);
        puddle_stream_free(&got);
        puddle_streams_close(u);
    }

    /* ---- several threads at once ---- */
    puddle_streams_close(s);
    {
        char path[900];
        snprintf(path, sizeof path, "%s/state/streams/%s.json", data_dir(), bob.uid);
        remove(path);
        snprintf(path, sizeof path, "%s/state/blocks.tsv", data_dir());
        remove(path);
        shared = open_streams();
        puddle_stream rec;
        CHECK(puddle_stream_init(&rec, alice.uid, PUDDLE_STREAM_ACTIVE, T0) == 0);
        give_chain(&rec, &alice);
        CHECK(puddle_streams_put(shared, &rec, err, sizeof err) == 0);
        puddle_stream_free(&rec);
        pthread_t th[4];
        int ids[4] = {0, 1, 2, 3};
        for (int i = 0; i < 4; i++) pthread_create(&th[i], NULL, hammer, &ids[i]);
        for (int i = 0; i < 4; i++) pthread_join(th[i], NULL);
        key_of(&alice, kid, pub);
        CHECK(puddle_streams_peer_key(shared, alice.uid, kid, found) == 1); /* untouched by all that */
        puddle_streams_close(shared);
    }

    /* the same store on the memory driver: no folder, and what it keeps outlives the store but not the storage */
    {
        puddle_storage *mem = puddle_storage_open_memory();
        puddle_streams *m = puddle_streams_open_with(mem, err, sizeof err);
        CHECK(m != NULL);
        puddle_stream rec;
        CHECK(puddle_stream_init(&rec, alice.uid, PUDDLE_STREAM_PENDING_OUT, T0) == 0);
        CHECK(puddle_stream_add_address(&rec, "203.0.113.7:8443") == 0);
        CHECK(puddle_streams_put(m, &rec, err, sizeof err) == 0);
        puddle_stream_free(&rec);
        CHECK(puddle_streams_block(m, bob.uid, T0, err, sizeof err) == 0);
        puddle_streams_close(m);
        m = puddle_streams_open_with(mem, err, sizeof err);
        CHECK(m && puddle_streams_get(m, alice.uid, &got) == 0 && got.state == PUDDLE_STREAM_PENDING_OUT && got.naddr == 1);
        puddle_stream_free(&got);
        CHECK(puddle_streams_count(m, PUDDLE_STREAM_PENDING_OUT) == 1);
        CHECK(puddle_streams_is_blocked(m, bob.uid) == 1);
        puddle_streams_close(m);
        puddle_storage_close(mem);
    }

    puddle_identity_close(alice.id); puddle_identity_close(bob.id); puddle_identity_close(carol.id);
    nftw(root, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    return failures ? 1 : 0;
}
