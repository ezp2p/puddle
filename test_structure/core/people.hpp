#pragma once

// Everyone this Puddle knows of, one record each: the profile they show us, as their Puddle sent it.
//
// Profiles already arrive over federation: in an access request, in the answer to one, and in
// POST /stream/v1/profile whenever they change it. Today each copy is written into the stream record
// (puddle_stream.profile), so someone without a stream, such as a group member we never connected to, has nowhere to
// keep one. Here the person is the record, and the stream is only one thing we may have with them.
//
//     engine.people().receivedProfile(userId, profileJson, now);   // federation, whenever one arrives
//     auto p = engine.people().get(userId);                        // objects::User::get reads this
//
// Records "people", key = user id (model/records/person.yaml).

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace puddle::storage { class RecordStore; }

namespace puddle::core {

struct Person {
    std::string id;
    std::optional<std::string> profile; // the Profile JSON they sent us (model/wire/profile.yaml), exactly as received
    int64_t profileAt = 0;              // when we received it
    int64_t firstSeen = 0;              // when we first heard of them: a request, an answer, or a group
};

class People {
public:
    explicit People(storage::RecordStore &records) : records_(records) {}

    std::optional<Person> get(const std::string &id);
    std::vector<Person> all();

    // A profile from their Puddle replaces the one we held. `profileJson` is checked by wire::readProfile first.
    void receivedProfile(const std::string &id, const std::string &profileJson, int64_t now);
    // Someone we have heard of with no profile yet (a request we sent, a group member named in an invite).
    void noted(const std::string &id, int64_t now);
    // Forget them: nothing else refers to them any more (no stream, no group, not blocked).
    bool forget(const std::string &id);

private:
    storage::RecordStore &records_;
};

} // namespace puddle::core
