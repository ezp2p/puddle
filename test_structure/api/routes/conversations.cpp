// Owner API: /api/v1/conversations/... DMs and groups alike; the id says which.

#include "http/router.hpp"
#include "objects/conversation.hpp"

#include "api/json/json.hpp"

namespace puddle::api {

namespace {

// POST /api/v1/conversations/{conversation}/messages/send   { "body": "...", "reply_to": "..." }
Response send(Call &call)
{
    auto chat = Conversation::get(call.puddle(), call.param("conversation"));
    if (!chat)
        return Response::notFound();
    auto in = call.json();
    // An Error the object throws ("no_stream", "blocked") becomes {"error": code} with the status the
    // router maps it to (see api/src/errors.cpp).
    return Response::ok(json::message(chat->send(in.string("body"), in.optString("reply_to"))));
}

// POST /api/v1/conversations/{conversation}/messages/{message}/edit   { "body": "..." }
Response edit(Call &call)
{
    auto msg = Message::get(call.puddle(), call.param("conversation"), call.param("message"));
    if (!msg)
        return Response::notFound();
    return Response::ok(json::message(msg->edit(call.json().string("body"))));
}

} // namespace

void addConversationRoutes(Router &r)
{
    r.get("/api/v1/conversations", Auth::Owner, /* list */ nullptr);
    r.get("/api/v1/conversations/{conversation:ChatId}", Auth::Owner, /* get */ nullptr);
    r.get("/api/v1/conversations/{conversation:ChatId}/messages", Auth::Owner, /* page */ nullptr);
    r.post("/api/v1/conversations/{conversation:ChatId}/messages/send", Auth::Owner, send, 16 * 1024);
    r.post("/api/v1/conversations/{conversation:ChatId}/messages/{message:EventId}/edit", Auth::Owner, edit, 16 * 1024);
    // .../messages/{message}/delete, .../read, .../retry
}

} // namespace puddle::api
