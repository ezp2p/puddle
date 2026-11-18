#include "http/router.h"

#include "core/profile.h"

#include "platform/clock.h"
#include "core/error.hpp"
#include "storage/media.h"
#include "core/link.h"
#include "core/streams.h"
#include "http/reqsig.h"
#include "base/hash.h"
#include "base/seal.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include "platform/fsutil.h"

#include <new>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BODY_BUF 28000 /* what a handler may write; the rest of the 32 KB response is headers */

int64_t puddle_call_now_ms(const puddle_call *call) { return puddle_ctx_now_ms(call->ctx); }

static int param_ok(const char *type, size_t tl, const char *v, size_t vl)
{
    char tmp[PUDDLE_PARAM_MAX];
    if (vl == 0 || vl >= sizeof tmp)
        return 0;
    memcpy(tmp, v, vl);
    tmp[vl] = '\0';
    if (tl == 2 && !memcmp(type, "id", 2))
        return puddle_id_valid(tmp);
    if (tl == 5 && !memcmp(type, "event", 5)) {
        if (vl != 64)
            return 0;
        for (size_t i = 0; i < vl; i++)
            if (!((tmp[i] >= '0' && tmp[i] <= '9') || (tmp[i] >= 'a' && tmp[i] <= 'f')))
                return 0;
        return 1;
    }
    if (tl == 7 && !memcmp(type, "profile", 7))
        return puddle_profile_id_valid(tmp);
    if (tl == 5 && !memcmp(type, "index", 5)) { /* a count from 0: digits, no leading zero, below a million */
        if (vl > 6 || (vl > 1 && tmp[0] == '0'))
            return 0;
        for (size_t i = 0; i < vl; i++)
            if (tmp[i] < '0' || tmp[i] > '9')
                return 0;
        return 1;
    }
    if (tl == 3 && !memcmp(type, "num", 3)) { /* a number from 1: digits, no leading zero, up to 18 of them */
        if (vl > 18 || tmp[0] == '0')
            return 0;
        for (size_t i = 0; i < vl; i++)
            if (tmp[i] < '0' || tmp[i] > '9')
                return 0;
        return 1;
    }
    if (tl == 5 && !memcmp(type, "token", 5)) {
        if (vl > 64)
            return 0;
        for (size_t i = 0; i < vl; i++) {
            char ch = tmp[i];
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_'))
                return 0;
        }
        return 1;
    }
    return 0;
}

/* Does `path` fit `pattern`? Fills `call`'s parameters when it is given. */
static int path_match(const char *pattern, const char *path, puddle_call *call)
{
    int np = 0;
    const char *p = pattern, *s = path;
    while (*p && *s) {
        if (*p != *s && *p != '{')
            return 0;
        if (*p != '{') {
            p++;
            s++;
            continue;
        }
        const char *close = strchr(p, '}'), *colon = strchr(p, ':');
        if (!close || !colon || colon > close || np >= PUDDLE_PATH_PARAMS)
            return 0;
        const char *end = strchr(s, '/');
        size_t vl = end ? (size_t)(end - s) : strlen(s);
        if (!param_ok(colon + 1, (size_t)(close - colon - 1), s, vl))
            return 0;
        if (call) {
            size_t nl = (size_t)(colon - p - 1);
            snprintf(call->param_name[np], sizeof call->param_name[np], "%.*s", (int)nl, p + 1);
            snprintf(call->param[np], sizeof call->param[np], "%.*s", (int)vl, s);
        }
        np++;
        p = close + 1;
        s += vl;
    }
    return !*p && !*s;
}

static const puddle_route *find(const puddle_router *r, const char *method, const char *path)
{
    for (size_t i = 0; i < r->count; i++)
        if (!strcmp(r->routes[i].method, method) && path_match(r->routes[i].path, path, NULL))
            return &r->routes[i];
    return NULL;
}

const char *puddle_param(const puddle_call *call, const char *name)
{
    for (int i = 0; i < PUDDLE_PATH_PARAMS; i++)
        if (call->param_name[i][0] && !strcmp(call->param_name[i], name))
            return call->param[i];
    return "";
}

