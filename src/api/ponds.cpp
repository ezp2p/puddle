#include "api/api.h"
#include "api/request.hpp"
#include "core/link.h"
#include "core/ponds.h"
#include "core/profile.h"

#include <cstdlib>
#include <cstring>
#include <string>

/* The Ponds the owner is in (docs/ponds.md): joining with a pond:// link, the channels and members each one has, reading and
 * sending messages, and leaving. What is shown comes from what the Puddle keeps of each Pond; only scrolling back past that, and
 * what the owner does there, reaches the Pond itself (through the courier). */

using namespace puddle;
using namespace puddle::api;

static puddle_ponds *pondsOf(const puddle_call *c)
{
    if (!c->ctx->ponds)
        throw Error::notFound();
    return c->ctx->ponds;
}

/* A Pond's courier code as the owner is told it. */
static Error failure(const char *code)
{
    if (!strcmp(code, "no_pond") || !strcmp(code, "no_channel") || !strcmp(code, "no_profile"))
        return Error(404, code);
    if (!strcmp(code, "bad_text") || !strcmp(code, "bad_query"))
        return Error(400, code);
    if (!strcmp(code, "slow_down"))
        return Error(429, code);
    if (!strcmp(code, "invite_refused") || !strcmp(code, "removed"))
        return Error(409, code);
    return Error(502, code); /* unreachable, not_a_pond, certificate_changed, failed */
}

/* Answer with JSON that `fn` writes, however big: a Pond can have many members, and a page of messages up to 4000 characters
 * each. It goes out as a body of its own rather than in the router's buffer. */
template <typename F> static int big_json(const puddle_call *c, int status, F fn)
{
    for (size_t cap = 64 * 1024; cap <= (64u << 20); cap *= 4) {
        char *buf = (char *)malloc(cap);
        if (!buf)
            throw Error::internal();
        puddle_jw w;
        puddle_jw_init(&w, buf, cap);
        int r = fn(&w);
        size_t n = puddle_jw_finish(&w);
        if (r == 0 && n) {
            c->blob->data = (unsigned char *)buf;
            c->blob->len = n;
            snprintf(c->blob->mime, sizeof c->blob->mime, "application/json");
            return status;
        }
        free(buf);
        if (r != 0)
            throw r > 0 ? Error::notFound("no_pond") : Error::internal();
    }
    throw Error::internal("too big");
}

static int list_ponds(const puddle_call *c, puddle_jw *)
{
    puddle_ponds *p = pondsOf(c);
    return big_json(c, 200, [&](puddle_jw *w) {
        puddle_ponds_write_list(p, w);
        return 0;
    });
}

static int get_pond(const puddle_call *c, puddle_jw *)
{
    puddle_ponds *p = pondsOf(c);
    return big_json(c, 200, [&](puddle_jw *w) { return puddle_ponds_write_one(p, puddle_param(c, "pond"), w); });
}

/* {"link": "pond://...", "profile": id or null}: the profile is who the members will see the owner as. */
static int join_pond(const puddle_call *c, puddle_jw *)
{
    Body b(c);
    puddle_ponds *p = pondsOf(c);
    std::string text = b.string("link", 512);
    std::optional<std::string> profile;
    if (b.has("profile") && !b.isNull("profile"))
        profile = b.string("profile", PUDDLE_PROFILE_ID_MAX + 4);
    puddle_link link;
    char err[160];
    if (puddle_pond_link_parse(text.c_str(), &link, err, sizeof err) != 0)
        throw Error::badRequest("bad_link", err);
    char id[PUDDLE_USER_ID_LEN + 1];
    const char *code = "internal";
    if (!c->ctx->courier || !c->ctx->courier->pond_join ||
        c->ctx->courier->pond_join(c->ctx, &link, profile ? profile->c_str() : nullptr, id, &code) != 0)
        throw failure(code);
    return big_json(c, 201, [&](puddle_jw *w) { return puddle_ponds_write_one(p, id, w); });
}

static int64_t channelOf(const puddle_call *c)
{
    int64_t ch = strtoll(puddle_param(c, "channel"), nullptr, 10);
    if (!puddle_ponds_has_channel(pondsOf(c), puddle_param(c, "pond"), ch))
        throw Error::notFound("no_channel");
    return ch;
}

