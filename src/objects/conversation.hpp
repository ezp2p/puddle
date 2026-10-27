#pragma once

// A DM or a group chat (test_structure/model/objects/conversation.yaml). Its id is the other person's user id for a
// DM and the group's id for a group, so the same routes serve both.
//
//     auto chat = puddle::Conversation::get(ctx, id);
//     chat->send("hello");
//     chat->markRead();
//
// Nothing is stored for a conversation yet: each is worked out from the stream records, the groups, the end of its
// chat log and the read marker whenever it is asked for.

#include "objects/group.hpp"
#include "objects/message.hpp"
#include "core/context.h"
#include "base/json.h"
#include "core/streams.h"

#include <optional>
#include <string>
#include <vector>

namespace puddle {

class PartialUsers;

class Conversation {
public:
    enum class Kind { Dm, Group };

    static std::optional<Conversation> get(puddle_ctx *ctx, const std::string &id);
    // Every DM that has messages and every group the owner is in, newest activity first.
    static std::vector<Conversation> all(puddle_ctx *ctx);

    const std::string &id() const { return id_; }
    Kind kind() const { return kind_; }
    const std::string &chat() const { return chat_; }

    Message send(const std::string &text, const std::string &replyTo);
    Conversation markRead();
    Conversation retry(); // try again to deliver what is stuck; Error `nothing_to_retry` if nothing was

    void write(puddle_jw *w, PartialUsers &people) const;
    // A page of its history, `limit` messages ending `offset` from the newest, written oldest first; a group's notices
    // are messages too, in among them.
    void writeMessages(puddle_jw *w, PartialUsers &people, size_t limit, size_t offset) const;

private:
    Conversation(puddle_ctx *ctx, std::string id, Kind kind) : ctx_(ctx), id_(std::move(id)), kind_(kind) {}
    void summarise(); // the last message, the unread count and the sort time, from the end of the chat log

    puddle_ctx *ctx_;
    std::string id_, chat_;
    Kind kind_;
    std::string name_;
    std::optional<puddle_stream_state> stream_; // a DM's
    std::optional<Group> group_;                 // a group's
    int unread_ = 0;
    int64_t sortTs_ = 0;
    struct Last {
        std::string id, sender, preview;
        int64_t ts = 0;
        bool deleted = false;
    };
    std::optional<Last> last_;
};

} // namespace puddle
