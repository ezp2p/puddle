#include "objects/group.hpp"

#include "objects/profile.hpp"

#include "core/error.hpp"
#include "objects/user.hpp"

#include <algorithm>
#include <cstring>

namespace puddle {

namespace {

// what the group flow says went wrong, as an Error
void check(int status, const char *code)
{
    if (status)
        throw Error(status, code ? code : "internal");
}

} // namespace

Group::Group(Group &&other) noexcept : ctx_(other.ctx_), g_(other.g_)
{
    other.g_.members = nullptr;
    other.g_.nmembers = 0;
}

Group &Group::operator=(Group &&other) noexcept
{
    if (this != &other) {
        puddle_group_free(&g_);
        ctx_ = other.ctx_;
        g_ = other.g_;
        other.g_.members = nullptr;
        other.g_.nmembers = 0;
    }
    return *this;
}

Group::~Group()
{
    puddle_group_free(&g_);
}

std::optional<Group> Group::get(puddle_ctx *ctx, const std::string &id)
{
    puddle_group g;
    if (!ctx->groups || puddle_groups_get(ctx->groups, id.c_str(), &g) != 0)
        return std::nullopt;
    return Group(ctx, g);
}

std::vector<Group> Group::all(puddle_ctx *ctx)
{
    std::vector<Group> out;
    puddle_group *list;
    size_t n;
    if (!ctx->groups)
        return out; // a Puddle without group chats (some tests) has none
    if (puddle_groups_list(ctx->groups, &list, &n) != 0)
        throw Error::internal();
    for (size_t i = 0; i < n; i++)
        if (list[i].my == PUDDLE_GROUP_IN || list[i].my == PUDDLE_GROUP_INVITED) {
            out.push_back(Group(ctx, list[i]));
            list[i].members = nullptr; // now the Group's
            list[i].nmembers = 0;
        }
    puddle_group_list_free(list, n);
    std::sort(out.begin(), out.end(), [](const Group &a, const Group &b) {
        if (a.g_.updated_ms != b.g_.updated_ms)
            return a.g_.updated_ms > b.g_.updated_ms;
        return strcmp(a.g_.id, b.g_.id) < 0;
    });
    return out;
}

Group Group::create(puddle_ctx *ctx, const std::string &name)
{
    char gid[PUDDLE_USER_ID_LEN + 1];
    const char *code = nullptr;
    check(ctx->courier->group_create(ctx, name.c_str(), gid, &code), code);
    auto g = get(ctx, gid);
    if (!g)
        throw Error::internal();
    return std::move(*g);
}

Group Group::reload() const
{
    auto g = get(ctx_, g_.id);
    if (!g)
        throw Error::notFound();
    return std::move(*g);
}

Group Group::invite(const std::string &userId)
{
    const char *code = nullptr;
    check(ctx_->courier->group_invite(ctx_, g_.id, userId.c_str(), &code), code);
    return reload();
}

Group Group::accept()
{
    const char *code = nullptr;
    check(ctx_->courier->group_accept(ctx_, g_.id, &code), code);
    return reload();
}

Group Group::decline()
{
    const char *code = nullptr;
    check(ctx_->courier->group_decline(ctx_, g_.id, &code), code);
    return reload();
}

Group Group::leave()
{
    const char *code = nullptr;
    check(ctx_->courier->group_leave(ctx_, g_.id, &code), code);
    return reload();
}

Group Group::rename(const std::string &name)
{
    const char *code = nullptr;
    check(ctx_->courier->group_rename(ctx_, g_.id, name.c_str(), &code), code);
    return reload();
}

Group Group::showProfile(const std::optional<std::string> &profileId)
{
    Profile::showAt(ctx_, place(), profileId);
    return reload();
}

std::string Group::place() const
{
    return std::string("group:") + g_.id;
}

void Group::writeSummary(puddle_jw *w, PartialUsers &people) const
{
    PartialUsers::In here(people, place()); // everyone as they are in this group
    puddle_jw_object(w);
    puddle_jw_str(w, "id", g_.id);
    puddle_jw_str(w, "name", g_.name);
    puddle_jw_str(w, "my", puddle_group_my_name(g_.my));
    puddle_jw_key(w, "creator");
    people.write(w, g_.creator);
    puddle_jw_num(w, "member_count", (int64_t)memberCount());
    puddle_jw_num(w, "created_at", g_.created_ms);
    puddle_jw_num(w, "updated_at", g_.updated_ms);
    puddle_jw_end_object(w);
}

void Group::write(puddle_jw *w, PartialUsers &people) const
{
    PartialUsers::In here(people, place()); // everyone as they are in this group
    puddle_jw_object(w);
    puddle_jw_str(w, "id", g_.id);
    puddle_jw_str(w, "name", g_.name);
    puddle_jw_str(w, "my", puddle_group_my_name(g_.my));
    if (auto mine = Profile::shownAt(ctx_, place()))
        puddle_jw_str(w, "profile", mine->c_str()); /* which of the owner's profiles the members see */
    puddle_jw_key(w, "creator");
    people.write(w, g_.creator);
    if (g_.invited_by[0] && g_.my == PUDDLE_GROUP_INVITED) {
        puddle_jw_key(w, "invited_by");
        people.write(w, g_.invited_by);
    }
    puddle_jw_key(w, "members");
    puddle_jw_array(w);
    for (size_t i = 0; i < g_.nmembers; i++) {
        const puddle_gmember &m = g_.members[i];
        if (m.state == PUDDLE_GM_LEFT)
            continue;
        puddle_jw_object(w);
        puddle_jw_key(w, "user");
        people.write(w, m.id);
        puddle_jw_str(w, "state", puddle_gm_state_name(m.state));
        if (m.by[0]) {
            puddle_jw_key(w, "invited_by");
            people.write(w, m.by);
        }
        puddle_jw_num(w, "since", m.ts);
        puddle_jw_end_object(w);
    }
    puddle_jw_end_array(w);
    puddle_jw_num(w, "created_at", g_.created_ms);
    puddle_jw_num(w, "updated_at", g_.updated_ms);
    puddle_jw_end_object(w);
}

} // namespace puddle
