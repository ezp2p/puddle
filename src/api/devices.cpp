/* Phones paired with this Puddle (docs/identity.md, "Devices"): the owner's routes to let them in and out (owner token only,
 * so a phone can't add or remove phones), and the two a phone calls itself, to ask to be paired and to see how it stands. */
#include "api/api.h"

#include "base/encoding.h"
#include "base/json.h"
#include "base/log.h"
#include "core/devices.h"
#include "core/link.h"
#include "core/ratelimit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *body_text(const puddle_call *c) { return c->body_len ? (const char *)c->body : "{}"; }
static size_t body_size(const puddle_call *c) { return c->body_len ? c->body_len : 2; }

static int member_str(const puddle_call *c, const char *key, char *out, size_t cap)
{
    puddle_span v;
    return puddle_json_valid_object(body_text(c), body_size(c)) && puddle_json_member(body_text(c), body_size(c), key, &v) == 0 &&
                   puddle_json_string(v, out, cap) == 0
               ? 0
               : -1;
}

static void device_json(puddle_jw *w, const puddle_device *d)
{
    char check[7];
    puddle_device_check(d->pub, check);
    puddle_jw_object(w);
    puddle_jw_str(w, "id", d->id);
    puddle_jw_str(w, "name", d->name);
    puddle_jw_str(w, "state", d->state == PUDDLE_DEVICE_ACTIVE ? "active" : "pending");
    puddle_jw_str(w, "check", check);
    puddle_jw_num(w, "created_at", d->created_ms);
    if (d->last_seen_ms)
        puddle_jw_num(w, "last_seen", d->last_seen_ms);
    puddle_jw_end_object(w);
}

/* ---- the owner's side (owner token only) ---- */

static int list_devices(const puddle_call *c, puddle_jw *w)
{
    puddle_device *all;
    size_t n;
    if (!c->ctx->devices || puddle_devices_list(c->ctx->devices, &all, &n) != 0)
        return puddle_fail(w, 500, "internal");
    puddle_jw_object(w);
    puddle_jw_key(w, "devices");
    puddle_jw_array(w);
    for (size_t i = 0; i < n; i++)
        device_json(w, &all[i]);
    puddle_jw_end_array(w);
    puddle_jw_end_object(w);
    free(all);
    return 200;
}

static int start_pairing(const puddle_call *c, puddle_jw *w)
{
    char where[PUDDLE_ADDR_MAX], me[PUDDLE_USER_ID_LEN + 1], secret[PUDDLE_PAIR_SECRET_LEN + 1], link[400];
    if (!c->ctx->devices)
        return puddle_fail(w, 500, "internal");
    if (!c->ctx->config || !puddle_my_address(c->ctx->config, where, sizeof where))
        return puddle_fail(w, 409, "no_address"); /* a phone has to be told where to find us */
    int64_t until;
    if (puddle_devices_new_secret(c->ctx->devices, puddle_call_now_ms(c), secret, &until) != 0)
        return puddle_fail(w, 500, "internal");
    puddle_identity_id(c->ctx->identity, me);
    snprintf(link, sizeof link, "puddle-pair://%s?id=%s&secret=%s", where, me, secret);
    puddle_jw_object(w);
    puddle_jw_str(w, "link", link);
    puddle_jw_num(w, "expires_at", until);
    puddle_jw_end_object(w);
    memset(secret, 0, sizeof secret);
    memset(link, 0, sizeof link);
    return 201;
}

static int device_ref(const puddle_call *c, char id[PUDDLE_DEVICE_ID_LEN + 1])
{
    const char *p = puddle_param(c, "device");
    if (!puddle_id_valid(p))
        return -1;
    snprintf(id, PUDDLE_DEVICE_ID_LEN + 1, "%s", p);
    return 0;
}

