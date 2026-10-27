#pragma once

// One record per conversation, kept up to date as events arrive, so listing conversations reads a hundred small
// records instead of working them out from every stream, group and chat log on each request.
//
// The records are kept by core, not by a route. Whenever Chats appends an event, Conversations updates that chat's
// record: its latest message, its unread count, its order. A route only reads them.
//
// Two kinds of field, and the difference matters:
//   - derived: latest message, unread count, sort time. Worked out from the chat log, so if the records are lost or
//     look wrong, rebuild() makes them again from the logs (the logs stay the source of truth, as in docs/storage.md).
//   - the owner's own: when they last read it. Nothing else holds it, so it can't be rebuilt; it lives only here.
//
//     engine.conversations().list(50, 0);           // newest activity first
//     engine.conversations().markRead(chatId, now);
//
// Records "conversations", key = conversation id (model/records/conversation.yaml).

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace puddle::storage { class RecordStore; }

namespace puddle::core {

class Chats;

struct ConversationRecord {
    std::string id;           // the other person's user id for a DM, the group id for a group
    std::string chat;         // the chat log it reads (a DM's chat id is a hash of both user ids)
    enum class Kind { Dm, Group } kind;
    int64_t sortTs = 0;       // the latest message, or when the group was made
    int64_t readAt = 0;       // the owner's own: when they last read it (0 = never)
    int unread = 0;           // messages from others received after readAt
    struct Last {
        std::string id, sender;
        int64_t ts = 0;
        std::string preview;  // the start of the text, "" if taken back
        bool deleted = false;
    };
    std::optional<Last> last;
};

class Conversations {
public:
    Conversations(storage::RecordStore &records, Chats &chats);

    std::optional<ConversationRecord> get(const std::string &id);
    std::vector<ConversationRecord> list(size_t limit, size_t offset); // by sortTs, newest first

    void markRead(const std::string &id, int64_t now);
    void ensure(const std::string &id, ConversationRecord::Kind kind, int64_t now); // a new friend or group: listed before anything is said

    // Rebuild every derived field from the chat logs, keeping each readAt. Run when the records are missing or
    // were written by an older version.
    void rebuild();

private:
    void onAppend(const std::string &chat, const std::string &bytes); // hooked to Chats::onAppend in the constructor

    storage::RecordStore &records_;
    Chats &chats_;
};

} // namespace puddle::core
