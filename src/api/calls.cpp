/* Owner API: calls with friends. /api/v1/calls/... (docs/calls.md, "The owner's side"). The calls themselves are carried by
 * federation/calls.cpp, reached through the courier. */
#include "api/api.h"
#include "api/request.hpp"

#include <cstdlib>
#include <cstring>
#include <string>

using namespace puddle;
using namespace puddle::api;

#define SDP_MAX (16 * 1024) /* a description, as federation/calls.h's PUDDLE_CALL_SDP_MAX (the API doesn't include federation) */

static const puddle_courier &courier(const puddle_call *c)
{
    if (!c->ctx->courier || !c->ctx->courier->call_start || !c->ctx->courier->calls_write)
        throw Error::conflict("no_calls");
    return *c->ctx->courier;
}

static Error failure(const char *code)
{
    if (!strcmp(code, "no_call") || !strcmp(code, "no_group"))
        return Error::notFound(code);
    if (!strcmp(code, "no_stream") || !strcmp(code, "busy") || !strcmp(code, "no_calls") || !strcmp(code, "full"))
        return Error::conflict(code);
    if (!strcmp(code, "bad_sdp"))
        return Error::badRequest("bad_sdp");
    return Error::internal();
}

/* The call `id` as it stands now. */
static int one(const puddle_call *c, puddle_jw *w, const char *id, int status = 200)
{
    if (courier(c).calls_write(c->ctx, id, nullptr, nullptr, 0, w) != 0)
        throw Error::notFound("no_call");
    return status;
}

static const char *call_param(const puddle_call *c)
{
    const char *id = puddle_param(c, "call");
    if (strlen(id) != 26)
        throw Error::notFound("no_call");
    return id;
}

/* ?user=<id>: only the calls with that friend (a chat shows them); ?group=<id>: only that group's. ?limit=: how many ended
 * ones, 50 unless said, up to 200. */
static int list_calls(const puddle_call *c, puddle_jw *w)
{
    const char *keys[3] = { "limit", "user", "group" };
    char v[3][PUDDLE_QUERY_VALUE_MAX];
    if (puddle_query_parse(c->req->target, keys, 3, v) != 0)
        throw Error::badRequest();
    long limit = 50;
    if (v[0][0]) {
        char *end;
        limit = strtol(v[0], &end, 10);
        if (*end || limit < 1 || limit > 200)
            throw Error::badRequest();
    }
    if ((v[1][0] && !puddle_id_valid(v[1])) || (v[2][0] && !puddle_id_valid(v[2])))
        throw Error::badRequest("bad_query");
    courier(c).calls_write(c->ctx, nullptr, v[1][0] ? v[1] : nullptr, v[2][0] ? v[2] : nullptr, (int)limit, w);
    return 200;
}

static int get_call(const puddle_call *c, puddle_jw *w) { return one(c, w, call_param(c)); }

/* {"user": id}: ring a friend. The call comes back as it stands: `calling`, or `ended` with why (unreachable, busy, ...).
 * {"group": id}: start a call in a group, or join the one going on there (docs/calls.md, "Group calls").
 * From a phone, the phone has the call, and sets up its leg next (media). */
static int start(const puddle_call *c, puddle_jw *w)
{
    Body b(c);
    auto user = b.optString("user", 64), group = b.optString("group", 64);
    if (user.has_value() == group.has_value() || !puddle_id_valid((user ? *user : *group).c_str()))
        throw Error::badRequest();
    char id[27];
    const char *code = "internal";
    if (group) {
        if (!courier(c).call_start_group)
            throw Error::conflict("no_calls");
        if (courier(c).call_start_group(c->ctx, group->c_str(), c->device, id, &code) != 0)
            throw failure(code);
    } else if (courier(c).call_start(c->ctx, user->c_str(), c->device, id, &code) != 0) {
        throw failure(code);
    }
    return one(c, w, id, 201);
}

static int accept(const puddle_call *c, puddle_jw *w)
{
    const char *id = call_param(c);
    const char *code = "internal";
    if (courier(c).call_accept(c->ctx, id, c->device, &code) != 0)
        throw failure(code);
    return one(c, w, id);
}

/* {"sdp": offer}: the phone that has the call sets up its own WebRTC leg to this Puddle (docs/calls.md, "On a phone"),
 * and gets {"sdp": answer, "udp": port}. Only from a phone: the window's sound goes through this process. */
static int media(const puddle_call *c, puddle_jw *w)
{
    const char *id = call_param(c);
    if (!c->device)
        throw Error::badRequest("not_a_phone");
    if (!courier(c).call_media)
        throw Error::conflict("no_calls");
    Body b(c);
    std::string offer = b.string("sdp", SDP_MAX);
    char *answer = nullptr;
    int udp = 0;
    const char *code = "internal";
    if (courier(c).call_media(c->ctx, id, c->device, offer.c_str(), &answer, &udp, &code) != 0)
        throw failure(code);
    std::string sdp(answer);
    free(answer);
    puddle_jw_object(w);
    puddle_jw_str(w, "sdp", sdp.c_str());
    puddle_jw_num(w, "udp", udp);
    puddle_jw_end_object(w);
    return 200;
}

template <int (*puddle_courier::*Fn)(puddle_ctx *, const char *, const char **)>
static int act(const puddle_call *c, puddle_jw *w)
{
    const char *id = call_param(c);
    const char *code = "internal";
    if ((courier(c).*Fn)(c->ctx, id, &code) != 0)
        throw failure(code);
    return one(c, w, id);
}

const puddle_route puddle_call_api_routes[] = {
    { "GET",  "/api/v1/calls",                       PUDDLE_AUTH_OWNER, 0,   list_calls,                          NULL },
    { "POST", "/api/v1/calls/start",                 PUDDLE_AUTH_OWNER, 256, start,                               NULL },
    { "GET",  "/api/v1/calls/{call:token}",          PUDDLE_AUTH_OWNER, 0,   get_call,                            NULL },
    { "POST", "/api/v1/calls/{call:token}/accept",   PUDDLE_AUTH_OWNER, 64,   accept,                              NULL },
    { "POST", "/api/v1/calls/{call:token}/media",    PUDDLE_AUTH_OWNER, SDP_MAX + 1024, media,    NULL },
    { "POST", "/api/v1/calls/{call:token}/decline",  PUDDLE_AUTH_OWNER, 64,   act<&puddle_courier::call_decline>,  NULL },
    { "POST", "/api/v1/calls/{call:token}/end",      PUDDLE_AUTH_OWNER, 64,   act<&puddle_courier::call_end>,      NULL },
};
const size_t puddle_call_api_routes_count = sizeof puddle_call_api_routes / sizeof puddle_call_api_routes[0];
