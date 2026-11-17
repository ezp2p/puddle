#pragma once

// A person as this Puddle knows them: a friend, someone asking for a stream, someone we share a group with, someone
// we blocked, or the owner (docs/objects.md, test_structure/model/objects/user.yaml).
//
//     auto bob = puddle::User::get(ctx, bobId);   // nullopt if we know nothing of them
//     if (bob)
//         bob->block();
//
// A User is a snapshot of one moment: get() reads it, and an action returns the User as it is afterwards.

#include "core/context.h"
#include "base/json.h"
#include "core/streams.h"
#include "objects/profile.hpp"

#include <map>
#include <optional>
#include <string>

namespace puddle {

class User {
public:
    static std::optional<User> get(puddle_ctx *ctx, const std::string &id);
    static User me(puddle_ctx *ctx);

    const std::string &id() const { return id_; }
    bool isMe() const { return me_; }
    bool blocked() const { return blocked_; }
    std::optional<puddle_stream_state> streamState() const { return stream_; }
    const std::string &profileJson() const { return profile_; } // what they show us directly, as cleaned; "" if nothing
    // What they show us directly ("dm"), or in a place we share ("group:<group>"); nullopt if nothing. For the owner, the
    // profile shown there (directly: their first profile, as the app shows them to themselves).
    std::optional<Profile> profile() const { return profileIn("dm"); }
    std::optional<Profile> profileIn(const std::string &place) const;
    std::string displayName() const;

    User block();
    User unblock();

    void write(puddle_jw *w) const;        // User
    void writeSelf(puddle_jw *w) const;    // SelfUser: the owner, with the profile the app shows them as, and their link

    // The short form of `id` (PartialUser), for whoever it is, known or not.
    static void writePartial(puddle_jw *w, puddle_ctx *ctx, const std::string &id);

    // Built from a stream record already in hand, without reading it again.
    static User fromStream(puddle_ctx *ctx, const puddle_stream &st);

    // What someone is called when they have shown us no profile: the start of their id.
    static std::string fallbackName(const std::string &id) { return id.substr(0, 8); }

private:
    User(puddle_ctx *ctx, std::string id) : ctx_(ctx), id_(std::move(id)) {}

    puddle_ctx *ctx_;
    std::string id_;
    std::string profile_;
    std::optional<puddle_stream_state> stream_;
    bool blocked_ = false;
    bool me_ = false;
};

// PartialUsers for one answer: each person is looked up once however many messages or members name them. A person is
// shown as they show themselves in the place being written: directly ("dm", the default) or in a group
// ("group:<group>"); a group's members are written inside a `PartialUsers::In`.
class PartialUsers {
public:
    explicit PartialUsers(puddle_ctx *ctx) : ctx_(ctx) {}
    void write(puddle_jw *w, const std::string &id);
    std::string name(const std::string &id);

    // While it lives, people are shown as they are in `place`.
    class In {
    public:
        In(PartialUsers &p, std::string place) : p_(p), was_(std::move(p.place_)) { p_.place_ = std::move(place); }
        ~In() { p_.place_ = std::move(was_); }
        In(const In &) = delete;
        In &operator=(const In &) = delete;

    private:
        PartialUsers &p_;
        std::string was_;
    };

private:
    struct Entry {
        std::string name, avatar, color; // avatar and color as raw JSON, "" if none
    };
    const Entry &lookup(const std::string &id);

    puddle_ctx *ctx_;
    std::string place_ = "dm";
    std::map<std::string, Entry> cache_; // by place and id
};

} // namespace puddle
