/* Owner API: streams, invite links, and who may ask. /api/v1/streams/... and /api/v1/invites/... (docs/objects.md).
 * Talking to the other Puddle about any of this is the outbox's job: these only change what we hold and record what we
 * still owe them. */
#include "api/api.h"
#include "api/request.hpp"
#include "objects/stream.hpp"

#include <cstdio>
#include <cstring>

using namespace puddle;
using namespace puddle::api;

static Stream streamOf(const puddle_call *c)
{
    auto s = Stream::get(c->ctx, puddle_param(c, "user"));
    if (!s)
        throw Error::notFound();
    return std::move(*s);
}

static int list_streams(const puddle_call *c, puddle_jw *w)
{
    std::string stateName;
    Page p = paging(c, 200, "state", &stateName);
    std::optional<puddle_stream_state> state;
    if (!stateName.empty()) {
        puddle_stream_state s;
        if (puddle_stream_state_parse(stateName.c_str(), &s) != 0)
            throw Error::badRequest();
        state = s;
    }
    auto all = Stream::all(c->ctx, state);
    beginPage(w);
    size_t i = p.offset;
    for (size_t shown = 0; i < all.size() && shown < p.limit && w->len + 12500 <= w->cap; i++, shown++)
        all[i].write(w);
    endPage(w, i < all.size() ? std::optional<size_t>(i) : std::nullopt);
    return 200;
}

static int request_stream(const puddle_call *c, puddle_jw *w)
{
    Body b(c);
    std::string target = b.string("target", 399);
    std::string note = b.optString("message", PUDDLE_NOTE_MAX_BYTES).value_or("");
    std::string profile = b.optString("profile", PUDDLE_PROFILE_ID_MAX).value_or("");
    if (b.has("profile") && profile.empty())
        throw Error::badRequest();
    Stream::request(c->ctx, target, note, profile).write(w);
    return 201;
}

static int get_stream(const puddle_call *c, puddle_jw *w)
{
    streamOf(c).write(w);
    return 200;
}

/* Say yes. {"profile": id}, if given, is who they will see the owner as; their yes carries it. */
static int accept_stream(const puddle_call *c, puddle_jw *w)
{
    Body b(c);
    Stream s = streamOf(c);
    if (b.has("profile") && !b.isNull("profile"))
        s = s.showProfile(b.string("profile", PUDDLE_PROFILE_ID_MAX + 4));
    s.accept().write(w);
    return 200;
}

static int decline_stream(const puddle_call *c, puddle_jw *w)
{
    streamOf(c).decline().write(w);
    return 200;
}

static int delete_stream(const puddle_call *c, puddle_jw *w)
{
    streamOf(c).remove().write(w);
    return 200;
}

static int trust_stream(const puddle_call *c, puddle_jw *w)
{
    streamOf(c).trust().write(w);
    return 200;
}

/* Choose which of the owner's profiles this person sees directly. `profile: null`: none (they see only the id). */
static int stream_profile(const puddle_call *c, puddle_jw *w)
{
    Body b(c);
    if (!b.has("profile"))
        throw Error::badRequest();
    std::optional<std::string> pid;
    if (!b.isNull("profile"))
        pid = b.string("profile", PUDDLE_PROFILE_ID_MAX + 4);
    streamOf(c).showProfile(pid).write(w);
    return 200;
}

/* ---- who may ask ---- */

static int get_policy(const puddle_call *c, puddle_jw *w)
{
    puddle_jw_object(w);
    puddle_jw_str(w, "access", puddle_access_name(puddle_streams_access(c->ctx->streams)));
    puddle_jw_end_object(w);
    return 200;
}

static int set_policy(const puddle_call *c, puddle_jw *w)
{
    Body b(c);
    puddle_access a;
    char err[300];
    if (puddle_access_parse(b.string("access", 31).c_str(), &a) != 0)
        throw Error::badRequest();
    if (puddle_streams_set_access(c->ctx->streams, a, err, sizeof err) != 0)
        throw Error::internal(err);
    return get_policy(c, w);
}

/* ---- invite links ---- */

static void invite_json(puddle_jw *w, const puddle_ctx *ctx, const puddle_invite *inv)
{
    char link[300];
    puddle_jw_object(w);
    puddle_jw_key(w, "link");
    if (puddle_make_link(ctx->config, inv->token, link, sizeof link))
        puddle_jw_string(w, link);
    else
        puddle_jw_null(w);
    puddle_jw_str(w, "token", inv->token);
    puddle_jw_num(w, "expires_at", inv->expires_ms);
    puddle_jw_num(w, "uses", inv->uses);
    puddle_jw_end_object(w);
}

