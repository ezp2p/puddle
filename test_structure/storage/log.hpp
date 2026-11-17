#pragma once

// Append-only logs: the chat logs. A log is named (a chat id) and holds entries ordered by (ts, id). Nothing in it
// is ever changed or removed; an edit or a deletion is a new entry.
//
//     auto r = storage.logs().append(chatId, {.id = ev.id(), .ts = ts, .bytes = ev.toJson()});
//     if (r == Appended::Duplicate) ...                  // we already had it
//     for (auto &e : storage.logs().tail(chatId, 50)) ...

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace puddle::storage {

struct LogEntry {
    std::string id;     // unique within its log; a second append of the same id is a duplicate
    int64_t ts = 0;     // with id, the order
    std::string bytes;  // stored and returned exactly as given
};

// Logs are split into chunks so two Puddles can compare what they hold one chunk at a time.
// A chunk is one UTC day of a log.
struct Chunk {
    std::string name;   // "2026-10-23"
    std::string hash;   // over the entries' ids, in order
    size_t count = 0;
};

enum class Appended { New, Duplicate };

class LogStore {
public:
    virtual ~LogStore() = default;

    // Safe on disk before it returns.
    virtual Appended append(const std::string &log, const LogEntry &entry) = 0;

    virtual std::optional<LogEntry> get(const std::string &log, const std::string &id) = 0;

    // The newest `n`, returned oldest first. `before`: only entries older than this id.
    virtual std::vector<LogEntry> tail(const std::string &log, size_t n, const std::string &before = {}) = 0;

    virtual std::vector<LogEntry> chunk(const std::string &log, const std::string &chunkName) = 0;
    virtual std::vector<Chunk> chunks(const std::string &log) = 0;
    virtual std::vector<std::string> logs() = 0; // every log there is
};

} // namespace puddle::storage