/* ?before=<message id>&limit=: oldest first, `limit` (50, at most 100) before `before` (the newest when left out). */
static int channel_messages(const puddle_call *c, puddle_jw *)
{
    static const char *const KEYS[] = {"before", "limit"};
    char q[2][PUDDLE_QUERY_VALUE_MAX];
    if (puddle_query_parse(c->req->target, KEYS, 2, q) != 0)
        throw Error::badRequest("bad_query");
    char *end;
    int64_t before = *q[0] ? strtoll(q[0], &end, 10) : 0, limit = *q[1] ? strtoll(q[1], &end, 10) : 50;
    if (before < 0 || limit < 1 || limit > 100)
        throw Error::badRequest("bad_query");
    puddle_ponds *p = pondsOf(c);
    const char *pond = puddle_param(c, "pond");
    int64_t ch = channelOf(c);
    return big_json(c, 200, [&](puddle_jw *w) {
        int r = puddle_ponds_write_messages(p, pond, ch, before, (int)limit, w);
        if (r <= 0)
            return r;
        const char *code = "internal"; /* further back than is kept: ask the Pond */
        puddle_jw_init(w, w->buf, w->cap);
        if (!c->ctx->courier || !c->ctx->courier->pond_history ||
            c->ctx->courier->pond_history(c->ctx, pond, ch, before, (int)limit, w, &code) != 0)
            throw failure(code);
        return 0;
    });
}

static int send_message(const puddle_call *c, puddle_jw *w)
{
    Body b(c);
    std::string text = b.messageBody("body");
    int64_t ch = channelOf(c);
    const char *code = "internal";
    if (!c->ctx->courier || !c->ctx->courier->pond_send ||
        c->ctx->courier->pond_send(c->ctx, puddle_param(c, "pond"), ch, text.c_str(), &code) != 0)
        throw failure(code);
    puddle_jw_object(w);
    puddle_jw_flag(w, "sent", 1);
    puddle_jw_end_object(w);
    return 200;
}

static int leave_pond(const puddle_call *c, puddle_jw *)
{
    const char *code = "internal";
    if (!c->ctx->courier || !c->ctx->courier->pond_leave || c->ctx->courier->pond_leave(c->ctx, puddle_param(c, "pond"), &code) != 0)
        throw failure(code);
    return 204;
}

/* Choose which of the owner's profiles the Pond's members see. `profile: null`: none (they see only the id). */
static int pond_profile(const puddle_call *c, puddle_jw *w)
{
    Body b(c);
    if (!b.has("profile"))
        throw Error::badRequest();
    const char *pond = puddle_param(c, "pond");
    if (puddle_ponds_conn(pondsOf(c), pond, nullptr) != 0)
        throw Error::notFound("no_pond");
    std::optional<std::string> pid;
    if (!b.isNull("profile")) {
        pid = b.string("profile", PUDDLE_PROFILE_ID_MAX + 4);
        if (!c->ctx->profiles || !puddle_profiles_has(c->ctx->profiles, pid->c_str()))
            throw Error::notFound("no_profile");
    }
    char place[PUDDLE_PLACE_MAX], err[160];
    if (!c->ctx->assignments || puddle_place_pond(pond, place) != 0 ||
        puddle_assignments_set(c->ctx->assignments, place, pid ? pid->c_str() : nullptr, err, sizeof err) != 0)
        throw Error::internal(err);
    if (c->ctx->courier && c->ctx->courier->profile_push)
        c->ctx->courier->profile_push(c->ctx, place);
    puddle_jw_object(w);
    puddle_jw_key(w, "profile");
    if (pid)
        puddle_jw_string(w, pid->c_str());
    else
        puddle_jw_null(w);
    puddle_jw_end_object(w);
    return 200;
}

#define POND "/api/v1/ponds/{pond:id}"

const puddle_route puddle_pond_api_routes[] = {
    { "GET",  "/api/v1/ponds",                                    PUDDLE_AUTH_OWNER, 0,     list_ponds,       NULL },
    { "POST", "/api/v1/ponds/join",                               PUDDLE_AUTH_OWNER, 1024,  join_pond,        NULL },
    { "GET",  POND,                                               PUDDLE_AUTH_OWNER, 0,     get_pond,         NULL },
    { "GET",  POND "/channels/{channel:num}/messages",            PUDDLE_AUTH_OWNER, 0,     channel_messages, NULL },
    { "POST", POND "/channels/{channel:num}/messages/send",       PUDDLE_AUTH_OWNER, 20000, send_message,     NULL },
    { "POST", POND "/profile",                                    PUDDLE_AUTH_OWNER, 128,   pond_profile,     NULL },
    { "POST", POND "/leave",                                      PUDDLE_AUTH_OWNER, 64,    leave_pond,       NULL },
};
const size_t puddle_pond_api_routes_count = sizeof puddle_pond_api_routes / sizeof puddle_pond_api_routes[0];
