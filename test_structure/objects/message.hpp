#pragma once

// One entry in a conversation, built from its events (events::chatView) plus what only this Puddle knows.
// Something someone wrote, or what happened to a group (created, renamed,
// someone invited, joined, left).
//
//     auto msg = Message::get(puddle, conversationId, messageId);
//
// get() needs both ids: the conversation says which log to read, and the id finds the message in it
// (chats().get, through the storage driver's index, so no scanning of day files). The message's own edits and
// redaction come from chats().about, and events::foldMessage applies the same rules as a whole chat view.
// Delivery state is added from the outbox for the owner's own messages.
//
//     msg->edit("fixed the typo");
//     msg->reply("and another thing");
//     std::cout << msg->author().displayName();

#include "objects/user.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace puddle {

enum class MessageType { Default, GroupCreate, GroupRename, GroupInvite, GroupJoin, GroupLeave };

struct Delivery {
    enum class State { Queued, Retrying, Failed, Held } state;
    int attempts = 0;
    std::optional<int64_t> nextAt;
    std::string reason;
};

class Message {
public:
    static std::optional<Message> get(Puddle &puddle, const std::string &conversation, const std::string &id);

    const std::string &id() const { return id_; }
    const std::string &conversationId() const { return conversation_; }
    MessageType type() const { return type_; }
    User author() const;
    const std::optional<std::string> &body() const { return body_; } // empty once taken back
    int64_t ts() const { return ts_; }
    bool outgoing() const { return outgoing_; }
    bool edited() const { return editedAt_.has_value(); }
    bool deleted() const { return deleted_; }
    std::optional<Message> replyTo() const;
    const std::optional<Delivery> &delivery() const { return delivery_; } // our own, not yet delivered

    Message edit(const std::string &body);  // only our own
    Message remove();                       // only our own; a request to hide, not a guarantee
    Message reply(const std::string &body);

private:
    Puddle *puddle_;
    std::string id_, conversation_, authorId_, replyToId_;
    MessageType type_ = MessageType::Default;
    std::optional<std::string> body_;
    int64_t ts_ = 0;
    std::optional<int64_t> editedAt_;
    bool outgoing_ = false, deleted_ = false;
    std::optional<Delivery> delivery_;
};

} // namespace puddle
