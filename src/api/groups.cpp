/* Owner API: group chats. /api/v1/groups/... (docs/objects.md). Their messages are under /api/v1/conversations. */
#include "api/api.h"
#include "api/request.hpp"
#include "objects/group.hpp"
#include "objects/user.hpp"

#include <cstdio>

using namespace puddle;
using namespace puddle::api;

static Group groupOf(const puddle_call *c)
{
    auto g = Group::get(c->ctx, puddle_param(c, "group"));
    if (!g)
        throw Error::notFound();
    return std::move(*g);
}

static int answer(const puddle_call *c, puddle_jw *w, const Group &g, int status = 200)
{
    PartialUsers people(c->ctx);
    g.write(w, people);
    return status;
}

static int list_groups(const puddle_call *c, puddle_jw *w)
{
    Page p = paging(c, 100);
    auto all = Group::all(c->ctx);
    PartialUsers people(c->ctx);
    beginPage(w);
    size_t i = p.offset;
    for (size_t shown = 0; i < all.size() && shown < p.limit && w->len + 12000 <= w->cap; i++, shown++)
        all[i].write(w, people);
    endPage(w, i < all.size() ? std::optional<size_t>(i) : std::nullopt);
    return 200;
}

/* Start a group and invite the friends named. Those who could not be asked are listed, with why, beside the group:
 * {"group": Group, "not_invited": [{"user", "reason"}]} */
static int create_group(const puddle_call *c, puddle_jw *w)
{
    Body b(c);
    std::string name = b.string("name", PUDDLE_GROUP_NAME_MAX_BYTES + 4);
    puddle_span members{};
    bool hasMembers = b.has("members");
    if (hasMembers) {
        members = b.raw("members");
        if (members.p[0] != '[')
            throw Error::badRequest();
    }
    Group g = Group::create(c->ctx, name);
    if (b.has("profile") && !b.isNull("profile")) /* who the members will see the owner as */
        g = g.showProfile(b.string("profile", PUDDLE_PROFILE_ID_MAX + 4));
    struct Failed { std::string user, reason; };
    std::vector<Failed> failed;
    size_t cursor = 0;
    puddle_span item;
    int asked = 0;
    while (hasMembers && puddle_json_array_next(members, &cursor, &item) == 1 && asked < PUDDLE_GROUP_MAX_MEMBERS) {
        char id[128];
        asked++;
        if (puddle_json_string(item, id, sizeof id) != 0 || !puddle_id_valid(id))
            continue;
        try {
            g = g.invite(id);
        } catch (const Error &e) {
            failed.push_back({ id, e.code() });
        }
    }
    PartialUsers people(c->ctx);
    puddle_jw_object(w);
    puddle_jw_key(w, "group");
    g.write(w, people);
    puddle_jw_key(w, "not_invited");
    puddle_jw_array(w);
    for (const Failed &f : failed) {
        puddle_jw_object(w);
        puddle_jw_str(w, "user", f.user.c_str());
        puddle_jw_str(w, "reason", f.reason.c_str());
        puddle_jw_end_object(w);
    }
    puddle_jw_end_array(w);
    puddle_jw_end_object(w);
    return 201;
}

static int get_group(const puddle_call *c, puddle_jw *w)
{
    return answer(c, w, groupOf(c));
}

static int invite(const puddle_call *c, puddle_jw *w)
{
    Body b(c);
    return answer(c, w, groupOf(c).invite(b.id("user")));
}

/* Join. {"profile": id}, if given, is who the members will see the owner as. */
static int accept(const puddle_call *c, puddle_jw *w)
{
    Body b(c);
    Group g = groupOf(c);
    if (b.has("profile") && !b.isNull("profile"))
        g = g.showProfile(b.string("profile", PUDDLE_PROFILE_ID_MAX + 4));
    return answer(c, w, g.accept());
}

/* Choose which of the owner's profiles the members see. `profile: null`: none (they see only the id). */
static int group_profile(const puddle_call *c, puddle_jw *w)
{
    Body b(c);
    if (!b.has("profile"))
        throw Error::badRequest();
    std::optional<std::string> pid;
    if (!b.isNull("profile"))
        pid = b.string("profile", PUDDLE_PROFILE_ID_MAX + 4);
    return answer(c, w, groupOf(c).showProfile(pid));
}
static int decline(const puddle_call *c, puddle_jw *w) { return answer(c, w, groupOf(c).decline()); }
static int leave(const puddle_call *c, puddle_jw *w) { return answer(c, w, groupOf(c).leave()); }

static int rename_group(const puddle_call *c, puddle_jw *w)
{
    Body b(c);
    return answer(c, w, groupOf(c).rename(b.string("name", PUDDLE_GROUP_NAME_MAX_BYTES + 4)));
}

const puddle_route puddle_group_api_routes[] = {
    { "GET",  "/api/v1/groups",                    PUDDLE_AUTH_OWNER, 0,    list_groups,  NULL },
    { "POST", "/api/v1/groups/create",             PUDDLE_AUTH_OWNER, 8192, create_group, NULL },
    { "GET",  "/api/v1/groups/{group:id}",         PUDDLE_AUTH_OWNER, 0,    get_group,    NULL },
    { "POST", "/api/v1/groups/{group:id}/invite",  PUDDLE_AUTH_OWNER, 1024, invite,       NULL },
    { "POST", "/api/v1/groups/{group:id}/accept",  PUDDLE_AUTH_OWNER, 128,  accept,       NULL },
    { "POST", "/api/v1/groups/{group:id}/profile", PUDDLE_AUTH_OWNER, 128,  group_profile, NULL },
    { "POST", "/api/v1/groups/{group:id}/decline", PUDDLE_AUTH_OWNER, 64,   decline,      NULL },
    { "POST", "/api/v1/groups/{group:id}/leave",   PUDDLE_AUTH_OWNER, 64,   leave,        NULL },
    { "POST", "/api/v1/groups/{group:id}/rename",  PUDDLE_AUTH_OWNER, 2048, rename_group, NULL },
};
const size_t puddle_group_api_routes_count = sizeof puddle_group_api_routes / sizeof puddle_group_api_routes[0];
