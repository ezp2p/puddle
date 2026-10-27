#include "http/reqsig.h"

#include "platform/clock.h"
#include "base/encoding.h"
#include "base/hash.h"

#include <mutex>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <openssl/rand.h>

#define NONCE_REMEMBER_MS (2 * PUDDLE_SIG_WINDOW_MS)

static int64_t now_ms(void)
{
    return puddle_wall_ms();
}

/* The exact bytes that are signed. */
static int canonical(char *out, size_t cap, const char *method, const char *target, const void *body,
                     size_t body_len, int64_t ts, const char *nonce, const char *recipient)
{
    char hash[65];
    puddle_sha256_hex(body ? body : "", body ? body_len : 0, hash);
    int n = snprintf(out, cap, "puddle-request-v1\n%s\n%s\n%s\n%lld\n%s\n%s", method, target, hash,
                     (long long)ts, nonce, recipient);
    return (n > 0 && (size_t)n < cap) ? n : -1;
}

static int valid_method(const char *m)
{
    size_t n = strlen(m);
    if (n == 0 || n > 7)
        return 0;
    for (size_t i = 0; i < n; i++)
        if (m[i] < 'A' || m[i] > 'Z')
            return 0;
    return 1;
}

/* a request target as sent: printable, no spaces or line breaks */
static int valid_target(const char *t)
{
    size_t n = strlen(t);
    if (n == 0 || n > 1100 || t[0] != '/')
        return 0;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)t[i] <= 0x20 || (unsigned char)t[i] >= 0x7f)
            return 0;
    return 1;
}

static int valid_user_id(const char *s)
{
    if (strlen(s) != PUDDLE_USER_ID_LEN)
        return 0;
    for (int i = 0; i < PUDDLE_USER_ID_LEN; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '2' && s[i] <= '7')))
            return 0;
    return 1;
}

