/* Owner API: conversations and their messages, DMs and groups alike; the id says which (docs/objects.md).
 * /api/v1/conversations/... */
#include "api/api.h"
#include "api/request.hpp"
#include "objects/conversation.hpp"
#include "objects/user.hpp"

#include <cstring>

using namespace puddle;
using namespace puddle::api;

static Conversation conversationOf(const puddle_call *c)
{
    auto conv = Conversation::get(c->ctx, puddle_param(c, "conversation"));
    if (!conv)
        throw Error::notFound();
    return std::move(*conv);
}

static Message messageOf(const puddle_call *c)
{
    auto m = Message::get(c->ctx, puddle_param(c, "conversation"), puddle_param(c, "message"));
    if (!m)
        throw Error::notFound();
    return *m;
}

static int list_conversations(const puddle_call *c, puddle_jw *w)
{
    Page p = paging(c, 100);
    auto all = Conversation::all(c->ctx);
    PartialUsers people(c->ctx);
    beginPage(w);
    size_t i = p.offset;
    for (size_t shown = 0; i < all.size() && shown < p.limit && w->len + 2500 <= w->cap; i++, shown++)
        all[i].write(w, people);
    endPage(w, i < all.size() ? std::optional<size_t>(i) : std::nullopt);
    return 200;
}

static int get_conversation(const puddle_call *c, puddle_jw *w)
{
    PartialUsers people(c->ctx);
    conversationOf(c).write(w, people);
    return 200;
}

static int list_messages(const puddle_call *c, puddle_jw *w)
{
    Page p = paging(c, 200);
    PartialUsers people(c->ctx);
    conversationOf(c).writeMessages(w, people, p.limit, p.offset);
    return 200;
}

static int send_message(const puddle_call *c, puddle_jw *w)
{
    char me[PUDDLE_USER_ID_LEN + 1];
    puddle_identity_id(c->ctx->identity, me);
    if (!strcmp(puddle_param(c, "conversation"), me))
        throw Error::badRequest("cannot_message_yourself");
    Body b(c);
    std::string text = b.messageBody();
    std::string ref = b.optString("reply_to", 127).value_or("");
    Conversation conv = conversationOf(c);
    Message m = conv.send(text, ref);
    PartialUsers people(c->ctx);
    Deliveries d(c->ctx, conv.kind() == Conversation::Kind::Dm ? conv.id() : std::string());
    m.write(w, c->ctx, people, &d);
    return 201;
}

static int edit_message(const puddle_call *c, puddle_jw *w)
{
    Body b(c);
    std::string text = b.messageBody();
    Message m = messageOf(c).edit(c->ctx, text);
    PartialUsers people(c->ctx);
    m.write(w, c->ctx, people, nullptr);
    return 200;
}

static int delete_message(const puddle_call *c, puddle_jw *w)
{
    Message m = messageOf(c).remove(c->ctx);
    PartialUsers people(c->ctx);
    m.write(w, c->ctx, people, nullptr);
    return 200;
}

/* The owner has looked at the conversation: everything that has arrived so far is read. */
static int mark_read(const puddle_call *c, puddle_jw *w)
{
    PartialUsers people(c->ctx);
    conversationOf(c).markRead().write(w, people);
    return 200;
}

/* What is stuck goes again: what failed is queued again, and what is waiting is tried now. */
static int retry(const puddle_call *c, puddle_jw *w)
{
    PartialUsers people(c->ctx);
    conversationOf(c).retry().write(w, people);
    return 200;
}

const puddle_route puddle_conversation_routes[] = {
    { "GET",  "/api/v1/conversations",                                   PUDDLE_AUTH_OWNER, 0,    list_conversations, NULL },
    { "GET",  "/api/v1/conversations/{conversation:id}",                 PUDDLE_AUTH_OWNER, 0,    get_conversation,   NULL },
    { "GET",  "/api/v1/conversations/{conversation:id}/messages",        PUDDLE_AUTH_OWNER, 0,    list_messages,      NULL },
    { "POST", "/api/v1/conversations/{conversation:id}/messages/send",   PUDDLE_AUTH_OWNER, 8192, send_message,       NULL },
    { "POST", "/api/v1/conversations/{conversation:id}/messages/{message:event}/edit", PUDDLE_AUTH_OWNER, 8192, edit_message, NULL },
    { "POST", "/api/v1/conversations/{conversation:id}/messages/{message:event}/delete", PUDDLE_AUTH_OWNER, 64, delete_message, NULL },
    { "POST", "/api/v1/conversations/{conversation:id}/read",            PUDDLE_AUTH_OWNER, 64,   mark_read,          NULL },
    { "POST", "/api/v1/conversations/{conversation:id}/retry",           PUDDLE_AUTH_OWNER, 64,   retry,              NULL },
};
const size_t puddle_conversation_routes_count = sizeof puddle_conversation_routes / sizeof puddle_conversation_routes[0];
