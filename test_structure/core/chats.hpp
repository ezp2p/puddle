#pragma once

// The chat logs: signed events, kept exactly as signed. Replaces the old core::Store.
//
//     if (engine.chats().append(chatId, ev.id(), ev.ts(), ev.toJson())) ...   // true: new; false: we had it
//     auto page = engine.chats().tail(chatId, 50);
//
// Kept in storage's LogStore, but nothing in this header says so: layers above core never see storage types.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace puddle::storage { class LogStore; }

namespace puddle::core {

struct ChatEntry {
    std::string id;
    int64_t ts = 0;
    std::string bytes; // the SignedEvent as JSON, exactly as received
};

class Chats {
public:
    explicit Chats(storage::LogStore &logs) : logs_(logs) {}

    bool append(const std::string &chat, const std::string &id, int64_t ts, const std::string &bytes); // false: a duplicate
    std::vector<ChatEntry> tail(const std::string &chat, size_t n, const std::string &before = {});
    std::optional<ChatEntry> get(const std::string &chat, const std::string &id);
    // The edits and redactions that name this message (their `ref`), oldest first. They can sit in any later day of
    // the log, so Chats keeps an index of ref -> ids beside the log. Like every index it is derived: rebuilt from
    // the log when it is missing.
    std::vector<ChatEntry> about(const std::string &chat, const std::string &id);
    std::vector<std::string> all();

    // Called after every new append, from the thread that appended. api/updates.cpp listens here, so it hears about
    // events whether the owner sent them or a peer delivered them.
    void onAppend(std::function<void(const std::string &chat, const ChatEntry &)> listener);

private:
    storage::LogStore &logs_;
    std::vector<std::function<void(const std::string &, const ChatEntry &)>> listeners_;
};

} // namespace puddle::core
