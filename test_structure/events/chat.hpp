#pragma once

// The chat events, in DMs and groups alike (model/events/message.yaml, edit.yaml, redact.yaml).
// Each one builds its exact bytes for signing and parses strictly, with the rules every receiver applies.

#include "events/event.hpp"

#include <optional>
#include <string>

namespace puddle::events {

struct MessageEvent : Header {          // type "message"
    std::string body;                   // 1 to 4000 characters
    std::optional<std::string> ref;     // the message this replies to

    std::string bytes() const;
    static std::optional<MessageEvent> parse(const std::string &bytes, std::string *why = nullptr);
};

struct EditEvent : Header {             // type "edit"
    std::string ref;                    // the message being edited
    std::string body;                   // its new text

    std::string bytes() const;
    static std::optional<EditEvent> parse(const std::string &bytes, std::string *why = nullptr);
};

struct RedactEvent : Header {           // type "redact"; carries nothing else
    std::string ref;                    // the message being taken back

    std::string bytes() const;
    static std::optional<RedactEvent> parse(const std::string &bytes, std::string *why = nullptr);
};

} // namespace puddle::events
