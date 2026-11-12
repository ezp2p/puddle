#pragma once

// A chat as its events add up to: each message with edits and redactions applied.
//
// These rules have to give the same answer on every Puddle, from the same events in any order, so they live here
// with the events and not in objects/:
//   - an edit or redaction counts only if it is by the message's author and later than the message
//   - the latest edit is the text
//   - a redaction beats any edit, whenever it arrived
//
// objects::Message is a ChatView entry plus what only this Puddle knows: outgoing, delivery, read, received_at.

#include "events/chat.hpp"

#include <span>
#include <vector>

namespace puddle::events {

struct ViewedMessage {
    std::string id, sender;
    int64_t ts = 0;
    std::optional<std::string> body; // empty once redacted
    std::optional<std::string> ref;
    std::optional<int64_t> editedTs;
    bool deleted = false;
};

std::vector<ViewedMessage> chatView(std::span<const SignedEvent> eventsInChatOrder);

// One message on its own: the message and the events that name it (core::Chats::about), by the same rules as
// chatView. objects::Message::get uses this, so a single message never needs the whole chat read.
std::optional<ViewedMessage> foldMessage(const SignedEvent &message, std::span<const SignedEvent> editsAndRedactions);

} // namespace puddle::events
