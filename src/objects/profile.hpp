#pragma once

// A profile: how someone appears (docs/profiles.md, test_structure/model/objects/profile.yaml). Either one of the owner's,
// which they edit and choose where to show, or one someone else shows us in a place we share.
//
//     auto work = puddle::Profile::get(ctx, "work");       // one of the owner's; nullopt if there is none
//     work = work->edit(changes, len);                     // and every place that shows it hears of it
//     auto theirs = bob->profile();                        // what Bob shows us directly; nullopt if he shows us nothing
//
// The owner's profiles never leave this Puddle except where the owner chose one (a direct conversation, a group): that
// choice is the consent. Another Puddle's profile was cleaned when it arrived; `notes()` says what had to be left out.

#include "base/json.h"
#include "core/context.h"

#include <optional>
#include <string>
#include <vector>

namespace puddle {

class Profile {
public:
    // ---- the owner's ----
    static std::vector<Profile> all(puddle_ctx *ctx); // by id
    static std::optional<Profile> get(puddle_ctx *ctx, const std::string &id);
    // A new profile `id` from a JSON object of its fields (ProfileFields). Error (400) on anything wrong with them, and
    // (409) if the id is taken or there are as many profiles as there can be. It is shown nowhere until chosen somewhere.
    static Profile create(puddle_ctx *ctx, const std::string &id, const char *fields, size_t len);
    // Change the fields `changes` names (a field given as null is cleared); every place showing it hears of it.
    Profile edit(const char *changes, size_t len) const;
    // Delete it. Every place it was shown sees nothing of the owner there any more, and is told so.
    void remove() const;
    std::vector<std::string> places() const; // where it is shown: "dm:<user>", "group:<group>"

    // Show `profileId` at `place` ("dm:<user>" or "group:<group>"), or nothing there (nullopt), and tell whoever is there.
    // Error 400 `unknown_profile` if there is no such profile.
    static void showAt(puddle_ctx *ctx, const std::string &place, const std::optional<std::string> &profileId);
    // The id of the profile shown at `place`, if one is.
    static std::optional<std::string> shownAt(puddle_ctx *ctx, const std::string &place);

    // Keep a picture the owner gave us (its bytes) for use in their profiles: {"hash","mime","size"}. Error 400 if it is
    // not a png, jpg, gif or webp image of 1 byte to the size limit.
    static void addPicture(puddle_ctx *ctx, const unsigned char *data, size_t len, puddle_jw *w);

    // ---- someone else's ----
    // What `userId` shows us at `place`: "dm" (directly) or "group:<group>".
    static std::optional<Profile> of(puddle_ctx *ctx, const std::string &userId, const std::string &place);
    static std::vector<Profile> allOf(puddle_ctx *ctx, const std::string &userId); // every place, in the order they came

    bool isOwn() const { return own_; }
    const std::string &id() const { return id_; }       // the owner's: its private id ("" for someone else's)
    const std::string &place() const { return place_; } // someone else's: where they show it ("" for the owner's)
    const std::string &fieldsJson() const { return fields_; } // the fields, as the JSON object they travel as
    std::string displayName() const;
    int64_t receivedAt() const { return at_; }
    const std::string &notes() const { return notes_; } // what was left out when it came, one line each; "" if nothing

    // Profile: the fields, and the owner's `id` and `places`, or someone else's `place`, `received_at` and `notes`.
    void write(puddle_jw *w) const;

private:
    Profile(puddle_ctx *ctx) : ctx_(ctx) {}

    puddle_ctx *ctx_;
    bool own_ = false;
    std::string id_, place_, fields_, notes_;
    int64_t at_ = 0;
};

} // namespace puddle
