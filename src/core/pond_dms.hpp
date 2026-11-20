#pragma once

// Direct messages through a Pond (docs/pond-dms.md): what this Puddle keeps of them, apart from the messages themselves, which
// are in the chat logs like any DM's. Kept in <data_dir>/state/pond_dms/:
//
//   conversations/<chat>.json   one per conversation: the Pond, the other member, the Pond's name (so it is still named after
//                               we leave), a friend offer they made us, and whether we made them one
//   out/<letter>.json           letters waiting to go, or gone and kept until their files have been fetched: the sender's
//                               Puddle holds a letter while the other Puddle is away, up to 30 days (the Pond keeps nothing)
//   in/<letter>.json            files a letter that arrived carries, still to be fetched through the Pond
//   blocks.tsv                  people whose direct messages are dropped unread, a user id and a time per line
//
// Network work is federation/pond_dms.cpp's; this only keeps. Safe to call from several threads.

#include "core/context.h"
#include "core/identity.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace puddle::pdm {

constexpr int64_t kKeepMs = 30LL * 24 * 3600 * 1000; // a letter is tried this long; a gone one's files can be fetched this long

// The chat (and conversation) id of the direct messages between `a` and `b` through `pond`: base32(SHA-256("pdm" || pond ||
// the two ids, sorted)), worked out alike on both sides. "" if any is not an id.
std::string chatId(const std::string &pond, const std::string &a, const std::string &b);

struct Conv {
    std::string chat, pond, member, pondName;
    std::string offer;    // their friend offer: the puddle:// link they sent, "" for none
    bool offered = false; // we sent them one
    int64_t created = 0;
};

// A letter of ours. `kind` "message" (a chat event: a message, an edit or a deletion), "offer" (a friend offer) or "receipt".
struct Letter {
    std::string id, chat, pond, to, kind;
    std::string event, eventSig; // a message's event as it was signed, and its signature
    std::string link;            // an offer's link
    std::vector<std::string> ids; // a receipt's: the letters it says arrived
    struct File {
        std::string id, key; // the file's id, and the key its pieces are sealed with (base64, 32 bytes)
    };
    std::vector<File> files;
    int64_t created = 0, next = 0;
    int attempts = 0;
    enum State { Waiting = 0, Delivered = 1, Failed = 2 } state = Waiting;
    std::string reason;
};

// A file still to fetch, from a letter that arrived.
struct Fetch {
    std::string letter, pond, from, chat, message;
    struct File {
        int n = 0;
        std::string id, key;
        int64_t size = 0;
        bool done = false;
    };
    std::vector<File> files;
    int64_t created = 0, next = 0;
    int attempts = 0;
};

class Store {
public:
    // NULL-like (empty) on failure, with a reason in `err`.
    static std::unique_ptr<Store> open(const std::string &dataDir, std::string &err);

    std::optional<Conv> conv(const std::string &chat);
    std::optional<Conv> convWith(const std::string &pond, const std::string &member);
    std::vector<Conv> convs();
    bool put(const Conv &c);

    bool put(const Letter &l);
    std::optional<Letter> letter(const std::string &id);
    std::vector<Letter> letters();
    bool removeLetter(const std::string &id);

    bool put(const Fetch &f);
    std::vector<Fetch> fetches();
    bool removeFetch(const std::string &letter);

    // blocking someone's direct messages (Puddle's own block is the streams'): 0, 1 already (or not, for unblock), -1
    int block(const std::string &id, int64_t now);
    int unblock(const std::string &id);
    bool blocked(const std::string &id);
    std::vector<std::pair<std::string, int64_t>> blocks();

private:
    explicit Store(std::string dir) : dir_(std::move(dir)) {}
    bool saveBlocks();
    std::string dir_;
    std::mutex mu_;
    std::vector<std::pair<std::string, int64_t>> blocks_;
};

// The context's Store (ctx->pond_dms), or nullptr when there is none (tests that keep none).
inline Store *of(const puddle_ctx *ctx)
{
    return static_cast<Store *>(ctx->pond_dms);
}

} // namespace puddle::pdm
