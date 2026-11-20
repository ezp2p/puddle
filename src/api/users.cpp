/* Owner API: people. /api/v1/users/... and /api/v1/blocks (docs/objects.md). Each handler gets a User, asks it to do
 * something, and answers with it. */
#include "api/api.h"
#include "api/request.hpp"
#include "objects/user.hpp"
#include "core/pond_dms.hpp"

#include <vector>

using namespace puddle;
using namespace puddle::api;

static User userOf(const puddle_call *c)
{
    auto u = User::get(c->ctx, puddle_param(c, "user"));
    if (!u)
        throw Error::notFound();
    return *u;
}

static int get_me(const puddle_call *c, puddle_jw *w)
{
    User::me(c->ctx).writeSelf(w);
    return 200;
}

static int get_user(const puddle_call *c, puddle_jw *w)
{
    userOf(c).write(w);
    return 200;
}

/* Every profile they show us, one for each place we share: directly ("dm") and in groups ("group:<id>"). */
static int user_profiles(const puddle_call *c, puddle_jw *w)
{
    User u = userOf(c);
    beginPage(w);
    for (const Profile &p : Profile::allOf(c->ctx, u.id()))
        p.write(w);
    endPage(w, std::nullopt);
    return 200;
}

static int block_user(const puddle_call *c, puddle_jw *w)
{
    // anyone can be blocked, including someone we have never heard of (a link someone warned us about)
    std::string id = puddle_param(c, "user");
    auto u = User::get(c->ctx, id);
    if (!u) {
        char err[300];
        if (puddle_streams_block(c->ctx->streams, id.c_str(), puddle_call_now_ms(c), err, sizeof err) != 0)
            throw Error::internal(err);
        u = User::get(c->ctx, id);
        if (!u)
            throw Error::internal();
        u->write(w);
        return 200;
    }
    u->block().write(w);
    return 200;
}

static int unblock_user(const puddle_call *c, puddle_jw *w)
{
    userOf(c).unblock().write(w);
    return 200;
}

static int list_blocks(const puddle_call *c, puddle_jw *w)
{
    Page p = paging(c, 200);
    size_t n = puddle_streams_blocked(c->ctx->streams, NULL, 0);
    std::vector<puddle_block> all(n ? n : 1);
    n = puddle_streams_blocked(c->ctx->streams, all.data(), n);
    PartialUsers people(c->ctx);
    beginPage(w);
    size_t i = p.offset;
    for (size_t shown = 0; i < n && shown < p.limit && w->len + 2000 <= w->cap; i++, shown++) {
        puddle_jw_object(w);
        puddle_jw_key(w, "user");
        people.write(w, all[i].id);
        puddle_jw_num(w, "created_at", all[i].ts_ms);
        puddle_jw_end_object(w);
    }
    endPage(w, i < n ? std::optional<size_t>(i) : std::nullopt);
    return 200;
}

/* Their direct messages through Ponds, dropped unread (docs/pond-dms.md, "Blocks"): the rest of what they do still shows. */
static pdm::Store &dmBlocks(const puddle_call *c)
{
    if (!pdm::of(c->ctx))
        throw Error::notFound();
    return *pdm::of(c->ctx);
}

static void writeDmBlock(puddle_jw *w, const puddle_call *c, const std::string &id)
{
    puddle_jw_object(w);
    puddle_jw_str(w, "id", id.c_str());
    puddle_jw_flag(w, "dms_blocked", dmBlocks(c).blocked(id));
    puddle_jw_end_object(w);
}

static int dm_block(const puddle_call *c, puddle_jw *w)
{
    std::string id = puddle_param(c, "user");
    if (dmBlocks(c).block(id, puddle_call_now_ms(c)) < 0)
        throw Error::internal();
    writeDmBlock(w, c, id);
    return 200;
}

static int dm_unblock(const puddle_call *c, puddle_jw *w)
{
    std::string id = puddle_param(c, "user");
    if (dmBlocks(c).unblock(id) < 0)
        throw Error::internal();
    writeDmBlock(w, c, id);
    return 200;
}

static int list_dm_blocks(const puddle_call *c, puddle_jw *w)
{
    PartialUsers people(c->ctx);
    beginPage(w);
    for (auto &[id, ts] : dmBlocks(c).blocks()) {
        if (w->len + 2000 > w->cap)
            break;
        puddle_jw_object(w);
        puddle_jw_key(w, "user");
        people.write(w, id);
        puddle_jw_num(w, "created_at", ts);
        puddle_jw_end_object(w);
    }
    endPage(w, std::nullopt);
    return 200;
}

const puddle_route puddle_user_routes[] = {
    { "GET",  "/api/v1/users/me",                PUDDLE_AUTH_OWNER, 0,  get_me,       NULL },
    { "GET",  "/api/v1/users/{user:id}",         PUDDLE_AUTH_OWNER, 0,  get_user,     NULL },
    { "GET",  "/api/v1/users/{user:id}/profiles", PUDDLE_AUTH_OWNER, 0, user_profiles, NULL },
    { "POST", "/api/v1/users/{user:id}/block",   PUDDLE_AUTH_OWNER, 64, block_user,   NULL },
    { "POST", "/api/v1/users/{user:id}/unblock", PUDDLE_AUTH_OWNER, 64, unblock_user, NULL },
    { "GET",  "/api/v1/blocks",                  PUDDLE_AUTH_OWNER, 0,  list_blocks,  NULL },
    { "POST", "/api/v1/users/{user:id}/dm-block",   PUDDLE_AUTH_OWNER, 64, dm_block,   NULL },
    { "POST", "/api/v1/users/{user:id}/dm-unblock", PUDDLE_AUTH_OWNER, 64, dm_unblock, NULL },
    { "GET",  "/api/v1/dm-blocks",               PUDDLE_AUTH_OWNER, 0,  list_dm_blocks, NULL },
};
const size_t puddle_user_routes_count = sizeof puddle_user_routes / sizeof puddle_user_routes[0];