int puddle_reqsig_sign(puddle_identity *me, const char *method, const char *target, const void *body,
                       size_t body_len, const char *recipient_id, int64_t at_ms, char *out, size_t cap)
{
    if (!valid_method(method) || !valid_target(target) || !(valid_user_id(recipient_id) || !strcmp(recipient_id, PUDDLE_RECIPIENT_ANY)))
        return -1;
    int64_t ts = at_ms ? at_ms : now_ms();

    unsigned char raw[16];
    char nonce[32];
    if (RAND_bytes(raw, sizeof raw) != 1 || !puddle_base64_encode(raw, sizeof raw, nonce, sizeof nonce))
        return -1;

    char canon[2400];
    int cl = canonical(canon, sizeof canon, method, target, body, body_len, ts, nonce, recipient_id);
    if (cl < 0)
        return -1;

    unsigned char sig[64], pub[32];
    char sig64[100], id[PUDDLE_USER_ID_LEN + 1], kid[65];
    if (puddle_identity_sign(me, canon, (size_t)cl, sig) != 0)
        return -1;
    puddle_identity_public(me, pub, NULL, NULL);
    puddle_identity_id(me, id);
    puddle_key_id(pub, kid);
    if (!puddle_base64_encode(sig, 64, sig64, sizeof sig64))
        return -1;

    int n = snprintf(out, cap, "v1 id=%s; kid=%s; ts=%lld; nonce=%s; sig=%s", id, kid, (long long)ts, nonce, sig64);
    return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

/* copy [p, p+len) into out if it is 1..cap-1 bytes */
static int field(const char *p, size_t len, char *out, size_t cap)
{
    if (len == 0 || len >= cap)
        return -1;
    memcpy(out, p, len);
    out[len] = '\0';
    return 0;
}

int puddle_reqsig_parse(const char *header, puddle_reqsig *out)
{
    static const char *const seps[] = { "v1 id=", "; kid=", "; ts=", "; nonce=", "; sig=" };
    if (!header || strlen(header) > 1000)
        return -1;
    const char *p = header;
    char vals[5][120];
    for (int i = 0; i < 5; i++) {
        size_t sl = strlen(seps[i]);
        if (strncmp(p, seps[i], sl) != 0)
            return -1;
        p += sl;
        const char *end = i < 4 ? strstr(p, seps[i + 1]) : header + strlen(header);
        if (!end || field(p, (size_t)(end - p), vals[i], sizeof vals[i]) != 0)
            return -1;
        p = end;
    }

    puddle_reqsig r;
    memset(&r, 0, sizeof r);
    unsigned char kidraw[32], tmp[72];
    size_t n = 0;
    if (!valid_user_id(vals[0]) || puddle_hex_decode(vals[1], kidraw, 32) != 0)
        return -1;
    if (strlen(vals[2]) > 15)
        return -1;
    for (const char *c = vals[2]; *c; c++)
        if (*c < '0' || *c > '9')
            return -1;
    if (strlen(vals[3]) < 16 || strlen(vals[3]) >= sizeof r.nonce)
        return -1;
    for (const char *c = vals[3]; *c; c++)
        if (!((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '+' || *c == '/' || *c == '='))
            return -1;
    if (puddle_base64_decode(vals[4], tmp, sizeof tmp, &n) != 0 || n != 64)
        return -1;

    memcpy(r.id, vals[0], sizeof r.id);
    memcpy(r.kid, vals[1], 65);
    r.ts = strtoll(vals[2], NULL, 10);
    memcpy(r.nonce, vals[3], strlen(vals[3]) + 1);
    memcpy(r.sig, tmp, 64);
    if (out)
        *out = r;
    return 0;
}

/* ------------------------------------------------------------ replay memory */

typedef struct {
    char key[100];
    int64_t expires;
} slot;

struct puddle_replay {
    std::mutex mu;
    size_t cap, used;
    slot *slots;
};

puddle_replay *puddle_replay_new(size_t capacity)
{
    if (capacity == 0)
        capacity = 65536;
    if (capacity < 8 || (capacity & (capacity - 1)) != 0)
        return NULL;
    puddle_replay *r = new (std::nothrow) puddle_replay();
    if (!r)
        return NULL;
    r->slots = (slot *)calloc(capacity, sizeof *r->slots);
    if (!r->slots) {
        delete r;
        return NULL;
    }
    r->cap = capacity;
    return r;
}

void puddle_replay_free(puddle_replay *r)
{
    if (!r)
        return;
    free(r->slots);
    delete r;
}

static size_t hash_key(const char *k)
{
    uint64_t h = 1469598103934665603ULL;
    for (; *k; k++)
        h = (h ^ (unsigned char)*k) * 1099511628211ULL;
    return (size_t)h;
}

static void insert_raw(slot *slots, size_t cap, const char *key, int64_t expires)
{
    size_t i = hash_key(key) & (cap - 1);
    while (slots[i].key[0])
        i = (i + 1) & (cap - 1);
    snprintf(slots[i].key, sizeof slots[i].key, "%s", key);
    slots[i].expires = expires;
}

/* drop expired entries by rebuilding the table */
static void purge(puddle_replay *r, int64_t now)
{
    slot *fresh = (slot *)calloc(r->cap, sizeof *fresh);
    if (!fresh)
        return;
    size_t used = 0;
    for (size_t i = 0; i < r->cap; i++)
        if (r->slots[i].key[0] && r->slots[i].expires > now) {
            insert_raw(fresh, r->cap, r->slots[i].key, r->slots[i].expires);
            used++;
        }
    free(r->slots);
    r->slots = fresh;
    r->used = used;
}

/* OK if new, REPLAYED if seen and still remembered, BUSY if there is no room */
static puddle_authn remember(puddle_replay *r, const char *sender, const char *nonce, int64_t now)
{
    char key[100];
    snprintf(key, sizeof key, "%.*s:%.31s", PUDDLE_USER_ID_LEN, sender, nonce); /* ids are 52 chars, nonces fit 32 bytes */
    puddle_authn rc = PUDDLE_AUTHN_OK;
    r->mu.lock();
    size_t i = hash_key(key) & (r->cap - 1);
    while (r->slots[i].key[0]) {
        if (strcmp(r->slots[i].key, key) == 0) {
            if (r->slots[i].expires > now) {
                rc = PUDDLE_AUTHN_REPLAYED;
            } else {
                r->slots[i].expires = now + NONCE_REMEMBER_MS; /* long gone: fresh again */
            }
            goto out;
        }
        i = (i + 1) & (r->cap - 1);
    }
    if ((r->used + 1) * 2 > r->cap) {
        purge(r, now);
        if ((r->used + 1) * 2 > r->cap) {
            rc = PUDDLE_AUTHN_BUSY;
            goto out;
        }
    }
    insert_raw(r->slots, r->cap, key, now + NONCE_REMEMBER_MS);
    r->used++;
out:
    r->mu.unlock();
    return rc;
}

/* ------------------------------------------------------------ authenticate */

puddle_authn puddle_reqsig_authenticate(const char *header, const char *method, const char *target,
                                        const void *body, size_t body_len, const char *my_id,
                                        puddle_peer_key_fn lookup, void *user, puddle_replay *replay,
                                        int64_t at_ms, puddle_reqsig *out)
{
    puddle_reqsig r;
    if (!method || !target || !my_id || !header || puddle_reqsig_parse(header, &r) != 0)
        return PUDDLE_AUTHN_REJECT;

    unsigned char pub[32];
    char kid_check[65];
    if (!lookup || !lookup(user, r.id, r.kid, pub))
        return PUDDLE_AUTHN_REJECT;
    puddle_key_id(pub, kid_check);
    if (strcmp(kid_check, r.kid) != 0) /* the lookup must hand back the key the request names */
        return PUDDLE_AUTHN_REJECT;

    char canon[2400];
    int cl = canonical(canon, sizeof canon, method, target, body, body_len, r.ts, r.nonce, my_id);
    if (cl < 0 || !puddle_verify_signature(pub, canon, (size_t)cl, r.sig))
        return PUDDLE_AUTHN_REJECT;

    /* the sender is now authenticated; from here we may say specific things */
    int64_t now = at_ms ? at_ms : now_ms();
    if (out)
        *out = r;
    int64_t skew = now - r.ts;
    if (skew > PUDDLE_SIG_WINDOW_MS || skew < -PUDDLE_SIG_WINDOW_MS)
        return PUDDLE_AUTHN_SKEW;
    if (replay)
        return remember(replay, r.id, r.nonce, now);
    return PUDDLE_AUTHN_OK;
}
