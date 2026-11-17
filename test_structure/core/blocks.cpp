// Blocks, deposited as records: collection "blocks", key = user id, document = {"created_at": ...}.
// This file knows the collection name and the document's shape. It doesn't know whether that is a file, a table
// or memory.

#include "core/blocks.hpp"

#include "core/json.hpp"
#include "storage/records.hpp"

namespace puddle::core {

namespace {
constexpr const char *kCollection = "blocks";
}

void Blocks::add(const std::string &user, int64_t now)
{
    records_.put(kCollection, user, json::object({{"created_at", now}}).dump());
}

bool Blocks::remove(const std::string &user)
{
    return records_.remove(kCollection, user);
}

bool Blocks::contains(const std::string &user)
{
    return records_.contains(kCollection, user);
}

std::vector<Block> Blocks::all()
{
    std::vector<Block> out;
    for (auto &[user, bytes] : records_.list(kCollection))
        out.push_back({user, json::parse(bytes).int64("created_at")});
    return out;
}

} // namespace puddle::core
