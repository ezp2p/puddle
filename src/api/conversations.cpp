/* Owner API: conversations and their messages, DMs and groups alike; the id says which (docs/objects.md).
 * /api/v1/conversations/... */
#include "api/api.h"
#include "api/request.hpp"
#include "objects/conversation.hpp"
#include "objects/user.hpp"

#include "core/outbox.h"

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

std::vector<puddle_file_ref> puddle::api::filesOf(puddle_span arr)
{
    std::vector<puddle_file_ref> out;
    size_t cur = 0;
    puddle_span item;
    int rc;
    while ((rc = puddle_json_array_next(arr, &cur, &item)) == 1) {
        if (out.size() == PUDDLE_MESSAGE_MAX_FILES)
            throw Error::badRequest("too_many_files", "a message carries at most " + std::to_string(PUDDLE_MESSAGE_MAX_FILES) + " files");
        puddle_file_ref f{};
        puddle_span v;
        auto str = [&](const char *key, char *dst, size_t cap, bool need) {
            int m = puddle_json_member(item.p, item.len, key, &v);
            if ((m == 1 && need) || m < 0 || (m == 0 && puddle_json_string(v, dst, cap) != 0))
                throw Error::badRequest("bad_file", std::string("files: ") + key);
        };
        auto num = [&](const char *key) -> int64_t {
            int64_t n = 0;
            int m = puddle_json_member(item.p, item.len, key, &v);
            if (m < 0 || (m == 0 && puddle_json_int64(v, &n) != 0))
                throw Error::badRequest("bad_file", std::string("files: ") + key);
            return n;
        };
        if (!puddle_json_valid_object(item.p, item.len))
            throw Error::badRequest("bad_file");
        str("id", f.id, sizeof f.id, true);
        str("name", f.name, sizeof f.name, true);
        str("mime", f.mime, sizeof f.mime, true);
        str("preview", f.preview, sizeof f.preview, false);
        int64_t fw = num("w"), fh = num("h");
        if (fw < 0 || fw > 100000 || fh < 0 || fh > 100000)
            throw Error::badRequest("bad_file", "files: w and h");
        f.w = (int)fw;
        f.h = (int)fh;
        f.duration_ms = num("duration_ms");
        out.push_back(f);
    }
    if (rc < 0)
        throw Error::badRequest("bad_file", "files must be a list");
    return out;
}

