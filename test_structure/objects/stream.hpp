#pragma once

// The connection between this Puddle and another one: asked for, waiting, active, closed, or group-only.
//
//     auto s = Stream::request(puddle, "home.example.org", "it's Bob from climbing");
//     ... later, on their side:
//     Stream::get(puddle, bobId)->accept();

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace puddle {

class Puddle;
class User;

enum class StreamState { PendingOut, PendingIn, Active, Closed, Scoped };

class Stream {
public:
    static std::optional<Stream> get(Puddle &puddle, const std::string &userId);
    static std::vector<Stream> all(Puddle &puddle);
    static Stream request(Puddle &puddle, const std::string &address, const std::string &note = {},
                          const std::string &profile = {});

    const std::string &id() const { return id_; } // the peer's user id
    User user() const;
    StreamState state() const { return state_; }
    const std::string &address() const { return address_; }
    bool certificateChanged() const { return certChanged_; }
    const std::string &profileShown() const { return profile_; } // which of our profiles they see

    Stream accept();
    Stream decline();
    Stream remove();
    Stream trust(); // accept their changed certificate
    Stream showProfile(const std::string &profileId);

private:
    Puddle *puddle_;
    std::string id_, address_, profile_;
    StreamState state_ = StreamState::PendingOut;
    bool certChanged_ = false;
};

} // namespace puddle
