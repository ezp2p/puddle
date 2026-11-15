/* Owner API: calls with friends. /api/v1/calls/... (docs/calls.md, "The owner's side"). The calls themselves are carried by
 * federation/calls.cpp, reached through the courier. */
#include "api/api.h"
#include "api/request.hpp"

#include <cstring>

using namespace puddle;
using namespace puddle::api;

static const puddle_courier &courier(const puddle_call *c)
{
    if (!c->ctx->courier || !c->ctx->courier->call_start || !c->ctx->courier->calls_write)
        throw Error::conflict("no_calls");
    return *c->ctx->courier;
}

static Error failure(const char *code)
{
    if (!strcmp(code, "no_call"))
        return Error::notFound("no_call");
    if (!strcmp(code, "no_stream") || !strcmp(code, "busy") || !strcmp(code, "no_calls"))
        return Error::conflict(code);
    return Error::internal();
}

/* The call `id` as it stands now. */
static int one(const puddle_call *c, puddle_jw *w, const char *id, int status = 200)
{
    if (courier(c).calls_write(c->ctx, id, w) != 0)
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

static int list_calls(const puddle_call *c, puddle_jw *w)
{
    courier(c).calls_write(c->ctx, nullptr, w);
    return 200;
}

static int get_call(const puddle_call *c, puddle_jw *w) { return one(c, w, call_param(c)); }

/* {"user": id}: ring a friend. The call comes back as it stands: `calling`, or `ended` with why (unreachable, busy, ...). */
static int start(const puddle_call *c, puddle_jw *w)
{
    Body b(c);
    std::string user = b.string("user", 64);
    if (!puddle_id_valid(user.c_str()))
        throw Error::badRequest();
    char id[27];
    const char *code = "internal";
    if (courier(c).call_start(c->ctx, user.c_str(), id, &code) != 0)
        throw failure(code);
    return one(c, w, id, 201);
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
    { "POST", "/api/v1/calls/{call:token}/accept",   PUDDLE_AUTH_OWNER, 0,   act<&puddle_courier::call_accept>,   NULL },
    { "POST", "/api/v1/calls/{call:token}/decline",  PUDDLE_AUTH_OWNER, 0,   act<&puddle_courier::call_decline>,  NULL },
    { "POST", "/api/v1/calls/{call:token}/end",      PUDDLE_AUTH_OWNER, 0,   act<&puddle_courier::call_end>,      NULL },
};
const size_t puddle_call_api_routes_count = sizeof puddle_call_api_routes / sizeof puddle_call_api_routes[0];
