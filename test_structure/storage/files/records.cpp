// RecordStore for the files driver: records/<collection>/<key>.json, replaced atomically.

#include "storage/records.hpp"

#include "platform/fs.hpp"

#include <filesystem>

namespace puddle::storage {

namespace {

class FileRecords final : public RecordStore {
public:
    explicit FileRecords(std::filesystem::path root) : root_(std::move(root)) {}

    void put(const std::string &collection, const std::string &key, const std::string &bytes) override
    {
        auto dir = root_ / checked(collection);
        std::filesystem::create_directories(dir);
        platform::atomicWrite(dir / (checked(key) + ".json"), bytes); // temp file, flush, rename over the old one
    }

    std::optional<std::string> get(const std::string &collection, const std::string &key) override
    {
        return platform::readFile(root_ / checked(collection) / (checked(key) + ".json"));
    }

    bool contains(const std::string &collection, const std::string &key) override
    {
        return std::filesystem::exists(root_ / checked(collection) / (checked(key) + ".json"));
    }

    bool remove(const std::string &collection, const std::string &key) override
    {
        return platform::removeFile(root_ / checked(collection) / (checked(key) + ".json"));
    }

    std::vector<std::pair<std::string, std::string>> list(const std::string &collection) override
    {
        std::vector<std::pair<std::string, std::string>> out;
        auto dir = root_ / checked(collection);
        if (!std::filesystem::exists(dir))
            return out;
        for (auto &f : std::filesystem::directory_iterator(dir))
            if (f.path().extension() == ".json")
                if (auto bytes = platform::readFile(f.path()))
                    out.emplace_back(f.path().stem().string(), std::move(*bytes));
        return out;
    }

private:
    // Names become file names, so only letters, digits, '-' and '_' are allowed: no "..", no separators.
    static const std::string &checked(const std::string &name);

    std::filesystem::path root_;
};

} // namespace

std::unique_ptr<RecordStore> fileRecords(const std::filesystem::path &dataDir)
{
    return std::make_unique<FileRecords>(dataDir / "records");
}

} // namespace puddle::storage
