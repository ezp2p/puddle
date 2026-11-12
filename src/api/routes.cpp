/* The Puddle's routes. The owner API (api/openapi.yaml) and the stream API (api/federation.yaml) are built
 * up here, one handler at a time. */
#include "http/router.h"

#include "api/api.h"

#include "core/phrase.h"

#include <mutex>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ROLL_TTL_MS (10 * 60 * 1000)

#define now_of puddle_call_now_ms

/* ---- the pending roll ---- */

struct puddle_roll_slot {
    std::mutex mu;
    puddle_roll *roll;
    int64_t expires_ms;
};

puddle_roll_slot *puddle_roll_slot_new(void)
{
    return new (std::nothrow) puddle_roll_slot();
}

void puddle_roll_slot_free(puddle_roll_slot *s)
{
    if (!s)
        return;
    if (s->roll)
        puddle_roll_abandon(s->roll);
    delete s;
}

/* ---- shared pieces ---- */

static void link_member(puddle_jw *w, const puddle_ctx *ctx)
{
    char link[256];
    puddle_jw_key(w, "link");
    if (ctx->config && puddle_make_link(ctx->config, NULL, link, sizeof link))
        puddle_jw_string(w, link);
    else
        puddle_jw_null(w);
}


/* the Identity schema */
static void identity_json(puddle_jw *w, const puddle_ctx *ctx)
{
    char id[PUDDLE_USER_ID_LEN + 1], kid[65];
    unsigned char pub[32];
    uint64_t seq;
    puddle_identity_id(ctx->identity, id);
    puddle_identity_public(ctx->identity, pub, NULL, &seq);
    puddle_key_id(pub, kid);
    puddle_jw_object(w);
    puddle_jw_str(w, "id", id);
    link_member(w, ctx);
    puddle_jw_num(w, "seq", (int64_t)seq);
    puddle_jw_str(w, "key_id", kid);
    puddle_jw_end_object(w);
}

/* ---- handlers ---- */

static int health(const puddle_call *c, puddle_jw *w)
{
    (void)c;
    puddle_jw_object(w);
    puddle_jw_str(w, "status", "ok");
    puddle_jw_end_object(w);
    return 200;
}

static int get_identity(const puddle_call *c, puddle_jw *w)
{
    identity_json(w, c->ctx);
    return 200;
}

/* Step one of changing keys: check the CURRENT phrase, hand back a NEW one. Nothing changes until confirm. */
static int roll_start(const puddle_call *c, puddle_jw *w)
{
    puddle_span v;
    char phrase[128], fresh[PUDDLE_PHRASE_LEN + 1], err[256];
    if (puddle_json_member((const char *)c->body, c->body_len, "phrase", &v) != 0 ||
        puddle_json_string(v, phrase, sizeof phrase) != 0)
        return puddle_fail(w, 400, "bad_request");

    puddle_roll_slot *slot = c->ctx->roll_slot;
    slot->mu.lock();
    if (slot->roll) {
        puddle_roll_abandon(slot->roll); /* starting again replaces an unfinished roll */
        slot->roll = NULL;
    }
    slot->roll = puddle_identity_roll_prepare(c->ctx->identity, phrase, fresh, err, sizeof err);
    memset(phrase, 0, sizeof phrase);
    if (!slot->roll) {
        slot->mu.unlock();
        puddle_jw_object(w);
        puddle_jw_str(w, "error", "bad_phrase");
        puddle_jw_str(w, "message", err);
        puddle_jw_end_object(w);
        return 400;
    }
    slot->expires_ms = now_of(c) + ROLL_TTL_MS;
    slot->mu.unlock();

    puddle_jw_object(w);
    puddle_jw_str(w, "new_phrase", fresh);
    puddle_jw_num(w, "expires_in", ROLL_TTL_MS / 1000);
    puddle_jw_end_object(w);
    memset(fresh, 0, sizeof fresh);
    return 200;
}

/* Step two: the owner has written the new phrase down. */
static int roll_confirm(const puddle_call *c, puddle_jw *w)
{
    char err[256];
    puddle_roll_slot *slot = c->ctx->roll_slot;
    slot->mu.lock();
    puddle_roll *roll = slot->roll;
    slot->roll = NULL;
    if (!roll || now_of(c) > slot->expires_ms) {
        if (roll)
            puddle_roll_abandon(roll); /* too old: the phrase was shown long ago, so ask again */
        slot->mu.unlock();
        return puddle_fail(w, 409, "no_roll_pending");
    }
    int rc = puddle_identity_roll_commit(c->ctx->identity, roll, err, sizeof err);
    if (rc != 0)
        puddle_roll_abandon(roll);
    slot->mu.unlock();
    if (rc != 0) {
        puddle_jw_object(w);
        puddle_jw_str(w, "error", "roll_failed");
        puddle_jw_str(w, "message", err);
        puddle_jw_end_object(w);
        return 500;
    }
    if (c->ctx->streams && c->ctx->outbox && c->ctx->courier)
        c->ctx->courier->keys_rolled(c->ctx); /* tell the people who hold our keys, ahead of anything else waiting for them */
    identity_json(w, c->ctx);
    return 200;
}

static int roll_cancel(const puddle_call *c, puddle_jw *w)
{
    puddle_roll_slot *slot = c->ctx->roll_slot;
    slot->mu.lock();
    int had = slot->roll != NULL;
    if (had)
        puddle_roll_abandon(slot->roll);
    slot->roll = NULL;
    slot->mu.unlock();
    puddle_jw_object(w);
    puddle_jw_flag(w, "cancelled", had);
    puddle_jw_end_object(w);
    return 200;
}

const puddle_route puddle_routes[] = {
    { "GET",  "/health",                   PUDDLE_AUTH_OWNER, 0,   health,       NULL },
    { "GET",  "/api/v1/identity",          PUDDLE_AUTH_OWNER, 0,   get_identity, NULL },
    { "POST", "/api/v1/identity/roll",     PUDDLE_AUTH_OWNER, 512, roll_start,   NULL },
    { "POST", "/api/v1/identity/roll/confirm", PUDDLE_AUTH_OWNER, 64, roll_confirm, NULL },
    { "POST", "/api/v1/identity/roll/cancel",  PUDDLE_AUTH_OWNER, 64, roll_cancel,  NULL },
};
const size_t puddle_routes_count = sizeof puddle_routes / sizeof puddle_routes[0];