static int accept_device(const puddle_call *c, puddle_jw *w)
{
    char id[PUDDLE_DEVICE_ID_LEN + 1], err[300];
    if (device_ref(c, id) != 0)
        return puddle_fail(w, 400, "bad_request");
    int rc = puddle_devices_accept(c->ctx->devices, id, err, sizeof err);
    if (rc == 1)
        return puddle_fail(w, 404, "not_found");
    if (rc == 2)
        return puddle_fail(w, 409, "not_pending");
    if (rc != 0)
        return puddle_fail(w, 500, "internal");
    puddle_device d;
    if (puddle_devices_get(c->ctx->devices, id, &d) != 0)
        return puddle_fail(w, 500, "internal");
    puddle_log(PUDDLE_LOG_INFO, "device '%s' may now use this Puddle", d.name);
    device_json(w, &d);
    return 200;
}

static int forget_device(const puddle_call *c, puddle_jw *w, int only_pending)
{
    char id[PUDDLE_DEVICE_ID_LEN + 1], err[300];
    if (device_ref(c, id) != 0)
        return puddle_fail(w, 400, "bad_request");
    int rc = puddle_devices_remove(c->ctx->devices, id, only_pending, err, sizeof err);
    if (rc == 1)
        return puddle_fail(w, 404, "not_found");
    if (rc == 2)
        return puddle_fail(w, 409, "not_pending");
    if (rc != 0)
        return puddle_fail(w, 500, "internal");
    puddle_log(PUDDLE_LOG_INFO, "device %.12s… %s", id, only_pending ? "declined" : "revoked");
    return 204;
}

static int decline_device(const puddle_call *c, puddle_jw *w) { return forget_device(c, w, 1); }
static int revoke_device(const puddle_call *c, puddle_jw *w) { return forget_device(c, w, 0); }

/* ---- the phone's side ---- */

typedef struct {
    unsigned char pub[32];
    char secret[PUDDLE_PAIR_SECRET_LEN + 8];
    char name[PUDDLE_DEVICE_NAME_MAX * 4];
} pair_req;

static int read_pair(const char *b, size_t len, pair_req *p)
{
    puddle_span v;
    char hex[80];
    memset(p, 0, sizeof *p);
    return puddle_json_valid_object(b, len) && puddle_json_member(b, len, "key", &v) == 0 && puddle_json_string(v, hex, sizeof hex) == 0 &&
                   strlen(hex) == 64 && puddle_hex_decode(hex, p->pub, 32) == 0 && puddle_json_member(b, len, "secret", &v) == 0 &&
                   puddle_json_string(v, p->secret, sizeof p->secret) == 0
               ? 0
               : -1;
}

/* PAIR: the key that signed is the one in the body, the device id is the one that key gives, and the secret is good.
 * Anything else is the usual 404: without the secret, nobody learns this is a Puddle, let alone whose. */
static int pair_key(puddle_ctx *ctx, const unsigned char *body, size_t len, const char *sender_id, const char *kid, unsigned char sign_pub[32])
{
    pair_req p;
    char id[PUDDLE_DEVICE_ID_LEN + 1], k[65];
    if (!ctx->devices || !sender_id || !kid || (ctx->anon_bucket && !puddle_bucket_take(ctx->anon_bucket, puddle_ctx_now_ms(ctx))) ||
        read_pair((const char *)body, len, &p) != 0)
        return 0;
    puddle_device_id(p.pub, id);
    puddle_key_id(p.pub, k);
    if (strcmp(id, sender_id) != 0 || strcmp(k, kid) != 0 || !puddle_devices_secret_ok(ctx->devices, p.secret, puddle_ctx_now_ms(ctx)))
        return 0;
    memcpy(sign_pub, p.pub, 32);
    return 1;
}

/* a name for the list: 1..64 bytes after trimming, no control characters */
static int good_name(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = '\0';
    size_t lead = strspn(s, " \t");
    memmove(s, s + lead, n - lead + 1);
    n -= lead;
    if (n == 0 || n > PUDDLE_DEVICE_NAME_MAX)
        return 0;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)s[i] < 0x20 || s[i] == 0x7f)
            return 0;
    return 1;
}

