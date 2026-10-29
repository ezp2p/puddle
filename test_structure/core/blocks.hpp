#pragma once

// Who the owner blocked. The pattern every part of core follows: it knows what its data means, turns it into
// bytes, and deposits them with storage, which decides where the bytes go.
//
//     engine.blocks().add(userId, now);
//     engine.blocks().contains(userId);

#include <cstdint>
#include <string>
#include <vector>

namespace puddle::storage { class RecordStore; }

namespace puddle::core {

struct Block {
    std::string user;
    int64_t createdAt = 0;
};

class Blocks {
public:
    explicit Blocks(storage::RecordStore &records) : records_(records) {}

    void add(const std::string &user, int64_t now);
    bool remove(const std::string &user);
    bool contains(const std::string &user);
    std::vector<Block> all();

private:
    storage::RecordStore &records_;
};

} // namespace puddle::core