static int send_message(const puddle_call *c, puddle_jw *w)
{
    char me[PUDDLE_USER_ID_LEN + 1];
    puddle_identity_id(c->ctx->identity, me);
    if (!strcmp(puddle_param(c, "conversation"), me))
        throw Error::badRequest("cannot_message_yourself");
    Body b(c);
    std::vector<puddle_file_ref> files;
    if (b.has("files"))
        files = filesOf(b.raw("files"));
    std::string text = files.empty() || b.has("body") ? b.messageBody() : std::string(); // with files, the words may be left out
    std::string ref = b.optString("reply_to", 127).value_or("");
    Conversation conv = conversationOf(c);
    Message m = conv.send(text, ref, files);
    PartialUsers people(c->ctx);
    Deliveries d(c->ctx, conv.kind() == Conversation::Kind::Group ? std::string() : conv.id());
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

/* What a Levee gave up on (spec/06, "When the Levee gives up"; docs/levee.md, "Given up"): the given-up deliveries of the
 * message in this conversation, for each friend: `back` only those whose friend has been heard from since. */
static std::vector<uint64_t> givenUp(const puddle_call *c, const Conversation &conv, bool back)
{
    std::vector<uint64_t> out;
    if (!c->ctx->outbox)
        return out;
    puddle_ob_item *items;
    size_t n;
    if (puddle_outbox_list(c->ctx->outbox, &items, &n) != 0)
        throw Error::internal();
    std::string needle = std::string("\"id\":\"") + puddle_param(c, "message") + "\"";
    bool dm = conv.kind() == Conversation::Kind::Dm;
    for (size_t i = 0; i < n; i++) {
        const puddle_ob_item &it = items[i];
        if (it.given_up != PUDDLE_OB_GIVEN_UP || strcmp(it.kind, "events") != 0 || !it.body || !strstr(it.body, needle.c_str()) ||
            (dm && conv.id() != it.peer) || (!dm && !strstr(it.body, conv.id().c_str())) || (back && !it.back_ms))
            continue;
        out.push_back(it.seq);
    }
    puddle_outbox_list_free(items, n);
    return out;
}

static void writeMessage(const puddle_call *c, const Conversation &conv, const Message &m, puddle_jw *w)
{
    PartialUsers people(c->ctx);
    Deliveries d(c->ctx, conv.kind() == Conversation::Kind::Group ? std::string() : conv.id());
    m.write(w, c->ctx, people, &d);
}

/* Resend: the same event, as a new request, to each friend who is back */
static int resend_message(const puddle_call *c, puddle_jw *w)
{
    Conversation conv = conversationOf(c);
    Message m = messageOf(c);
    std::vector<uint64_t> all = givenUp(c, conv, false), ready = givenUp(c, conv, true);
    if (all.empty())
        throw Error::notFound("nothing_to_resend");
    if (ready.empty())
        throw Error(409, "not_back", "they haven't been heard from since; it can go again once they are");
    for (uint64_t seq : ready)
        puddle_outbox_requeue(c->ctx->outbox, seq);
    writeMessage(c, conv, m, w);
    return 200;
}

/* Delete it: in a DM gone from the owner's chat too; in a group, where others have it, only the mark for those friends goes */
static int forget_message(const puddle_call *c, puddle_jw *w)
{
    Conversation conv = conversationOf(c);
    Message m = messageOf(c);
    bool dm = conv.kind() == Conversation::Kind::Dm;
    std::vector<uint64_t> seqs = givenUp(c, conv, !dm);
    if (seqs.empty())
        throw Error::notFound("nothing_to_delete");
    for (uint64_t seq : seqs)
        puddle_outbox_dismiss(c->ctx->outbox, seq);
    if (dm)
        m = m.remove(c->ctx);
    writeMessage(c, conv, m, w);
    return 200;
}

/* Through a Pond: offer to be friends (docs/pond-dms.md, "Becoming friends"). */
static int befriend(const puddle_call *c, puddle_jw *w)
{
    PartialUsers people(c->ctx);
    conversationOf(c).befriend().write(w, people);
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
    { "POST", "/api/v1/conversations/{conversation:id}/messages/send",   PUDDLE_AUTH_OWNER, 65536, send_message,      NULL },
    { "POST", "/api/v1/conversations/{conversation:id}/messages/{message:event}/edit", PUDDLE_AUTH_OWNER, 8192, edit_message, NULL },
    { "POST", "/api/v1/conversations/{conversation:id}/messages/{message:event}/delete", PUDDLE_AUTH_OWNER, 64, delete_message, NULL },
    { "POST", "/api/v1/conversations/{conversation:id}/messages/{message:event}/resend", PUDDLE_AUTH_OWNER, 64, resend_message, NULL },
    { "POST", "/api/v1/conversations/{conversation:id}/messages/{message:event}/forget", PUDDLE_AUTH_OWNER, 64, forget_message, NULL },
    { "POST", "/api/v1/conversations/{conversation:id}/read",            PUDDLE_AUTH_OWNER, 64,   mark_read,          NULL },
    { "POST", "/api/v1/conversations/{conversation:id}/retry",           PUDDLE_AUTH_OWNER, 64,   retry,              NULL },
    { "POST", "/api/v1/conversations/{conversation:id}/befriend",        PUDDLE_AUTH_OWNER, 64,   befriend,           NULL },
};
const size_t puddle_conversation_routes_count = sizeof puddle_conversation_routes / sizeof puddle_conversation_routes[0];