/* Before the body is read, so nothing is checked yet but who the signature says it is from: a device on the list (an
 * accepted one, with `active`)? The signature itself is checked once the body is in. */
static int claims_device(const puddle_ctx *ctx, const puddle_request *req, int active)
{
    puddle_reqsig s;
    puddle_device dv;
    if (!ctx->devices || !req->signature[0] || puddle_reqsig_parse(req->signature, &s) != 0)
        return 0;
    return active ? puddle_devices_is_active(ctx->devices, s.id) : puddle_devices_get(ctx->devices, s.id, &dv) == 0;
}

/* the most a route takes, sealed or not: sealing adds its tag to a body (spec/09) */
static size_t route_max(const puddle_route *rt, const puddle_request *req)
{
    return rt->body_max && req->seal[0] ? rt->body_max + PUDDLE_SEAL_TAG_LEN : rt->body_max;
}

size_t puddle_router_body_limit(const puddle_router *r, const puddle_request *req, int owner_ok)
{
    const puddle_route *rt = find(r, req->method, req->path);
    if (!rt)
        return 0;
    switch (rt->auth) {
    case PUDDLE_AUTH_LOCAL: return owner_ok ? rt->body_max : 0;
    case PUDDLE_AUTH_OWNER: return owner_ok || claims_device(r->ctx, req, 1) ? route_max(rt, req) : 0;
    case PUDDLE_AUTH_DEVICE: return claims_device(r->ctx, req, 0) ? route_max(rt, req) : 0;
    default: return route_max(rt, req);
    }
}

