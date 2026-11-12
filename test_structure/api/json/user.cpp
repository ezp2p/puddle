// User, PartialUser and SelfUser as the owner API sends them (model/objects/user.yaml, partial_user.yaml, self_user.yaml). The only place their fields
// are written; routes and events both call these.

#include "json.hpp"

#include "objects/user.hpp"

namespace puddle::api::json {

Value partialUser(const User &u)
{
    Value v = Value::object();
    v["id"] = u.id();
    v["display_name"] = u.displayName();
    if (u.profile()) {
        if (auto &a = u.profile()->avatar())
            v["avatar"] = mediaRef(*a);
        if (auto &c = u.profile()->color())
            v["color"] = color(*c);
    }
    return v;
}

Value user(const User &u)
{
    Value v = Value::object();
    v["id"] = u.id();
    if (u.profile())
        v["profile"] = profile(*u.profile());
    if (u.streamState())
        v["stream"] = streamState(*u.streamState());
    if (u.blocked())
        v["blocked"] = true;
    return v;
}

Value self(const User &me, const std::optional<std::string> &link)
{
    Value v = user(me);
    v["link"] = link ? Value(*link) : Value::null();
    return v;
}

} // namespace puddle::api::json
