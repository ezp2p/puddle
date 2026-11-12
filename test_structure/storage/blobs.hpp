#pragma once

// Blobs: bytes named by their SHA-256. Profile pictures, banners, backgrounds. The same bytes are stored once
// however many profiles use them.
//
//     std::string hash = storage.blobs().put(pngBytes);
//     auto bytes = storage.blobs().get(hash);

#include <optional>
#include <string>

namespace puddle::storage {

class BlobStore {
public:
    virtual ~BlobStore() = default;

    virtual std::string put(const std::string &bytes) = 0; // returns the hex SHA-256
    virtual std::optional<std::string> get(const std::string &hash) = 0;
    virtual bool contains(const std::string &hash) = 0;
    virtual bool remove(const std::string &hash) = 0;
};

} // namespace puddle::storage