int puddle_query_parse(const char *target, const char *const *keys, size_t nkeys, char (*values)[PUDDLE_QUERY_VALUE_MAX])
{
    for (size_t i = 0; i < nkeys; i++)
        values[i][0] = '\0';
    const char *q = strchr(target, '?');
    if (!q)
        return 0;
    q++;
    while (*q) {
        const char *eq = strchr(q, '=');
        const char *end = strchr(q, '&');
        if (!end)
            end = q + strlen(q);
        if (!eq || eq > end || end == q)
            return -1;
        size_t kl = (size_t)(eq - q), vl = (size_t)(end - eq - 1);
        if (vl == 0 || vl >= PUDDLE_QUERY_VALUE_MAX)
            return -1;
        size_t k = 0;
        while (k < nkeys && !(strlen(keys[k]) == kl && !strncmp(keys[k], q, kl)))
            k++;
        if (k == nkeys || values[k][0])
            return -1; /* unknown or repeated */
        for (size_t i = 0; i < vl; i++) {
            char c = eq[1 + i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
                return -1;
        }
        memcpy(values[k], eq + 1, vl);
        values[k][vl] = '\0';
        q = *end ? end + 1 : end;
        if (*end && !*q)
            return -1; /* a trailing & */
    }
    return 0;
}

int puddle_fail(puddle_jw *w, int status, const char *code)
{
    puddle_jw_object(w);
    puddle_jw_str(w, "error", code);
    puddle_jw_end_object(w);
    return status;
}

static int no_peer(void *u, const char *a, const char *b, unsigned char c[32])
{
    (void)u; (void)a; (void)b; (void)c;
    return 0;
}

typedef struct {
    puddle_ctx *ctx;
    const puddle_route *route;
    const unsigned char *body;
    size_t len;
} chain_look;

static int chain_lookup(void *u, const char *sender, const char *kid, unsigned char pub[32])
{
    chain_look *c = (chain_look *)u;
    return c->route->chain_key && c->route->chain_key(c->ctx, c->body, c->len, sender, kid, pub);
}

/* Whether an answer to this authenticated sender is signed for. A PEER route only ever reaches someone whose key we hold. A
 * CHAIN route can be reached by anyone with a chain of their own, so only someone we already hold a record of is answered in
 * our name (asked after the handler, which may just have filed them: an answer to our request). Routes that must answer
 * strangers the same whatever they decide never are. docs/streams.md, "Proving who answers". */
static int may_sign(const puddle_ctx *ctx, const puddle_route *rt, const char *sender)
{
    switch (rt->auth) {
    case PUDDLE_AUTH_CHAIN_ANON:
    case PUDDLE_AUTH_CHAIN_OPEN:
        return 0;
    case PUDDLE_AUTH_CHAIN:
        return puddle_streams_knows(ctx->streams, sender);
    default: /* a peer, or one of the owner's own devices (one pairing has shown the secret) */
        return 1;
    }
}

/* "X-Puddle-Receipt: ...\r\n" for an answer whose body (as sent) has the SHA-256 `hash` (hex), or "" when it is not to be
 * signed for */
static void receipt_line_hash(const puddle_ctx *ctx, const char *nonce, int status, const char *hash, char *out, size_t cap)
{
    char r[PUDDLE_RECEIPT_MAX];
    out[0] = '\0';
    if (nonce && *nonce && puddle_receipt_sign_hash(ctx->identity, nonce, status, hash, r, sizeof r) == 0)
        snprintf(out, cap, "X-Puddle-Receipt: %s\r\n", r);
}

static void receipt_line(const puddle_ctx *ctx, const char *nonce, int status, const void *body, size_t len, char *out, size_t cap)
{
    char hash[65];
    out[0] = '\0';
    if (!nonce || !*nonce)
        return;
    puddle_sha256_hex(body ? body : "", body ? len : 0, hash);
    receipt_line_hash(ctx, nonce, status, hash, out, cap);
}

/* The SHA-256 (hex) of `len` bytes of an open file from where it stands, put back where it was. 0 or -1. */
static int hash_file_range(int fd, int64_t off, int64_t len, char hash[65])
{
    EVP_MD_CTX *md = EVP_MD_CTX_new();
    unsigned char buf[65536], out[32];
    unsigned ol = 0;
    int ok = md && EVP_DigestInit_ex(md, EVP_sha256(), NULL) == 1;
    int64_t left = len;
    while (ok && left > 0) {
        int64_t got = puddle_fd_read(fd, buf, left < (int64_t)sizeof buf ? (size_t)left : sizeof buf);
        if (got <= 0) {
            ok = 0;
            break;
        }
        ok = EVP_DigestUpdate(md, buf, (size_t)got) == 1;
        left -= got;
    }
    ok = ok && EVP_DigestFinal_ex(md, out, &ol) == 1 && ol == 32 && puddle_fd_seek(fd, off) == off;
    EVP_MD_CTX_free(md);
    if (ok)
        puddle_hex_encode(out, 32, hash);
    return ok ? 0 : -1;
}

/* The SHA-256 (hex) of records `first`.. of an open file, sealed as send_sealed_file (http/serve.cpp) will send them, from
 * where it stands, `len` bytes of plaintext; put back where it was. 0 or -1. */
static int hash_sealed_range(int fd, int64_t off, int64_t len, const unsigned char key[32], int status, uint64_t first, uint64_t final,
                             char hash[65])
{
    EVP_MD_CTX *md = EVP_MD_CTX_new();
    unsigned char *buf = (unsigned char *)malloc(PUDDLE_SEAL_RECORD), *sealed = (unsigned char *)malloc(PUDDLE_SEAL_RECORD + PUDDLE_SEAL_TAG_LEN);
    unsigned char out[32];
    unsigned ol = 0;
    int ok = md && buf && sealed && EVP_DigestInit_ex(md, EVP_sha256(), NULL) == 1;
    int64_t left = len;
    for (uint64_t i = first; ok; i++) {
        size_t want = left < (int64_t)PUDDLE_SEAL_RECORD ? (size_t)left : PUDDLE_SEAL_RECORD, got = 0;
        while (ok && got < want) {
            int64_t n = puddle_fd_read(fd, buf + got, want - got);
            ok = n > 0;
            got += ok ? (size_t)n : 0;
        }
        ok = ok && puddle_seal_record(key, i, status, i == final, buf, want, sealed) == 0 &&
             EVP_DigestUpdate(md, sealed, want + PUDDLE_SEAL_TAG_LEN) == 1;
        left -= (int64_t)want;
        if (i == final || left <= 0)
            break;
    }
    ok = ok && EVP_DigestFinal_ex(md, out, &ol) == 1 && ol == 32 && puddle_fd_seek(fd, off) == off;
    EVP_MD_CTX_free(md);
    free(buf);
    free(sealed);
    if (ok)
        puddle_hex_encode(out, 32, hash);
    return ok ? 0 : -1;
}

/* How an answer goes back to a sealed request: under the key its context exports */
typedef struct {
    int on;
    unsigned char key[32];
} answer_seal;

/* An answer with `body`: sealed if the request was, signed for if `nonce` is set. Into `out` when it fits, else the body
 * goes as `big->data` after the headers. `extra` is "" or header lines. */
static size_t respond(const puddle_ctx *ctx, const answer_seal *as, const char *nonce, int status, const char *mime, const char *extra,
                      const void *body, size_t n, char *out, size_t cap, puddle_big *big)
{
    char receipt[PUDDLE_RECEIPT_MAX + 32], hdr[PUDDLE_RECEIPT_MAX + 400];
    unsigned char *sealed = NULL;
    if (status == 204) { /* no body, so nothing to seal; still signed for */
        receipt_line(ctx, nonce, 204, "", 0, receipt, sizeof receipt);
        snprintf(hdr, sizeof hdr, "%s%s", extra ? extra : "", receipt);
        return puddle_http_response(204, hdr, "", 0, out, cap);
    }
    if (as && as->on) {
        size_t sl = puddle_seal_answer_len(n);
        sealed = (unsigned char *)malloc(sl);
        if (!sealed || puddle_seal_answer(as->key, status, body, n, sealed) != 0) {
            free(sealed);
            return puddle_http_error(500, out, cap);
        }
        body = sealed;
        n = sl;
        mime = PUDDLE_SEALED_TYPE;
    }
    receipt_line(ctx, nonce, status, body, n, receipt, sizeof receipt);
    snprintf(hdr, sizeof hdr, "%s%s", extra ? extra : "", receipt);
    size_t hl = puddle_http_head(status, mime, hdr, n, out, cap);
    if (!hl) {
        free(sealed);
        return puddle_http_error(500, out, cap);
    }
    if (hl + n <= cap) {
        if (n)
            memcpy(out + hl, body, n);
        free(sealed);
        return hl + n;
    }
    if (!sealed) {
        sealed = (unsigned char *)malloc(n);
        if (!sealed)
            return puddle_http_error(500, out, cap);
        memcpy(sealed, body, n);
    }
    big->data = sealed;
    big->len = n;
    return hl;
}

static size_t not_found(char *out, size_t cap, int *status)
{
    *status = 404;
    return puddle_http_error(404, out, cap);
}

size_t puddle_router_handle(const puddle_router *r, const puddle_request *req, int owner_ok,
                            const unsigned char *body, size_t body_len, char *out, size_t cap, int *status)
{
    /* the plain form: a file answer has to fit in `out` with its headers, as it can for a small image in a test */
    puddle_big big;
    size_t n = puddle_router_handle_big(r, req, owner_ok, body, body_len, out, cap, status, &big);
    if (big.has_file) { /* read into `out` too, if it fits */
        int64_t got = n && n + (size_t)big.file_len <= cap ? puddle_fd_read(big.fd, out + n, (size_t)big.file_len) : -1;
        puddle_fd_close(big.fd);
        if (got == big.file_len) {
            n += (size_t)got;
        } else {
            *status = 500;
            n = puddle_http_error(500, out, cap);
        }
    }
    if (big.data) {
        if (n && n + big.len <= cap) {
            memcpy(out + n, big.data, big.len);
            n += big.len;
        } else {
            *status = 500;
            n = puddle_http_error(500, out, cap);
        }
        free(big.data);
    }
    return n;
}

size_t puddle_router_handle_big(const puddle_router *r, const puddle_request *req, int owner_ok,
                                const unsigned char *body, size_t body_len, char *out, size_t cap, int *status, puddle_big *big)
{
    memset(big, 0, sizeof *big);
    char json[BODY_BUF];
    puddle_jw w;
    puddle_ctx *ctx = r->ctx;
    const puddle_route *rt = find(r, req->method, req->path);

    if (!rt) {
        /* The owner is told when a path exists but not for that method; anyone else learns nothing. */
        if (owner_ok) {
            char allow[64] = "";
            for (size_t i = 0; i < r->count; i++)
                if (path_match(r->routes[i].path, req->path, NULL) && r->routes[i].auth == PUDDLE_AUTH_OWNER &&
                    strlen(allow) + strlen(r->routes[i].method) + 4 < sizeof allow)
                    snprintf(allow + strlen(allow), sizeof allow - strlen(allow), "%s%s", *allow ? ", " : "",
                             r->routes[i].method);
            if (*allow) {
                char hdr[96];
                snprintf(hdr, sizeof hdr, "Allow: %s\r\n", allow);
                *status = 405;
                return puddle_http_response(405, hdr, "{\"error\":\"Method Not Allowed\"}", 29, out, cap);
            }
        }
        return not_found(out, cap, status);
    }
    if (body_len > route_max(rt, req))
        return not_found(out, cap, status); /* the server never reads this much; belt and braces */

    puddle_blob blob;
    memset(&blob, 0, sizeof blob);
    puddle_call call;
    memset(&call, 0, sizeof call);
    call.ctx = ctx;
    call.blob = &blob;
    call.req = req;
    call.body = body;
    call.body_len = body_len;
    call.auth = rt->auth;
    path_match(rt->path, req->path, &call);
    char sender[PUDDLE_USER_ID_LEN + 1];
    char nonce[48] = "";           /* the authenticated request's, which every answer to it signs for; "" for the owner */
    answer_seal as;                /* a sealed request is answered sealed (spec/09) */
    memset(&as, 0, sizeof as);
    unsigned char *opened = NULL;  /* a sealed body, opened: what the handler sees */
    size_t total = 0;

    int by_token = (rt->auth == PUDDLE_AUTH_OWNER || rt->auth == PUDDLE_AUTH_LOCAL) && owner_ok;
    if (rt->auth == PUDDLE_AUTH_LOCAL && !owner_ok)
        return not_found(out, cap, status);
    if (!by_token) {
        char me[PUDDLE_USER_ID_LEN + 1];
        puddle_identity_id(ctx->identity, me);
        int chain = rt->auth == PUDDLE_AUTH_CHAIN || rt->auth == PUDDLE_AUTH_CHAIN_OPEN || rt->auth == PUDDLE_AUTH_CHAIN_ANON ||
                    rt->auth == PUDDLE_AUTH_PAIR;
        int first_contact = rt->auth == PUDDLE_AUTH_CHAIN_OPEN || rt->auth == PUDDLE_AUTH_CHAIN_ANON;
        int device = rt->auth == PUDDLE_AUTH_OWNER || rt->auth == PUDDLE_AUTH_DEVICE;
        if (device && !ctx->devices)
            return not_found(out, cap, status);

        /* A sealed body is opened before the signature is checked, since a chain that proves the sender may be inside it. One
         * that won't open is treated like a bad signature: a stranger learns nothing. One sealed to a key we have rolled away
         * from is told so, once the signature shows who sent it. */
        int stale = 0;
        if (req->seal[0]) {
            char kid[65], mine[65];
            unsigned char enc[PUDDLE_SEAL_ENC_LEN];
            puddle_reqsig claimed;
            if (puddle_seal_header_parse(req->seal, kid, enc) != 0 || puddle_reqsig_parse(req->signature, &claimed) != 0)
                return not_found(out, cap, status);
            puddle_identity_seal_kid(ctx->identity, mine);
            if (strcmp(kid, mine) != 0) {
                stale = 1;
            } else {
                char info[200], aad[1200];
                size_t al = puddle_seal_aad(req->method, req->target, aad, sizeof aad);
                const char *for_whom[2] = { me, first_contact ? PUDDLE_RECIPIENT_ANY : NULL };
                for (int i = 0; i < 2 && !as.on && for_whom[i]; i++) {
                    size_t il = puddle_seal_info(claimed.id, for_whom[i], claimed.nonce, info, sizeof info);
                    puddle_hpke hc;
                    if (!il || !al || puddle_identity_open_seal(ctx->identity, enc, info, il, &hc) != 0)
                        continue;
                    if (body_len) {
                        if (body_len < PUDDLE_SEAL_TAG_LEN || !(opened = (unsigned char *)malloc(body_len - PUDDLE_SEAL_TAG_LEN + 1)) ||
                            puddle_hpke_open(&hc, aad, al, body, body_len, opened) != 0) {
                            free(opened);
                            opened = NULL;
                            continue;
                        }
                        opened[body_len - PUDDLE_SEAL_TAG_LEN] = '\0';
                    }
                    as.on = puddle_seal_answer_key(&hc, as.key) == 0;
                }
                if (!as.on) {
                    free(opened);
                    return not_found(out, cap, status);
                }
                call.body = opened ? opened : (const unsigned char *)"";
                call.body_len = opened ? body_len - PUDDLE_SEAL_TAG_LEN : 0;
            }
        }

        chain_look cl = { ctx, rt, call.body, call.body_len };
        puddle_peer_key_fn lookup = chain ? chain_lookup
                                    : device ? (rt->auth == PUDDLE_AUTH_OWNER ? puddle_devices_key : puddle_devices_key_any)
                                    : (ctx->peer_key ? ctx->peer_key : no_peer);
        void *user = chain ? (void *)&cl : device ? (void *)ctx->devices : ctx->peer_key_user;
        int64_t now = puddle_ctx_now_ms(ctx);
        puddle_reqsig sig;
        /* the signature covers the body as sent: sealed, when it is */
        puddle_authn a = puddle_reqsig_authenticate(req->signature, req->method, req->target, body, body_len, me,
                                                    lookup, user, ctx->replay, now, &sig);
        if (a == PUDDLE_AUTHN_REJECT && rt->auth == PUDDLE_AUTH_CHAIN_OPEN) /* a first request names no recipient */
            a = puddle_reqsig_authenticate(req->signature, req->method, req->target, body, body_len, PUDDLE_RECIPIENT_ANY,
                                           lookup, user, ctx->replay, now, &sig);
        if (a == PUDDLE_AUTHN_REJECT) {
            free(opened);
            return not_found(out, cap, status);
        }
        /* from here the sender is who it says, so it may be told what went wrong, and (if they are no stranger) signed for */
        if (may_sign(ctx, rt, sig.id))
            snprintf(nonce, sizeof nonce, "%s", sig.nonce);
        puddle_jw_init(&w, json, sizeof json);
        if (a == PUDDLE_AUTHN_SKEW) {
            puddle_jw_object(&w);
            puddle_jw_str(&w, "error", "clock_skew");
            puddle_jw_num(&w, "now", now);
            puddle_jw_end_object(&w);
            *status = 401;
        } else if (a == PUDDLE_AUTHN_REPLAYED) {
            *status = puddle_fail(&w, 401, "replayed");
        } else if (a != PUDDLE_AUTHN_OK) {
            *status = puddle_fail(&w, 503, "busy");
        } else if (stale) { /* they hold a key of ours we have rolled away from: here is our chain, to try again with */
            size_t cl_len = 0;
            char *chain_text = puddle_identity_chain(ctx->identity, &cl_len);
            puddle_jw_object(&w);
            puddle_jw_str(&w, "error", "stale_key");
            if (chain_text)
                puddle_jw_str(&w, "chain", chain_text);
            puddle_jw_end_object(&w);
            free(chain_text);
            *status = 409;
        } else if (!as.on && !first_contact) { /* only a first request to a bare address may come unsealed */
            *status = puddle_fail(&w, 400, "not_sealed");
        } else {
            memcpy(sender, sig.id, sizeof sender);
            if (rt->auth == PUDDLE_AUTH_PEER && ctx->outbox) /* a friend we couldn't reach is up after all */
                puddle_outbox_heard_from(ctx->outbox, sender);
            if (rt->auth == PUDDLE_AUTH_OWNER)
                call.device = sender; /* the owner, on one of their phones */
            else
                call.sender = sender;
            if (device)
                puddle_devices_seen(ctx->devices, sender, now);
            goto run;
        }
        size_t n = puddle_jw_finish(&w);
        free(opened);
        return respond(ctx, &as, nonce, *status, "application/json", "Cache-Control: no-store\r\n", json, n, out, cap, big);
    }

run:
    puddle_jw_init(&w, json, sizeof json);
    int st;
    try {
        st = rt->fn(&call, &w);
    } catch (const puddle::Error &e) {
        /* an object refused: whatever the handler had started writing is replaced by the error */
        puddle_jw_init(&w, json, sizeof json);
        puddle_jw_object(&w);
        puddle_jw_str(&w, "error", e.code());
        if (e.message()[0])
            puddle_jw_str(&w, "message", e.message());
        puddle_jw_end_object(&w);
        st = e.status();
    } catch (const std::bad_alloc &) {
        puddle_jw_init(&w, json, sizeof json);
        st = puddle_fail(&w, 500, "internal");
    }
    if (opened) { /* the handler is done with it; it may have held a message */
        OPENSSL_cleanse(opened, call.body_len);
        free(opened);
        opened = NULL;
    }
    if (call.sender && !nonce[0] && may_sign(ctx, rt, call.sender)) {
        puddle_reqsig again; /* filed just now, by this very request (an answer to ours): now they are someone we know */
        if (puddle_reqsig_parse(req->signature, &again) == 0)
            snprintf(nonce, sizeof nonce, "%s", again.nonce);
    }
    if (blob.has_file && (st < 200 || st > 299)) { /* opened, then refused (or thrown): not sent */
        puddle_fd_close(blob.fd);
        blob.has_file = 0;
    }
    if (blob.has_file) { /* a file on disk: headers here, the bytes read and sent after them */
        int64_t off, len;
        int part = st == 200 ? puddle_http_range(req->range, blob.file_size, &off, &len) : 0;
        char hdr[400 + PUDDLE_RECEIPT_MAX + 32];
        if (part < 0) {
            puddle_fd_close(blob.fd);
            snprintf(hdr, sizeof hdr, "Content-Range: bytes */%lld\r\n", (long long)blob.file_size);
            *status = 416;
            static const char gone[] = "{\"error\":\"range_not_satisfiable\"}";
            return respond(ctx, &as, nonce, 416, "application/json", hdr, gone, sizeof gone - 1, out, cap, big);
        }
        if (!part) {
            off = 0;
            len = blob.file_size;
        }
        uint64_t first = 0, final = 0;
        if (as.on) { /* sealed: whole records, from the one the range starts in to the one it ends in (spec/09, "Downloads") */
            first = (uint64_t)(off / PUDDLE_SEAL_RECORD);
            final = blob.file_size ? (uint64_t)((blob.file_size - 1) / PUDDLE_SEAL_RECORD) : 0;
            uint64_t last = len ? (uint64_t)((off + len - 1) / PUDDLE_SEAL_RECORD) : first;
            int64_t end = (int64_t)(last + 1) * PUDDLE_SEAL_RECORD;
            off = (int64_t)first * PUDDLE_SEAL_RECORD;
            len = (end < blob.file_size ? end : blob.file_size) - off;
        }
        if (puddle_fd_seek(blob.fd, off) != off) {
            puddle_fd_close(blob.fd);
            *status = 500;
            return puddle_http_error(500, out, cap);
        }
        int n = snprintf(hdr, sizeof hdr, "Accept-Ranges: bytes\r\nX-Content-Type-Options: nosniff\r\nCache-Control: %s\r\n",
                         blob.immutable && !as.on ? "private, max-age=31536000, immutable" : "no-store");
        if (part) /* sealed, the range given is the records' plaintext, which the caller cuts what it asked for out of */
            snprintf(hdr + n, sizeof hdr - (size_t)n, "Content-Range: bytes %lld-%lld/%lld\r\n", (long long)off,
                     (long long)(off + len - 1), (long long)blob.file_size);
        *status = part ? 206 : st;
        size_t sent_len = (size_t)len;
        if (as.on) {
            uint64_t records = len ? (uint64_t)((len + PUDDLE_SEAL_RECORD - 1) / PUDDLE_SEAL_RECORD) : 1;
            sent_len = (size_t)len + (size_t)records * PUDDLE_SEAL_TAG_LEN;
        }
        if (nonce[0]) { /* signed for exactly the bytes that go: hashed from disk (sealed, as they will be), then sent from where it was */
            char hash[65], receipt[PUDDLE_RECEIPT_MAX + 32];
            int hr = as.on ? hash_sealed_range(blob.fd, off, len, as.key, *status, first, final, hash) : hash_file_range(blob.fd, off, len, hash);
            if (hr != 0) {
                puddle_fd_close(blob.fd);
                *status = 500;
                return puddle_http_error(500, out, cap);
            }
            receipt_line_hash(ctx, nonce, *status, hash, receipt, sizeof receipt);
            snprintf(hdr + strlen(hdr), sizeof hdr - strlen(hdr), "%s", receipt);
        }
        const char *mime = as.on ? PUDDLE_SEALED_TYPE : blob.mime[0] ? blob.mime : "application/octet-stream";
        size_t hl = puddle_http_head(*status, mime, hdr, sent_len, out, cap);
        if (!hl) {
            puddle_fd_close(blob.fd);
            *status = 500;
            return puddle_http_error(500, out, cap);
        }
        big->has_file = 1;
        big->fd = blob.fd;
        big->file_len = len;
        if (as.on) {
            big->sealed = 1;
            memcpy(big->seal_key, as.key, 32);
            big->seal_status = *status;
            big->first_record = first;
            big->final_record = final;
        }
        return hl;
    }
    if (blob.data) { /* a file: headers here, the bytes sent after them */
        *status = st;
        /* JSON built for this answer (too big for `out`) changes from one asking to the next; anything else is named by its hash */
        int fixed = strcmp(blob.mime, "application/json") != 0 && !as.on;
        size_t r = respond(ctx, &as, nonce, st, blob.mime[0] ? blob.mime : "application/octet-stream",
                           fixed ? "Cache-Control: private, max-age=31536000, immutable\r\nX-Content-Type-Options: nosniff\r\n"
                                 : "Cache-Control: no-store\r\n",
                           blob.data, blob.len, out, cap, big);
        free(blob.data);
        return r;
    }
    size_t n = 0;
    if (w.len == 0 && !w.err) {
        json[0] = '\0'; /* empty answer */
    } else {
        n = puddle_jw_finish(&w);
        if (n == 0) {
            *status = 500;
            return puddle_http_error(500, out, cap);
        }
    }
    *status = st;
    if (as.on) {
        total = respond(ctx, &as, nonce, st, "application/json", "Cache-Control: no-store\r\n", json, n, out, cap, big);
    } else {
        char receipt[PUDDLE_RECEIPT_MAX + 32];
        receipt_line(ctx, nonce, st, json, n, receipt, sizeof receipt);
        total = puddle_http_response(st, receipt, json, n, out, cap);
    }
    memset(json, 0, sizeof json); /* may have held a recovery phrase */
    OPENSSL_cleanse(&as, sizeof as);
    return total;
}

/* ---- answering with an image ---- */

int puddle_query_media_hash(const puddle_call *c, char out[PUDDLE_QUERY_VALUE_MAX])
{
    const char *keys[1] = { "hash" };
    char v[1][PUDDLE_QUERY_VALUE_MAX];
    if (puddle_query_parse(c->req->target, keys, 1, v) != 0 || !puddle_media_hash_valid(v[0]))
        return -1;
    memcpy(out, v[0], PUDDLE_QUERY_VALUE_MAX);
    return 0;
}

int puddle_answer_media(const puddle_call *c, const char *hash)
{
    unsigned char *data;
    size_t len;
    char mime[40];
    if (!c->ctx->config || puddle_media_read(c->ctx->config->data_dir, hash, PUDDLE_IMAGE_MAX_BYTES, &data, &len, mime) != 0)
        return 0;
    c->blob->data = data;
    c->blob->len = len;
    snprintf(c->blob->mime, sizeof c->blob->mime, "%s", mime);
    return 1;
}

int puddle_answer_file(const puddle_call *c, const char *path, const char *mime, int immutable)
{
    int fd = puddle_fd_open(path, PUDDLE_F_READ);
    if (fd < 0)
        return 0;
    int64_t size = puddle_fd_size(fd);
    if (size < 0) {
        puddle_fd_close(fd);
        return 0;
    }
    c->blob->has_file = 1;
    c->blob->fd = fd;
    c->blob->file_size = size;
    c->blob->immutable = immutable;
    snprintf(c->blob->mime, sizeof c->blob->mime, "%s", mime && *mime ? mime : "application/octet-stream");
    return 1;
}