static int pair_post(const puddle_call *c, puddle_jw *w)
{
    pair_req p;
    char err[300];
    if (read_pair(body_text(c), body_size(c), &p) != 0 || member_str(c, "name", p.name, sizeof p.name) != 0 || !good_name(p.name))
        return puddle_fail(w, 400, "bad_request");
    puddle_device d;
    int rc = puddle_devices_pair(c->ctx->devices, p.secret, p.pub, p.name, puddle_call_now_ms(c), &d, err, sizeof err);
    if (rc == 1)
        return puddle_fail(w, 404, "not_found"); /* spent in the moment since it was checked */
    if (rc == 2 && d.state == PUDDLE_DEVICE_ACTIVE)
        return puddle_fail(w, 409, "already_paired");
    if (rc == 3)
        return puddle_fail(w, 409, "too_many_devices");
    if (rc < 0)
        return puddle_fail(w, 500, "internal");
    if (rc == 0)
        puddle_log(PUDDLE_LOG_INFO, "device '%s' asks to use this Puddle; accept it in the app", d.name);
    char me[PUDDLE_USER_ID_LEN + 1], check[7];
    size_t cl = 0;
    char *chain = puddle_identity_chain(c->ctx->identity, &cl);
    if (!chain)
        return puddle_fail(w, 500, "internal");
    puddle_identity_id(c->ctx->identity, me);
    puddle_device_check(d.pub, check);
    puddle_jw_object(w);
    puddle_jw_str(w, "device", d.id);
    puddle_jw_str(w, "check", check);
    puddle_jw_str(w, "state", "pending");
    puddle_jw_key(w, "puddle");
    puddle_jw_object(w);
    puddle_jw_str(w, "id", me);
    puddle_jw_key(w, "chain");
    puddle_jw_string_n(w, chain, cl);
    puddle_jw_end_object(w);
    puddle_jw_end_object(w);
    free(chain);
    return 202;
}

/* How it stands, and our key chain: after a roll the phone's receipts are signed by a key it doesn't know yet, and the chain is
 * how it learns the new one (it proves itself against the id the phone already holds). The phone's handshake. */
static int status_get(const puddle_call *c, puddle_jw *w)
{
    puddle_device d;
    if (puddle_devices_get(c->ctx->devices, c->sender, &d) != 0)
        return puddle_fail(w, 404, "not_found");
    size_t cl = 0;
    char *chain = puddle_identity_chain(c->ctx->identity, &cl);
    if (!chain)
        return puddle_fail(w, 500, "internal");
    puddle_jw_object(w);
    puddle_jw_str(w, "state", d.state == PUDDLE_DEVICE_ACTIVE ? "active" : "pending");
    puddle_jw_key(w, "chain");
    puddle_jw_string_n(w, chain, cl);
    puddle_jw_end_object(w);
    free(chain);
    return 200;
}

const puddle_route puddle_device_routes[] = {
    { "GET",  "/api/v1/devices",                    PUDDLE_AUTH_LOCAL,  0,    list_devices,   NULL },
    { "POST", "/api/v1/devices/pair",               PUDDLE_AUTH_LOCAL,  64,   start_pairing,  NULL },
    { "POST", "/api/v1/devices/{device:id}/accept",  PUDDLE_AUTH_LOCAL,  64,   accept_device,  NULL },
    { "POST", "/api/v1/devices/{device:id}/decline", PUDDLE_AUTH_LOCAL,  64,   decline_device, NULL },
    { "POST", "/api/v1/devices/{device:id}/revoke",  PUDDLE_AUTH_LOCAL,  64,   revoke_device,  NULL },
    { "POST", "/device/v1/pair",         PUDDLE_AUTH_PAIR,   4096, pair_post,      pair_key },
    { "GET",  "/device/v1/status",       PUDDLE_AUTH_DEVICE, 0,    status_get,     NULL },
};
const size_t puddle_device_routes_count = sizeof puddle_device_routes / sizeof puddle_device_routes[0];
