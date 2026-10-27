#include "objects/user.hpp"

#include "core/error.hpp"
#include "core/group.h"
#include "core/identity.h"
#include "core/people.h"

#include <cstdlib>
#include <cstring>

namespace puddle {

namespace {

std::string ownId(puddle_ctx *ctx)
{
    char id[PUDDLE_USER_ID_LEN + 1];
    puddle_identity_id(ctx->identity, id);
    return id;
}

// The owner as they are in `place` ("dm" or "group:<group>", as a peer's records name places): the profile they chose
// for that group, or else their first profile. Only ever shown to the owner.
std::optional<Profile> ownProfile(puddle_ctx *ctx, const std::string &place)
{
    if (place != "dm")
        if (auto id = Profile::shownAt(ctx, place))
            if (auto p = Profile::get(ctx, *id))
                return p;
    auto all = Profile::all(ctx);
    if (all.empty())
        return std::nullopt;
    return std::move(all.front());
}

std::string ownProfileJson(puddle_ctx *ctx, const std::string &place = "dm")
{
    auto p = ownProfile(ctx, place);
    return p ? p->fieldsJson() : std::string();
}

bool sharesGroupWith(puddle_ctx *ctx, const std::string &id)
{
    if (!ctx->groups)
        return false;
    puddle_group *all;
    size_t n;
    if (puddle_groups_list(ctx->groups, &all, &n) != 0)
        return false;
    bool found = false;
    for (size_t i = 0; i < n && !found; i++) {
        if (all[i].my != PUDDLE_GROUP_IN && all[i].my != PUDDLE_GROUP_INVITED)
            continue;
        const puddle_gmember *m = puddle_group_member(&all[i], id.c_str());
        found = m && m->state != PUDDLE_GM_LEFT;
    }
    puddle_group_list_free(all, n);
    return found;
}

std::string member(const std::string &json, const char *key, bool asString)
{
    puddle_span v;
    if (json.empty() || puddle_json_member(json.data(), json.size(), key, &v) != 0)
        return {};
    if (!asString)
        return std::string(v.p, v.len);
    char out[512];
    return puddle_json_string(v, out, sizeof out) == 0 ? std::string(out) : std::string();
}

// the profile their Puddle sent us for `place` ("dm" or "group:<group>"), or "" if none has come
std::string peerProfile(puddle_ctx *ctx, const char *id, const char *place = PUDDLE_PEOPLE_DM)
{
    char *json;
    size_t len;
    if (!ctx->people || puddle_people_profile(ctx->people, id, place, &json, &len) != 0)
        return std::string();
    std::string out(json, len);
    free(json);
    return out;
}

} // namespace

std::optional<User> User::get(puddle_ctx *ctx, const std::string &id)
{
    if (!puddle_id_valid(id.c_str()))
        return std::nullopt;
    if (id == ownId(ctx))
        return me(ctx);
    puddle_stream st;
    if (puddle_streams_get(ctx->streams, id.c_str(), &st) == 0) {
        User u = fromStream(ctx, st);
        puddle_stream_free(&st);
        return u;
    }
    User u(ctx, id);
    u.blocked_ = puddle_streams_is_blocked(ctx->streams, id.c_str()) != 0;
    if (!u.blocked_ && !sharesGroupWith(ctx, id))
        return std::nullopt;
    u.profile_ = peerProfile(ctx, id.c_str());
    return u;
}

User User::fromStream(puddle_ctx *ctx, const puddle_stream &st)
{
    User u(ctx, st.id);
    u.stream_ = st.state;
    u.profile_ = peerProfile(ctx, st.id);
    u.blocked_ = puddle_streams_is_blocked(ctx->streams, st.id) != 0;
    return u;
}

User User::me(puddle_ctx *ctx)
{
    User u(ctx, ownId(ctx));
    u.me_ = true;
    u.profile_ = ownProfileJson(ctx);
    return u;
}

std::optional<Profile> User::profileIn(const std::string &place) const
{
    if (me_)
        return ownProfile(ctx_, place);
    return Profile::of(ctx_, id_, place);
}

std::string User::displayName() const
{
    std::string name = member(profile_, "display_name", true);
    return name.empty() ? fallbackName(id_) : name;
}

User User::block()
{
    if (me_)
        throw Error::badRequest("cannot_block_yourself");
    char err[300];
    if (puddle_streams_block(ctx_->streams, id_.c_str(), puddle_ctx_now_ms(ctx_), err, sizeof err) != 0)
        throw Error::internal(err);
    return *get(ctx_, id_);
}

User User::unblock()
{
    char err[300];
    int rc = puddle_streams_unblock(ctx_->streams, id_.c_str(), err, sizeof err);
    if (rc == 1)
        throw Error::notFound();
    if (rc != 0)
        throw Error::internal(err);
    auto u = get(ctx_, id_);
    if (u)
        return *u;
    User gone(ctx_, id_); // only blocked, and now not even that: what we know of them is their id
    return gone;
}

void User::write(puddle_jw *w) const
{
    puddle_jw_object(w);
    puddle_jw_str(w, "id", id_.c_str());
    if (!profile_.empty()) {
        puddle_jw_key(w, "profile");
        puddle_jw_raw(w, profile_.data(), profile_.size());
    }
    if (stream_)
        puddle_jw_str(w, "stream", puddle_stream_state_name(*stream_));
    if (blocked_)
        puddle_jw_flag(w, "blocked", 1);
    puddle_jw_end_object(w);
}

void User::writeSelf(puddle_jw *w) const
{
    char link[256];
    puddle_jw_object(w);
    puddle_jw_str(w, "id", id_.c_str());
    if (auto p = ownProfile(ctx_, "dm")) {
        puddle_jw_key(w, "profile");
        p->write(w);
    }
    puddle_jw_key(w, "link");
    if (ctx_->config && puddle_make_link(ctx_->config, NULL, link, sizeof link))
        puddle_jw_string(w, link);
    else
        puddle_jw_null(w);
    puddle_jw_end_object(w);
}

void User::writePartial(puddle_jw *w, puddle_ctx *ctx, const std::string &id)
{
    PartialUsers p(ctx);
    p.write(w, id);
}

// ---- PartialUsers ----

const PartialUsers::Entry &PartialUsers::lookup(const std::string &id)
{
    std::string key = place_ + " " + id;
    auto it = cache_.find(key);
    if (it != cache_.end())
        return it->second;
    std::string profile;
    if (id == ownId(ctx_)) {
        profile = ownProfileJson(ctx_, place_);
    } else {
        profile = peerProfile(ctx_, id.c_str(), place_.c_str());
    }
    Entry e;
    e.name = member(profile, "display_name", true);
    if (e.name.empty())
        e.name = User::fallbackName(id);
    e.avatar = member(profile, "avatar", false);
    e.color = member(profile, "color", false);
    return cache_.emplace(key, std::move(e)).first->second;
}

std::string PartialUsers::name(const std::string &id)
{
    return lookup(id).name;
}

void PartialUsers::write(puddle_jw *w, const std::string &id)
{
    const Entry &e = lookup(id);
    puddle_jw_object(w);
    puddle_jw_str(w, "id", id.c_str());
    puddle_jw_str(w, "display_name", e.name.c_str());
    if (!e.avatar.empty()) {
        puddle_jw_key(w, "avatar");
        puddle_jw_raw(w, e.avatar.data(), e.avatar.size());
    }
    if (!e.color.empty()) {
        puddle_jw_key(w, "color");
        puddle_jw_raw(w, e.color.data(), e.color.size());
    }
    puddle_jw_end_object(w);
}

} // namespace puddle