static int list_invites(const puddle_call *c, puddle_jw *w)
{
    puddle_invite all[PUDDLE_MAX_INVITES];
    size_t n = puddle_streams_invites(c->ctx->streams, puddle_call_now_ms(c), all, PUDDLE_MAX_INVITES);
    beginPage(w);
    for (size_t i = 0; i < n && i < PUDDLE_MAX_INVITES; i++)
        invite_json(w, c->ctx, &all[i]);
    endPage(w, std::nullopt);
    return 200;
}

static int create_invite(const puddle_call *c, puddle_jw *w)
{
    Body b(c);
    int64_t uses = b.optInt("uses").value_or(1), expiresIn = b.optInt("expires_in").value_or(7 * 24 * 3600);
    if (uses < 1 || uses > 1000 || expiresIn < 60 || expiresIn > 30 * 24 * 3600)
        throw Error::badRequest();
    char link[300], err[300];
    if (!puddle_make_link(c->ctx->config, "abcdefghijklmnopqrstuvwxy2", link, sizeof link))
        throw Error::conflict("no_address", "this Puddle doesn't know its public address; set `advertise: host:port` in puddle.yaml");
    puddle_invite inv;
    if (puddle_streams_invite_create(c->ctx->streams, puddle_call_now_ms(c), expiresIn * 1000, (int)uses, &inv, err, sizeof err) != 0)
        throw Error::conflict("cannot_create", err);
    invite_json(w, c->ctx, &inv);
    return 201;
}

static int revoke_invite(const puddle_call *c, puddle_jw *w)
{
    const char *token = puddle_param(c, "token");
    puddle_invite all[PUDDLE_MAX_INVITES], gone;
    size_t n = puddle_streams_invites(c->ctx->streams, puddle_call_now_ms(c), all, PUDDLE_MAX_INVITES);
    bool found = false;
    for (size_t i = 0; i < n && i < PUDDLE_MAX_INVITES && !found; i++)
        if (!strcmp(all[i].token, token)) {
            gone = all[i];
            found = true;
        }
    int rc = puddle_streams_invite_revoke(c->ctx->streams, token);
    if (rc == 1)
        throw Error::notFound();
    if (rc != 0)
        throw Error::internal();
    if (!found) { /* revoked, though it was not listed (expired at this instant): say so with what we know */
        memset(&gone, 0, sizeof gone);
        snprintf(gone.token, sizeof gone.token, "%s", token);
    }
    gone.uses = 0;
    invite_json(w, c->ctx, &gone);
    return 200;
}

const puddle_route puddle_stream_api_routes[] = {
    { "GET",  "/api/v1/streams",                    PUDDLE_AUTH_OWNER, 0,    list_streams,   NULL },
    { "POST", "/api/v1/streams/request",            PUDDLE_AUTH_OWNER, 2048, request_stream, NULL },
    { "GET",  "/api/v1/streams/policy",             PUDDLE_AUTH_OWNER, 0,    get_policy,     NULL },
    { "POST", "/api/v1/streams/policy/set",         PUDDLE_AUTH_OWNER, 256,  set_policy,     NULL },
    { "GET",  "/api/v1/streams/{user:id}",          PUDDLE_AUTH_OWNER, 0,    get_stream,     NULL },
    { "POST", "/api/v1/streams/{user:id}/accept",   PUDDLE_AUTH_OWNER, 128,  accept_stream,  NULL },
    { "POST", "/api/v1/streams/{user:id}/decline",  PUDDLE_AUTH_OWNER, 64,   decline_stream, NULL },
    { "POST", "/api/v1/streams/{user:id}/delete",   PUDDLE_AUTH_OWNER, 64,   delete_stream,  NULL },
    { "POST", "/api/v1/streams/{user:id}/trust",    PUDDLE_AUTH_OWNER, 64,   trust_stream,   NULL },
    { "POST", "/api/v1/streams/{user:id}/profile",  PUDDLE_AUTH_OWNER, 256,  stream_profile, NULL },
    { "GET",  "/api/v1/invites",                    PUDDLE_AUTH_OWNER, 0,    list_invites,   NULL },
    { "POST", "/api/v1/invites/create",             PUDDLE_AUTH_OWNER, 256,  create_invite,  NULL },
    { "POST", "/api/v1/invites/{token:token}/revoke", PUDDLE_AUTH_OWNER, 64, revoke_invite,  NULL },
};
const size_t puddle_stream_api_routes_count = sizeof puddle_stream_api_routes / sizeof puddle_stream_api_routes[0];
