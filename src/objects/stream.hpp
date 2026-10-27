#pragma once

// The connection to another Puddle, as the owner manages it (test_structure/model/objects/stream.yaml).
//
//     auto s = puddle::Stream::get(ctx, bobId);
//     if (s && s->state() == PUDDLE_STREAM_PENDING_IN)
//         s->accept();
//
// Answering, closing and the rest only change what this Puddle holds and record what it still owes the other one;
// telling them is the outbox's job (flow.h).

#include "core/context.h"
#include "base/json.h"
#include "core/streams.h"

#include <optional>
#include <string>
#include <vector>

namespace puddle {

class Stream {
public:
    static std::optional<Stream> get(puddle_ctx *ctx, const std::string &id);
    // Everyone we have a stream record for, newest change first; `state` narrows it to one state.
    static std::vector<Stream> all(puddle_ctx *ctx, std::optional<puddle_stream_state> state = std::nullopt);
    // Ask the Puddle at `target` (an address or link) for a stream. `profile`: which of the owner's profiles they will see
    // (empty: none, and they see only the id).
    static Stream request(puddle_ctx *ctx, const std::string &target, const std::string &note, const std::string &profile);

    Stream(Stream &&other) noexcept;
    Stream &operator=(Stream &&other) noexcept;
    Stream(const Stream &) = delete;
    Stream &operator=(const Stream &) = delete;
    ~Stream();

    std::string id() const { return rec_.id; }
    puddle_stream_state state() const { return rec_.state; }
    const puddle_stream &record() const { return rec_; }

    Stream accept();   // a request waiting on the owner: yes
    Stream decline();  // a request waiting on the owner: no
    Stream remove();   // withdraw our request, or end the stream
    Stream trust();    // their certificate changed and the owner says the new one is fine
    Stream showProfile(const std::optional<std::string> &profileId); // nullopt: they see none of the owner's profiles

    void write(puddle_jw *w) const;

private:
    Stream(puddle_ctx *ctx, const puddle_stream &rec) : ctx_(ctx), rec_(rec) {} // takes ownership of rec's buffers
    Stream transition(unsigned allowed, puddle_stream_state to, puddle_owed owed, const char *conflict);

    puddle_ctx *ctx_;
    puddle_stream rec_;
};

} // namespace puddle
