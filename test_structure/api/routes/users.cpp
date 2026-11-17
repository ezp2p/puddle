// Owner API: /api/v1/users/...
// Each handler gets an object, asks it to do something, and returns it as JSON. No storage here.

#include "http/router.hpp"
#include "objects/user.hpp"

#include "api/json/json.hpp"

namespace puddle::api {

namespace {

// GET /api/v1/users/me
Response me(Call &call)
{
    return Response::ok(json::self(User::me(call.puddle()), call.puddle().config().link()));
}

// GET /api/v1/users/{user}
Response getUser(Call &call)
{
    auto user = User::get(call.puddle(), call.param("user"));
    if (!user)
        return Response::notFound();
    return Response::ok(json::user(*user));
}

// POST /api/v1/users/{user}/block
Response block(Call &call)
{
    auto user = User::get(call.puddle(), call.param("user"));
    if (!user)
        return Response::notFound();
    return Response::ok(json::user(user->block()));
}

// POST /api/v1/users/{user}/unblock
Response unblock(Call &call)
{
    auto user = User::get(call.puddle(), call.param("user"));
    if (!user || !user->blocked())
        return Response::notFound();
    return Response::ok(json::user(user->unblock()));
}

} // namespace

// `{user:UserId}` only matches a well-formed user id, so "me" never reaches getUser, and a malformed id is a 404
// before any handler runs.
void addUserRoutes(Router &r)
{
    r.get("/api/v1/users/me", Auth::Owner, me);
    r.get("/api/v1/users/{user:UserId}", Auth::Owner, getUser);
    r.post("/api/v1/users/{user:UserId}/block", Auth::Owner, block);
    r.post("/api/v1/users/{user:UserId}/unblock", Auth::Owner, unblock);
}

} // namespace puddle::api
