/* The owner's say over this Puddle's Levee (docs/levee.md; api/openapi.yaml, "Levee"):
 *
 *   GET  /api/v1/levee        whether we are behind one, where, and whether we are attached now
 *   POST /api/v1/levee/join   {link}: join the Levee a levee:// link names; our address becomes its
 *   POST /api/v1/levee/leave  leave it; our address is our own again (or none) */

#include "api/api.h"

#include "base/json.h"
#include "core/levee.h"

#include <stdio.h>
#include <string.h>

static int levee_get(const puddle_call *c, puddle_jw *w)
{
    puddle_levee_state lv;
    int joined = puddle_levee_current(&lv);
    puddle_jw_object(w);
    puddle_jw_flag(w, "joined", joined);
    if (joined) {
        puddle_jw_str(w, "address", lv.address.address);
        puddle_jw_str(w, "id", lv.id);
        puddle_jw_num(w, "since", lv.joined_ms);
        puddle_jw_flag(w, "attached", c->ctx->levee_attached ? c->ctx->levee_attached(c->ctx) : 0);
        puddle_jw_flag(w, "calls", lv.turn_port != 0);
    }
    puddle_jw_end_object(w);
    return 200;
}

static int levee_join(const puddle_call *c, puddle_jw *w)
{
    puddle_span v;
    char link[600], err[300];
    if (!puddle_json_valid_object((const char *)c->body, c->body_len) ||
        puddle_json_member((const char *)c->body, c->body_len, "link", &v) != 0 || puddle_json_string(v, link, sizeof link) != 0)
        return puddle_fail(w, 400, "bad_request");
    if (!c->ctx->levee_join)
        return puddle_fail(w, 409, "no_levee");
    if (c->ctx->levee_join(c->ctx, link, err, sizeof err) != 0) {
        puddle_jw_object(w);
        puddle_jw_str(w, "error", strstr(err, "levee://") ? "bad_link" : "refused");
        puddle_jw_str(w, "message", err);
        puddle_jw_end_object(w);
        return 409;
    }
    return levee_get(c, w);
}

static int levee_leave(const puddle_call *c, puddle_jw *w)
{
    char err[300];
    if (!c->ctx->levee_leave || c->ctx->levee_leave(c->ctx, err, sizeof err) != 0)
        return puddle_fail(w, 409, "refused");
    return 204;
}

const puddle_route puddle_levee_routes[] = {
    { "GET",  "/api/v1/levee",       PUDDLE_AUTH_OWNER, 0,    levee_get,   NULL },
    { "POST", "/api/v1/levee/join",  PUDDLE_AUTH_OWNER, 2048, levee_join,  NULL },
    { "POST", "/api/v1/levee/leave", PUDDLE_AUTH_OWNER, 64,   levee_leave, NULL },
};
const size_t puddle_levee_routes_count = sizeof puddle_levee_routes / sizeof puddle_levee_routes[0];
