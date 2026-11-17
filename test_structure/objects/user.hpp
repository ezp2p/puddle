#pragma once

// A person as this Puddle knows them: a friend, someone asking for a stream, someone we share a group with,
// or someone we blocked.
//
//     auto bob = User::get(puddle, bobId);     // nullopt if we know nothing of them
//     if (bob) {
//         bob->message("hi");                  // a DM; throws Error("no_stream") without an active stream
//         std::cout << bob->id() << " " << bob->displayName();
//     }
//
// A User is a snapshot of one moment. get() reads it; an action returns the new state (block() returns the
// blocked User) or what it made (message() returns the Message).

#include "objects/error.hpp"
#include "objects/profile.hpp"
#include "objects/stream.hpp"

#include <optional>
#include <string>

namespace puddle {

class Puddle;
class Message;
class Conversation;

class User {
public:
    static std::optional<User> get(Puddle &puddle, const std::string &id);
    static User me(Puddle &puddle);

    // what we know
    const std::string &id() const { return id_; }
    const std::optional<Profile> &profile() const { return profile_; } // what they show us; empty until they have
    std::string displayName() const;                                     // their profile's name, else a short form of the id
    std::optional<StreamState> streamState() const { return stream_; }   // empty when there is no stream
    bool blocked() const { return blocked_; }
    bool isMe() const { return me_; }

    // what they lead to
    std::optional<Stream> stream() const;
    Conversation conversation() const; // the DM with them

    // what can be done
    Message message(const std::string &body, const std::string &replyTo = {});
    User block();
    User unblock();

private:
    User(Puddle &puddle, std::string id) : puddle_(&puddle), id_(std::move(id)) {}

    Puddle *puddle_;
    std::string id_;
    std::optional<Profile> profile_;
    std::optional<StreamState> stream_;
    bool blocked_ = false;
    bool me_ = false;
};

} // namespace puddle
