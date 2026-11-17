#pragma once

// A group chat we are in or invited to, and its members.
//
//     auto g = Group::create(puddle, "Climbing", {alice, bob});
//     g.invite(carol);
//     g.rename("Climbing (Thursdays)");

#include "objects/member.hpp"
#include "objects/user.hpp"

#include <optional>
#include <string>
#include <vector>

namespace puddle {

class Group {
public:
    enum class Mine { Invited, In, Left, Declined };

    static std::optional<Group> get(Puddle &puddle, const std::string &id);
    static std::vector<Group> all(Puddle &puddle);
    static Group create(Puddle &puddle, const std::string &name, const std::vector<User> &invite);

    const std::string &id() const { return id_; }
    const std::string &name() const { return name_; }
    Mine mine() const { return mine_; }
    User creator() const;
    std::vector<Member> members() const;

    Group invite(const User &who);
    Group accept();
    Group decline();
    Group leave();
    Group rename(const std::string &name);

private:
    Puddle *puddle_;
    std::string id_, name_, creatorId_;
    Mine mine_ = Mine::In;
};

} // namespace puddle
