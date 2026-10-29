#pragma once

// A driver that keeps everything in memory and forgets it on exit. For tests: an Engine on MemoryStorage needs no
// temp directory, and a test can look at exactly what was stored.

#include "storage/storage.hpp"

#include <memory>

namespace puddle::storage {

std::unique_ptr<Storage> openMemory();

} // namespace puddle::storage
