#pragma once

// The five events that change a group (model/events/chat.create.yaml and the rest). Messages in a group are the
// ordinary chat events with the group's id as `chat`.

#include "events/event.hpp"

#include <optional>
#include <string>

namespace puddle::events {

struct ChatCreate : Header {    // "chat.create": the creator; the group's id is made from creator, ts and nonce
    std::string name;
    std::string nonce;          // 32 hex characters
    std::string bytes() const;
    static std::optional<ChatCreate> parse(const std::string &bytes, std::string *why = nullptr);
};

struct ChatRename : Header {    // "chat.rename": any member
    std::string name;           // sent as `body`
    std::string bytes() const;
    static std::optional<ChatRename> parse(const std::string &bytes, std::string *why = nullptr);
};

struct MemberInvite : Header {  // "member.invite": any member, naming someone not already in
    std::string member;
    std::string bytes() const;
    static std::optional<MemberInvite> parse(const std::string &bytes, std::string *why = nullptr);
};

struct MemberJoin : Header {    // "member.join": the invitee, saying yes
    std::string bytes() const;
    static std::optional<MemberJoin> parse(const std::string &bytes, std::string *why = nullptr);
};

struct MemberLeave : Header {   // "member.leave": a member or invitee, for themselves
    std::string bytes() const;
    static std::optional<MemberLeave> parse(const std::string &bytes, std::string *why = nullptr);
};

} // namespace puddle::events
