#include "objects/user.hpp"

#include "objects/conversation.hpp"
#include "objects/message.hpp"
#include "objects/puddle.hpp"

namespace puddle {

std::optional<User> User::get(Puddle &puddle, const std::string &id)
{
    if (id == puddle.identity().userId())
        return me(puddle);

    auto person = puddle.people().get(id); // the profile their Puddle sent us, kept apart from any stream
    if (!person)
        return std::nullopt; // nobody we know
    User u(puddle, id);
    if (person->profile)
        u.profile_ = Profile::fromJson(*person->profile);
    if (auto rec = puddle.streams().find(id))
        u.stream_ = rec->state;
    u.blocked_ = puddle.blocks().contains(id);
    return u;
}

User User::me(Puddle &puddle)
{
    User u(puddle, puddle.identity().userId());
    u.profile_ = Profile::defaultOwn(puddle);
    u.me_ = true;
    return u;
}

std::string User::displayName() const
{
    if (profile_)
        return profile_->displayName();
    return id_.substr(0, 8);
}

std::optional<Stream> User::stream() const
{
    return Stream::get(*puddle_, id_);
}

Conversation User::conversation() const
{
    return Conversation::dm(*puddle_, id_);
}

Message User::message(const std::string &body, const std::string &replyTo)
{
    if (stream_ != StreamState::Active)
        throw Error("no_stream");
    if (blocked_)
        throw Error("blocked");
    // The Conversation signs the event, stores it in the chat log and queues it in the outbox.
    return conversation().send(body, replyTo);
}

User User::block()
{
    puddle_->blocks().add(id_, puddle_->now());
    puddle_->outbox().dropFor(id_); // nothing more goes to them
    return *get(*puddle_, id_);
}

User User::unblock()
{
    puddle_->blocks().remove(id_);
    if (auto u = get(*puddle_, id_))
        return *u;
    throw Error("not_found"); // only blocked, and now not even that
}

} // namespace puddle
