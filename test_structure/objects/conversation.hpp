#pragma once

// A DM or a group chat: its history, its unread count, and sending into it.
//
//     auto chat = Conversation::get(puddle, id);  // a user id for a DM, a group id for a group
//     for (auto &m : chat->messages({.limit = 50}))
//         ...
//     chat->send("hello");
//     chat->markRead();

#include "objects/message.hpp"

#include <optional>
#include <string>
#include <vector>

namespace puddle {

class Group;

class Conversation {
public:
    enum class Kind { Dm, Group };
    struct Page { std::string before; int limit = 50; };

    static std::optional<Conversation> get(Puddle &puddle, const std::string &id);
    static Conversation dm(Puddle &puddle, const std::string &userId);
    static std::vector<Conversation> all(Puddle &puddle); // newest first

    const std::string &id() const { return id_; }
    Kind kind() const { return kind_; }
    std::string name() const;
    int unread() const { return unread_; }
    std::optional<Message> lastMessage() const;
    std::optional<User> recipient() const; // a DM's other person
    std::optional<Group> group() const;    // a group's details

    std::vector<Message> messages(const Page &page) const; // oldest first
    Message send(const std::string &body, const std::string &replyTo = {});
    Conversation markRead();
    Conversation retry(); // try again to deliver what is stuck

private:
    Puddle *puddle_;
    std::string id_;
    Kind kind_ = Kind::Dm;
    int unread_ = 0;
};

} // namespace puddle
