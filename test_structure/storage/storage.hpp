#pragma once

// Where a Puddle keeps things, without saying how. Like PDO: one interface, a driver behind it.
//
// The data comes in four kinds, and each kind has a store with the handful of operations it needs:
//
//   logs     append-only, ordered, deduplicated by id    chat events            storage/log.hpp
//   records  a document per key, in named collections    streams, groups, outbox, blocks, read markers, invites
//                                                                                storage/records.hpp
//   blobs    bytes named by their own hash               media                  storage/blobs.hpp
//   secrets  small, owner-only                           keys, the owner token  storage/secrets.hpp
//
// Storage handles bytes only. It never knows what a block or a stream is: core/ turns those into bytes and back.
// The owner's own files (puddle.yaml, profiles.yaml, theme.yaml) are not storage. The owner edits them, so they
// stay plain YAML where the owner put them, and core reads them as config.
//
//     auto storage = storage::open(config);          // the driver config names: files (default), memory, ...
//     core::Engine engine(config, *storage);         // nothing above core sees `storage` again

#include "storage/blobs.hpp"
#include "storage/log.hpp"
#include "storage/records.hpp"
#include "storage/secrets.hpp"

#include <filesystem>
#include <memory>
#include <string>

namespace puddle::storage {

class Storage {
public:
    virtual ~Storage() = default;

    virtual LogStore &logs() = 0;
    virtual RecordStore &records() = 0;
    virtual BlobStore &blobs() = 0;
    virtual SecretStore &secrets() = 0;
};

struct Options {
    std::string driver = "files";   // `storage:` in puddle.yaml
    std::filesystem::path dataDir;  // for drivers that use one
};

// Open the driver `options` names. A driver that keeps its data on disk also takes the data directory's lock, so
// two Puddles never share one. Throws if the driver is unknown or can't open.
std::unique_ptr<Storage> open(const Options &options);

} // namespace puddle::storage
