#pragma once

// Who is in a group, as its events add up to. Nobody is in charge: every Puddle folds the same events by the same
// rules and gets the same members, in whatever order the events came.
//   - rename: from a member; the newer one wins
//   - invite: from a member, naming someone not in; a leaver may be invited again by a later event
//   - join: only from someone invited, answering an invitation older than it
//   - leave: from a member or invitee, for themselves
//   - never more than 50 members and invited together
//
// objects::Group is a GroupState plus what only this Puddle knows: whether we were invited and haven't answered,
// who invited us, addresses.

#include "events/group.hpp"

#include <vector>

namespace puddle::events {

struct GroupMemberState {
    std::string id;
    enum class State { Invited, Member, Left } state;
    std::string invitedBy;
    int64_t ts = 0;
};

class GroupState {
public:
    explicit GroupState(const ChatCreate &create);

    // 0 changed, 1 fine but nothing changed (already so, or stale), -1 not allowed (`why` says which rule).
    int apply(const ChatRename &e, std::string *why = nullptr);
    int apply(const MemberInvite &e, std::string *why = nullptr);
    int apply(const MemberJoin &e, std::string *why = nullptr);
    int apply(const MemberLeave &e, std::string *why = nullptr);

    const std::string &id() const { return id_; }
    const std::string &name() const { return name_; }
    const std::string &creator() const { return creator_; }
    const std::vector<GroupMemberState> &members() const { return members_; }

private:
    std::string id_, name_, creator_;
    int64_t nameTs_ = 0;
    std::vector<GroupMemberState> members_;
};

} // namespace puddle::events
