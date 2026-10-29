#pragma once

// Records: one document per key, in named collections. Everything mutable that isn't a log: stream records, group
// state, the outbox, blocks, read markers, invites, which profile each stream sees.
//
//     storage.records().put("blocks", userId, R"({"created_at":1761200000000})");
//     storage.records().contains("blocks", userId);
//     for (auto &[key, bytes] : storage.records().list("streams")) ...
//
// Each call is atomic on its own: after a crash, a record is either the old document or the new one, never part of
// each. There are no transactions across calls, the same as today; a caller that needs two things to agree writes
// them so either order can be recovered from.

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace puddle::storage {

class RecordStore {
public:
    virtual ~RecordStore() = default;

    virtual void put(const std::string &collection, const std::string &key, const std::string &bytes) = 0;
    virtual std::optional<std::string> get(const std::string &collection, const std::string &key) = 0;
    virtual bool contains(const std::string &collection, const std::string &key) = 0;
    virtual bool remove(const std::string &collection, const std::string &key) = 0; // false if there was none

    virtual std::vector<std::pair<std::string, std::string>> list(const std::string &collection) = 0;
};

} // namespace puddle::storage
