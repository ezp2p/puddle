#pragma once

// Secrets: small values only this Puddle may read. The signing keys, the roll key, the owner token.
// Kept apart from records so a driver can protect them differently: the files driver writes them owner-only
// (0600, or an owner-only ACL on Windows).
//
//     storage.secrets().put("owner-token", token);

#include <optional>
#include <string>

namespace puddle::storage {

class SecretStore {
public:
    virtual ~SecretStore() = default;

    virtual void put(const std::string &name, const std::string &bytes) = 0;
    virtual std::optional<std::string> get(const std::string &name) = 0;
    virtual bool remove(const std::string &name) = 0;
};

} // namespace puddle::storage
