#include "http/reqsig.h"

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

static puddle_identity *make(const char *name, char *phrase_out)
{
    char dir[700], err[300];
    snprintf(dir, sizeof dir, "%s/%s", root, name);
    puddle_identity *id = puddle_identity_create(dir, phrase_out, err, sizeof err);
    if (!id) fprintf(stderr, "cannot make %s: %s\n", name, err);
    else puddle_identity_confirm(dir);
    return id;
}

/* What Bob's Puddle has stored about one friend, learned from their chain when the stream was made. */
typedef struct {
    char id[53];
    char kid[65];
    unsigned char pub[32];
} friend_rec;

static void learn(friend_rec *f, puddle_identity *who)
{
    size_t len;
    char *chain = puddle_identity_chain(who, &len);
    puddle_chain_head h;
    char err[200];
    if (puddle_chain_verify(chain, len, NULL, &h, err, sizeof err) != 0) { fprintf(stderr, "chain: %s\n", err); exit(2); }
    free(chain);
    memcpy(f->id, h.id, sizeof f->id);
    memcpy(f->pub, h.sign_pub, 32);
    puddle_key_id(h.sign_pub, f->kid);
}

/* Bob's lookup: only the friend he knows, and only their current key */
static int lookup(void *user, const char *sender_id, const char *kid, unsigned char pub[32])
{
    const friend_rec *f = user;
    if (strcmp(sender_id, f->id) != 0 || strcmp(kid, f->kid) != 0)
        return 0;
    memcpy(pub, f->pub, 32);
    return 1;
}

#define T0 1760000000000LL

static puddle_authn check(const char *hdr, const char *method, const char *target, const char *body,
                          const char *me, const friend_rec *f, puddle_replay *r, int64_t now)
{
    return puddle_reqsig_authenticate(hdr, method, target, body, body ? strlen(body) : 0, me, lookup,
                                      (void *)f, r, now, NULL);
}

/* the header with its signature's last base64 character changed (still well-formed) */
static void corrupt_sig(char *hdr)
{
    size_t n = strlen(hdr);
    hdr[n - 3] = (hdr[n - 3] == 'A') ? 'B' : 'A'; /* the char before the padding "=" */
}

