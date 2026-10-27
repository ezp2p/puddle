#pragma once

// A file sent in a chat (docs/files.md): what the message that carries it says of it (name, type, size, a preview), and
// how much of it is here. Its bytes are kept by id in storage/files.h; one id is one file wherever it came from.
//
//     auto up = puddle::Upload::create(ctx, size);    // the owner sends one: a chunk at a time, then
//     up.put(0, bytes, len); ...
//     puddle::File f = up.finish();                    // its id, and it is here whole
//     conversation.send("look", "", { ref });          // a message carries it

#include "events/message.h"
#include "core/context.h"
#include "base/json.h"

#include <optional>
#include <string>

namespace puddle {

class File {
public:
    enum class State { Available, Downloading, Waiting, TooLarge, Failed };

    // As a message describes it, with what is here of it.
    static File of(puddle_ctx *ctx, const puddle_file_ref &ref);
    // Only what is here: the id, its size and how much has arrived; nullopt if nothing of it is here.
    static std::optional<File> here(puddle_ctx *ctx, const std::string &id);

    const std::string &id() const { return id_; }
    int64_t size() const { return ref_.size; }
    State state() const { return state_; }

    void write(puddle_jw *w) const;

private:
    File() = default;
    std::string id_;
    puddle_file_ref ref_{};
    bool described_ = false; // the name, type and preview are known (from a message)
    State state_ = State::Waiting;
    int64_t have_ = 0;
    std::string reason_; // why a download is waiting or failed
};

// A file the owner is sending us, a chunk at a time, before it has an id.
class Upload {
public:
    // Error 400 if `size` is not 1 byte to files.max_size.
    static Upload create(puddle_ctx *ctx, int64_t size);
    static std::optional<Upload> get(puddle_ctx *ctx, const std::string &token);

    const std::string &token() const { return token_; }

    Upload put(uint32_t n, const unsigned char *data, size_t len) const; // 400 if chunk n is not that long, or there is none
    File finish() const;                                                 // 409 while chunks are missing
    void cancel() const;

    void write(puddle_jw *w) const;

private:
    Upload() = default;
    puddle_ctx *ctx_ = nullptr;
    std::string token_;
    int64_t size_ = 0;
    uint32_t have_ = 0, count_ = 0;
};

} // namespace puddle
