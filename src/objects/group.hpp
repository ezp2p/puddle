#pragma once

// A group chat the owner is in or invited to (test_structure/model/objects/group.yaml). Who is in it comes from
// folding its group events (group.h); where the owner stands (`my`, who invited us) is this Puddle's own.
//
//     auto g = puddle::Group::create(ctx, "Climbing");
//     g.invite(bobId);
//     g.rename("Climbing (Thursdays)");

#include "core/group.h"
#include "core/context.h"
#include "base/json.h"

#include <optional>
#include <string>
#include <vector>

namespace puddle {

class PartialUsers;

class Group {
public:
    static std::optional<Group> get(puddle_ctx *ctx, const std::string &id);
    static std::vector<Group> all(puddle_ctx *ctx); // in or invited, newest change first
    static Group create(puddle_ctx *ctx, const std::string &name);

    Group(Group &&other) noexcept;
    Group &operator=(Group &&other) noexcept;
    Group(const Group &) = delete;
    Group &operator=(const Group &) = delete;
    ~Group();

    std::string id() const { return g_.id; }
    std::string name() const { return g_.name; }
    puddle_group_my mine() const { return g_.my; }
    const puddle_group &record() const { return g_; }
    size_t memberCount() const { return puddle_group_count(&g_, PUDDLE_GM_MEMBER); }

    Group invite(const std::string &userId);
    Group accept();
    Group decline();
    Group leave();
    Group rename(const std::string &name);
    // Show the members `profileId` (one of the owner's), or nothing (nullopt); they are told.
    Group showProfile(const std::optional<std::string> &profileId);
    std::string place() const; // "group:<id>": where a profile is shown

    void write(puddle_jw *w, PartialUsers &people) const;          // Group, with its members
    void writeSummary(puddle_jw *w, PartialUsers &people) const;   // without the member list: for a conversation

private:
    Group(puddle_ctx *ctx, const puddle_group &g) : ctx_(ctx), g_(g) {} // takes ownership of g's members
    Group reload() const;

    puddle_ctx *ctx_;
    puddle_group g_;
};

} // namespace puddle
