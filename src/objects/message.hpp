#pragma once

// One entry in a conversation, as the owner sees it (test_structure/model/objects/message.yaml): a message with its
// edits and redaction applied (message.h, puddle_chat_view), or something that happened to a group (a notice), plus
// what only this Puddle knows: whether the owner sent it, when it arrived, and how its delivery is going.
//
//     auto m = puddle::Message::get(ctx, conversationId, messageId);
//     if (m)
//         m->edit("fixed the typo");

#include "events/message.h"
#include "core/outbox.h"
#include "core/context.h"
#include "base/json.h"

#include <optional>
#include <string>

namespace puddle {

class PartialUsers;

// How the delivery of the owner's own messages is going, from what the outbox still holds (a message no longer in it
// has been delivered). Read once per answer.
class Deliveries {
public:
    Deliveries(puddle_ctx *ctx, const std::string &peer); // peer "": a group, where the worst of each member's delivery counts
    ~Deliveries();
    Deliveries(const Deliveries &) = delete;
    Deliveries &operator=(const Deliveries &) = delete;
    const puddle_ob_item *of(const std::string &messageId) const;

private:
    puddle_ob_item *items_ = nullptr;
    size_t n_ = 0;
    std::string peer_;
};

class Message {
public:
    enum class Type { Default, GroupCreate, GroupRename, GroupInvite, GroupJoin, GroupLeave };

    // `conversation` is the Conversation's id: the other person's for a DM, the group's for a group.
    static std::optional<Message> get(puddle_ctx *ctx, const std::string &conversation, const std::string &id);

    static Message fromView(const puddle_view_msg &v, const std::string &conversation, const std::string &chat);
    static std::optional<Message> fromNotice(const puddle_event &ev, const std::string &conversation);

    const std::string &id() const { return id_; }
    const std::string &sender() const { return sender_; }
    int64_t ts() const { return ts_; }
    Type type() const { return type_; }
    bool deleted() const { return deleted_; }
    const std::optional<std::string> &body() const { return body_; }

    Message edit(puddle_ctx *ctx, const std::string &text) const;
    Message remove(puddle_ctx *ctx) const;

    // `deliveries` may be null: nothing is said about delivery.
    void write(puddle_jw *w, puddle_ctx *ctx, PartialUsers &people, const Deliveries *deliveries) const;

private:
    Message() = default;

    std::string id_, conversation_, chat_, sender_, ref_, member_, name_;
    Type type_ = Type::Default;
    std::optional<std::string> body_;
    int64_t ts_ = 0, rts_ = 0;
    std::optional<int64_t> editedTs_;
    bool deleted_ = false;
};

// Sign an event of the owner's, keep it in `chat` and queue it for `to` (a DM). Throws if it can't; the id is in `id`.
void commitDirectEvent(puddle_ctx *ctx, const std::string &to, const std::string &chat, const char *event, size_t len,
                       char id[PUDDLE_ID_HEX + 1]);

// The message `target` of `chat` with what was done to it: true found (in *out, body malloc'd).
bool findInChat(puddle_ctx *ctx, const std::string &chat, const std::string &target, puddle_view_msg *out);

} // namespace puddle
