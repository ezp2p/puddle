#pragma once

// The default driver: plain files in the data directory, the layout Puddle uses today (docs/storage.md).
//
//   <data_dir>/
//     lock                                 held while open (platform/lock.hpp)
//     chats/<log>/<YYYY-MM-DD>.jsonl       logs: one file per log per UTC day, one entry per line
//     records/<collection>/<key>.json      records: one file each, replaced atomically (platform/fs.hpp)
//     media/<sha256>                       blobs
//     secrets/<name>                       secrets, owner-only
//
// Everything that touches the disk goes through platform/fs.hpp, so this driver has no OS-specific code either.
// The index that makes get() by id fast is kept in memory and rebuilt from the logs when the driver opens.

#include "storage/storage.hpp"

#include <filesystem>
#include <memory>

namespace puddle::storage {

std::unique_ptr<Storage> openFiles(const std::filesystem::path &dataDir);

} // namespace puddle::storage
