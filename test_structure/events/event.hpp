#pragma once

// A signed event: what crosses between Puddles, and what every chat log is made of (model/events/envelope.yaml).
//
// An event is one compact JSON object, kept and sent exactly as it was signed. Its id is the SHA-256 of those bytes,
// so it is never re-serialized. The signature covers "puddle-event-v1\n" followed by the bytes and travels beside
// them as "<key id>:<base64>".
//
//     auto ev = MessageEvent{.chat = chat, .sender = me, .ts = now, .body = "hi"};
//     SignedEvent signed = SignedEvent::sign(identity, ev.bytes());
//     ...
//     auto in = SignedEvent::parse(json);           // from a Delivery
//     if (in.verify(senderKey)) {
//         auto any = AnyEvent::parse(in.bytes());   // a MessageEvent, an EditEvent, a ChatRename, ...
//     }

#include "core/identity.hpp"

#include <array>
#include <optional>
#include <string>
#include <variant>

namespace puddle::events {

class SignedEvent {
public:
    static SignedEvent sign(core::Identity &me, std::string bytes);
    static std::optional<SignedEvent> parse(const std::string &json); // {id, event, sig}

    const std::string &id() const { return id_; }       // hex SHA-256 of bytes()
    const std::string &bytes() const { return bytes_; } // the event's exact JSON text
    const std::string &keyId() const { return kid_; }
    int64_t ts() const { return ts_; }                   // from the event's header, read when parsed; with id(), its place in the log
    bool verify(const std::array<unsigned char, 32> &signingKey) const;

    std::string toJson() const;

private:
    std::string id_, bytes_, kid_, sig_;
    int64_t ts_ = 0;
};

// Every event has these. The rest depends on `type`.
struct Header {
    std::string type;   // "message", "edit", "redact", "chat.create", ...
    std::string chat;   // ChatId
    std::string sender; // UserId
    int64_t ts = 0;     // the sender's clock, UTC ms
};

} // namespace puddle::events