int main(void)
{
    const char *base = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/puddle-reqsig-XXXXXX", base ? base : "/tmp");
    if (!mkdtemp(root))
        return 2;

    char alice_phrase[PUDDLE_PHRASE_LEN + 1], other_phrase[PUDDLE_PHRASE_LEN + 1];
    puddle_identity *alice = make("alice", alice_phrase), *bob = make("bob", other_phrase),
                    *carol = make("carol", other_phrase), *mallory = make("mallory", other_phrase);
    char aid[53], bid[53], cid[53];
    puddle_identity_id(alice, aid);
    puddle_identity_id(bob, bid);
    puddle_identity_id(carol, cid);
    friend_rec alice_at_bob;
    learn(&alice_at_bob, alice); /* Bob pinned Alice's chain when he accepted her stream */

    char hdr[500], hdr2[500];
    puddle_replay *replay = puddle_replay_new(0);
    puddle_reqsig parsed;

    /* ---- Alice proves she is Alice ---- */
    CHECK(puddle_reqsig_sign(alice, "POST", "/stream/v1/events", "{\"chat\":\"c\"}", 12, bid, T0, hdr, sizeof hdr) == 0);
    CHECK(puddle_reqsig_parse(hdr, &parsed) == 0 && strcmp(parsed.id, aid) == 0 && parsed.ts == T0);
    CHECK(strcmp(parsed.kid, alice_at_bob.kid) == 0);
    CHECK(puddle_reqsig_authenticate(hdr, "POST", "/stream/v1/events", "{\"chat\":\"c\"}", 12, bid, lookup,
                                     &alice_at_bob, replay, T0 + 1000, &parsed) == PUDDLE_AUTHN_OK);
    CHECK(strcmp(parsed.id, aid) == 0);

    /* a captured request can't be replayed, now or a minute later */
    CHECK(puddle_reqsig_authenticate(hdr, "POST", "/stream/v1/events", "{\"chat\":\"c\"}", 12, bid, lookup,
                                     &alice_at_bob, replay, T0 + 1000, NULL) == PUDDLE_AUTHN_REPLAYED);
    CHECK(puddle_reqsig_authenticate(hdr, "POST", "/stream/v1/events", "{\"chat\":\"c\"}", 12, bid, lookup,
                                     &alice_at_bob, replay, T0 + 60000, NULL) == PUDDLE_AUTHN_REPLAYED);

    /* every request has a fresh nonce */
    puddle_reqsig_sign(alice, "POST", "/stream/v1/events", "x", 1, bid, T0, hdr2, sizeof hdr2);
    puddle_reqsig p2;
    puddle_reqsig_parse(hdr2, &p2);
    CHECK(strcmp(p2.nonce, parsed.nonce) != 0);

    /* ---- Mallory tries to be Alice ---- */
    puddle_replay *r2 = puddle_replay_new(0);
    char mh[500];
    puddle_reqsig_sign(mallory, "POST", "/stream/v1/events", "evil", 4, bid, T0, mh, sizeof mh);
    CHECK(check(mh, "POST", "/stream/v1/events", "evil", bid, &alice_at_bob, r2, T0) == PUDDLE_AUTHN_REJECT); /* his own id: a stranger */

    /* he claims Alice's id but signs with his own key (and names his own key id) */
    puddle_reqsig mp;
    puddle_reqsig_parse(mh, &mp);
    char claim[500];
    snprintf(claim, sizeof claim, "v1 id=%s; kid=%s; ts=%lld; nonce=%s; sig=", aid, mp.kid, (long long)T0, mp.nonce);
    strcpy(claim + strlen(claim), strstr(mh, "sig=") + 4);
    CHECK(check(claim, "POST", "/stream/v1/events", "evil", bid, &alice_at_bob, r2, T0) == PUDDLE_AUTHN_REJECT);

    /* he names Alice's real key id too, but can't produce her signature */
    snprintf(claim, sizeof claim, "v1 id=%s; kid=%s; ts=%lld; nonce=%s; sig=", aid, alice_at_bob.kid, (long long)T0, mp.nonce);
    strcpy(claim + strlen(claim), strstr(mh, "sig=") + 4);
    CHECK(check(claim, "POST", "/stream/v1/events", "evil", bid, &alice_at_bob, r2, T0) == PUDDLE_AUTHN_REJECT);

    /* he takes one of Alice's real requests and changes it */
    puddle_reqsig_sign(alice, "POST", "/stream/v1/events", "hello", 5, bid, T0, hdr, sizeof hdr);
    CHECK(check(hdr, "POST", "/stream/v1/events", "hello", bid, &alice_at_bob, r2, T0) == PUDDLE_AUTHN_OK); /* the original is fine */
    puddle_reqsig_sign(alice, "POST", "/stream/v1/events", "hello", 5, bid, T0, hdr, sizeof hdr);
    CHECK(check(hdr, "POST", "/stream/v1/events", "hellp", bid, &alice_at_bob, r2, T0) == PUDDLE_AUTHN_REJECT);       /* body */
    CHECK(check(hdr, "GET", "/stream/v1/events", "hello", bid, &alice_at_bob, r2, T0) == PUDDLE_AUTHN_REJECT);        /* method */
    CHECK(check(hdr, "POST", "/stream/v1/close", "hello", bid, &alice_at_bob, r2, T0) == PUDDLE_AUTHN_REJECT);        /* path */
    CHECK(check(hdr, "POST", "/stream/v1/events?x=1", "hello", bid, &alice_at_bob, r2, T0) == PUDDLE_AUTHN_REJECT);   /* query */
    CHECK(check(hdr, "POST", "/stream/v1/events", "", bid, &alice_at_bob, r2, T0) == PUDDLE_AUTHN_REJECT);            /* no body */
    CHECK(check(hdr, "POST", "/stream/v1/events", NULL, bid, &alice_at_bob, r2, T0) == PUDDLE_AUTHN_REJECT);
    strcpy(hdr2, hdr);
    corrupt_sig(hdr2);
    CHECK(check(hdr2, "POST", "/stream/v1/events", "hello", bid, &alice_at_bob, r2, T0) == PUDDLE_AUTHN_REJECT);     /* signature */

    /* a request meant for Bob can't be used on Carol */
    puddle_reqsig_sign(alice, "POST", "/stream/v1/events", "hi", 2, bid, T0, hdr, sizeof hdr);
    CHECK(check(hdr, "POST", "/stream/v1/events", "hi", cid, &alice_at_bob, r2, T0) == PUDDLE_AUTHN_REJECT);
    CHECK(check(hdr, "POST", "/stream/v1/events", "hi", bid, &alice_at_bob, r2, T0) == PUDDLE_AUTHN_OK);

    /* forgeries and failed attempts don't use up a real nonce */
    puddle_replay *r3 = puddle_replay_new(0);
    puddle_reqsig_sign(alice, "GET", "/x", NULL, 0, bid, T0, hdr, sizeof hdr);
    strcpy(hdr2, hdr);
    corrupt_sig(hdr2);
    CHECK(check(hdr2, "GET", "/x", NULL, bid, &alice_at_bob, r3, T0) == PUDDLE_AUTHN_REJECT);
    CHECK(check(hdr, "GET", "/x", NULL, bid, &alice_at_bob, r3, T0) == PUDDLE_AUTHN_OK);
    puddle_replay_free(r3);

    /* ---- the clock ---- */
    puddle_replay *r4 = puddle_replay_new(0);
    puddle_reqsig_sign(alice, "GET", "/a", NULL, 0, bid, T0, hdr, sizeof hdr);
    CHECK(check(hdr, "GET", "/a", NULL, bid, &alice_at_bob, r4, T0 + PUDDLE_SIG_WINDOW_MS) == PUDDLE_AUTHN_OK);       /* exactly five minutes */
    puddle_reqsig_sign(alice, "GET", "/b", NULL, 0, bid, T0, hdr, sizeof hdr);
    CHECK(check(hdr, "GET", "/b", NULL, bid, &alice_at_bob, r4, T0 + PUDDLE_SIG_WINDOW_MS + 1) == PUDDLE_AUTHN_SKEW); /* just over */
    puddle_reqsig_sign(alice, "GET", "/c", NULL, 0, bid, T0 + PUDDLE_SIG_WINDOW_MS, hdr, sizeof hdr);
    CHECK(check(hdr, "GET", "/c", NULL, bid, &alice_at_bob, r4, T0) == PUDDLE_AUTHN_OK);                              /* her clock fast by 5 min */
    puddle_reqsig_sign(alice, "GET", "/d", NULL, 0, bid, T0 + PUDDLE_SIG_WINDOW_MS + 1, hdr, sizeof hdr);
    CHECK(check(hdr, "GET", "/d", NULL, bid, &alice_at_bob, r4, T0) == PUDDLE_AUTHN_SKEW);
    /* skew is only ever reported to someone who has proved who they are */
    puddle_reqsig_sign(mallory, "GET", "/e", NULL, 0, bid, T0 - 3600000, mh, sizeof mh);
    CHECK(check(mh, "GET", "/e", NULL, bid, &alice_at_bob, r4, T0) == PUDDLE_AUTHN_REJECT);
    puddle_replay_free(r4);

    /* ---- nonce memory is bounded, and forgets ---- */
    puddle_replay *small = puddle_replay_new(8); /* room for 4 */
    for (int i = 0; i < 4; i++) {
        puddle_reqsig_sign(alice, "GET", "/n", NULL, 0, bid, T0, hdr, sizeof hdr);
        CHECK(check(hdr, "GET", "/n", NULL, bid, &alice_at_bob, small, T0) == PUDDLE_AUTHN_OK);
    }
    puddle_reqsig_sign(alice, "GET", "/n", NULL, 0, bid, T0, hdr, sizeof hdr);
    CHECK(check(hdr, "GET", "/n", NULL, bid, &alice_at_bob, small, T0) == PUDDLE_AUTHN_BUSY);
    int64_t later = T0 + 11 * 60 * 1000; /* ten minutes on, the old nonces are gone */
    puddle_reqsig_sign(alice, "GET", "/n", NULL, 0, bid, later, hdr, sizeof hdr);
    CHECK(check(hdr, "GET", "/n", NULL, bid, &alice_at_bob, small, later) == PUDDLE_AUTHN_OK);
    puddle_replay_free(small);
    CHECK(puddle_replay_new(3) == NULL && puddle_replay_new(12) == NULL);

    /* ---- Alice changes her keys ---- */
    puddle_replay *r5 = puddle_replay_new(0);
    puddle_reqsig_sign(alice, "GET", "/before", NULL, 0, bid, T0, hdr, sizeof hdr); /* signed by the key she is about to retire */
    char err[300], new_phrase[PUDDLE_PHRASE_LEN + 1];
    puddle_roll *roll = puddle_identity_roll_prepare(alice, alice_phrase, new_phrase, err, sizeof err);
    CHECK(roll != NULL && puddle_identity_roll_commit(alice, roll, err, sizeof err) == 0);
    puddle_reqsig_sign(alice, "GET", "/after", NULL, 0, bid, T0, hdr2, sizeof hdr2);
    /* Bob hasn't heard yet: the new key is unknown to him, so it looks like a stranger */
    CHECK(check(hdr2, "GET", "/after", NULL, bid, &alice_at_bob, r5, T0) == PUDDLE_AUTHN_REJECT);
    /* once her pushed chain verifies and he updates his record, it is Alice again */
    friend_rec updated;
    learn(&updated, alice);
    CHECK(strcmp(updated.id, aid) == 0 && strcmp(updated.kid, alice_at_bob.kid) != 0); /* same Alice, new key */
    CHECK(check(hdr2, "GET", "/after", NULL, bid, &updated, r5, T0) == PUDDLE_AUTHN_OK);
    /* and the key she retired can no longer start anything */
    CHECK(check(hdr, "GET", "/before", NULL, bid, &updated, r5, T0) == PUDDLE_AUTHN_REJECT);
    puddle_replay_free(r5);

    /* ---- strict parsing ---- */
    puddle_reqsig_sign(alice, "GET", "/p", NULL, 0, bid, T0, hdr, sizeof hdr);
    CHECK(puddle_reqsig_parse(hdr, NULL) == 0);
    char m[600];
    #define MUT(expr) do { snprintf(m, sizeof m, "%s", hdr); expr; CHECK(puddle_reqsig_parse(m, NULL) == -1); } while (0)
    MUT(m[0] = 'V');                                         /* version */
    MUT(m[1] = '2');
    MUT(memcpy(m + 3, "ID", 2));                             /* field name case */
    MUT(m[strlen(m) - 1] = '!');                             /* sig not base64 */
    MUT(strcat(m, " "));                                     /* trailing space */
    MUT(strcat(m, "; x=1"));                                 /* extra field */
    MUT(strcat(m, "A"));                                     /* wrong signature length */
    MUT(m[3 + 3 + 1] = 'A');                                 /* id character outside the alphabet */
    MUT(*strstr(m, "; kid=") = ',');                         /* separators are exact */
    MUT(*strstr(m, "ts=") = 'T');
    MUT({ char *t = strstr(m, "ts=") + 3; *t = 'x'; });      /* ts not a number */
    MUT({ char *t = strstr(m, "kid=") + 4; t[10] = 'G'; });  /* kid not hex */
    MUT({ char *t = strstr(m, "kid=") + 4; t[63] = '\0'; strcat(m, "x"); }); /* kid wrong length */
    MUT({ char *t = strstr(m, "nonce=") + 6; t[0] = '$'; });
    CHECK(puddle_reqsig_parse("", NULL) == -1);
    CHECK(puddle_reqsig_parse(NULL, NULL) == -1);
    char longh[1100];
    memset(longh, 'a', sizeof longh - 1);
    longh[sizeof longh - 1] = '\0';
    CHECK(puddle_reqsig_parse(longh, NULL) == -1);
    CHECK(check("", "GET", "/", NULL, bid, &alice_at_bob, NULL, T0) == PUDDLE_AUTHN_REJECT);

    /* ---- signing refuses nonsense ---- */
    CHECK(puddle_reqsig_sign(alice, "get", "/x", NULL, 0, bid, T0, hdr, sizeof hdr) == -1);        /* method */
    CHECK(puddle_reqsig_sign(alice, "GET", "x", NULL, 0, bid, T0, hdr, sizeof hdr) == -1);         /* target */
    CHECK(puddle_reqsig_sign(alice, "GET", "/a b", NULL, 0, bid, T0, hdr, sizeof hdr) == -1);
    CHECK(puddle_reqsig_sign(alice, "GET", "/a\nb", NULL, 0, bid, T0, hdr, sizeof hdr) == -1);
    CHECK(puddle_reqsig_sign(alice, "GET", "/x", NULL, 0, "nobody", T0, hdr, sizeof hdr) == -1);   /* recipient */
    CHECK(puddle_reqsig_sign(alice, "GET", "/x", NULL, 0, bid, T0, hdr, 50) == -1);                /* too small */

    puddle_replay_free(replay);
    puddle_replay_free(r2);
    puddle_identity_close(alice);
    puddle_identity_close(bob);
    puddle_identity_close(carol);
    puddle_identity_close(mallory);
    nftw(root, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    return failures ? 1 : 0;
}
