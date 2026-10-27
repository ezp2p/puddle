#include "objects/message.hpp"

#include "core/error.hpp"
#include "core/identity.h"
#include "base/log.h"
#include "objects/user.hpp"
#include "core/streams.h"
#include "core/group.h"
#include "events/group_events.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace puddle {

namespace {

std::string ownId(puddle_ctx *ctx)
{
    char id[PUDDLE_USER_ID_LEN + 1];
    puddle_identity_id(ctx->identity, id);
    return id;
}

bool isGroup(puddle_ctx *ctx, const std::string &id)
{
    puddle_group g;
    if (!ctx->groups || puddle_groups_get(ctx->groups, id.c_str(), &g) != 0)
        return false;
    puddle_group_free(&g);
    return true;
}

// a group we are a member of, or Error
void requireMember(puddle_ctx *ctx, const std::string &gid)
{
    puddle_group g;
    if (!ctx->groups || puddle_groups_get(ctx->groups, gid.c_str(), &g) != 0)
        throw Error::notFound();
    bool in = g.my == PUDDLE_GROUP_IN;
    puddle_group_free(&g);
    if (!in)
        throw Error::conflict("not_in_group");
}

// an open, unblocked stream with `to`, or Error
void requireOpen(puddle_ctx *ctx, const std::string &to)
{
    puddle_stream st;
    int have = puddle_streams_get(ctx->streams, to.c_str(), &st);
    bool active = have == 0 && st.state == PUDDLE_STREAM_ACTIVE;
    if (have == 0)
        puddle_stream_free(&st);
    if (!active || puddle_streams_is_blocked(ctx->streams, to.c_str()))
        throw Error(403, "no_stream");
}

int deliveryRank(const puddle_ob_item *it)
{
    return it->state == PUDDLE_OB_FAILED ? 3 : it->state == PUDDLE_OB_HELD ? 2 : it->attempts ? 1 : 0;
}

const char *typeName(Message::Type t)
{
    switch (t) {
    case Message::Type::GroupCreate: return "group_create";
    case Message::Type::GroupRename: return "group_rename";
    case Message::Type::GroupInvite: return "group_invite";
    case Message::Type::GroupJoin: return "group_join";
    case Message::Type::GroupLeave: return "group_leave";
    default: return "default";
    }
}

// send an event of ours into a conversation: a DM's goes to the other person, a group's to its members
void sendEvent(puddle_ctx *ctx, bool group, const std::string &conversation, const std::string &chat, const char *event, size_t len,
               char id[PUDDLE_ID_HEX + 1])
{
    if (!group) {
        commitDirectEvent(ctx, conversation, chat, event, len, id);
        return;
    }
    const char *code = nullptr;
    int sent;
    int st = ctx->courier->group_send_event(ctx, chat.c_str(), event, len, id, &sent, &code);
    if (st == 0)
        return;
    if (code && !strcmp(code, "not_queued"))
        throw Error(500, "not_queued", "it was saved here but could not be queued to send");
    throw Error(st, code ? code : "internal");
}

} // namespace

// ---- Deliveries ----

Deliveries::Deliveries(puddle_ctx *ctx, const std::string &peer) : peer_(peer)
{
    if (ctx->outbox && puddle_outbox_list(ctx->outbox, &items_, &n_) != 0) {
        items_ = nullptr;
        n_ = 0;
    }
}

Deliveries::~Deliveries()
{
    if (items_)
        puddle_outbox_list_free(items_, n_);
}

// the delivery of the message `id`: the envelope's own "id" member, not the id of something that mentions it (an edit)
const puddle_ob_item *Deliveries::of(const std::string &messageId) const
{
    char needle[PUDDLE_ID_HEX + 16];
    snprintf(needle, sizeof needle, "\"id\":\"%s\"", messageId.c_str());
    const puddle_ob_item *worst = nullptr;
    for (size_t i = 0; i < n_; i++) {
        const puddle_ob_item *it = &items_[i];
        if (strcmp(it->kind, "events") != 0 || (!peer_.empty() && peer_ != it->peer) || !it->body || !strstr(it->body, needle))
            continue;
        if (!worst || deliveryRank(it) > deliveryRank(worst))
            worst = it;
    }
    return worst;
}

// ---- finding and sending ----

bool findInChat(puddle_ctx *ctx, const std::string &chat, const std::string &target, puddle_view_msg *out)
{
    // the newest events of the chat, widened until they reach the message (everything done to it is newer)
    size_t n = 128;
    for (;;) {
        puddle_event_list tail;
        char err[200];
        if (puddle_store_tail(ctx->store, chat.c_str(), n, &tail, err, sizeof err) != 0)
            throw Error::internal(err);
        puddle_view_msg *v;
        size_t m;
        int rc = puddle_chat_view(tail.items, tail.count, &v, &m);
        bool full = tail.count >= n;
        puddle_event_list_free(&tail);
        if (rc != 0)
            throw Error::internal();
        for (size_t i = 0; i < m; i++)
            if (target == v[i].id) {
                *out = v[i];
                v[i].body = NULL;
                puddle_view_free(v, m);
                return true;
            }
        puddle_view_free(v, m);
        if (!full || n > (1u << 22))
            return false;
        n *= 8;
    }
}

void commitDirectEvent(puddle_ctx *ctx, const std::string &to, const std::string &chat, const char *event, size_t el,
                       char id[PUDDLE_ID_HEX + 1])
{
    char sig[PUDDLE_SIG_TEXT_MAX], err[300];
    if (puddle_event_sign(ctx->identity, event, el, sig) != 0)
        throw Error::internal();
    if (puddle_store_append(ctx->store, chat.c_str(), event, el, sig, 0, id, err, sizeof err) < 0)
        throw Error::internal(err);

    // the delivery: {"chat":..,"events":[{"id","event","sig"}]}
    std::vector<char> body(el * 6 + 1024);
    puddle_jw d;
    puddle_jw_init(&d, body.data(), body.size());
    puddle_jw_object(&d);
    puddle_jw_str(&d, "chat", chat.c_str());
    puddle_jw_key(&d, "events");
    puddle_jw_array(&d);
    puddle_jw_object(&d);
    puddle_jw_str(&d, "id", id);
    puddle_jw_key(&d, "event");
    puddle_jw_string_n(&d, event, el);
    puddle_jw_str(&d, "sig", sig);
    puddle_jw_end_object(&d);
    puddle_jw_end_array(&d);
    puddle_jw_end_object(&d);
    size_t dl = puddle_jw_finish(&d);
    bool queued = dl && ctx->outbox &&
                  puddle_outbox_enqueue(ctx->outbox, to.c_str(), "events", "/stream/v1/events", body.data(), dl, err, sizeof err) == 0;
    if (!queued) {
        puddle_log(PUDDLE_LOG_WARN, "message %.12s… is stored but could not be queued for delivery", id);
        throw Error(500, "not_queued", "it was saved here but could not be queued to send");
    }
}

// ---- Message ----

Message Message::fromView(const puddle_view_msg &v, const std::string &conversation, const std::string &chat)
{
    Message m;
    m.id_ = v.id;
    m.conversation_ = conversation;
    m.chat_ = chat;
    m.sender_ = v.sender;
    m.ts_ = v.ts;
    m.rts_ = v.rts;
    m.ref_ = v.ref;
    m.deleted_ = v.deleted != 0;
    if (!v.deleted)
        m.body_ = v.body ? v.body : "";
    if (v.edited && !v.deleted)
        m.editedTs_ = v.edited_ts;
    return m;
}

std::optional<Message> Message::fromNotice(const puddle_event &ev, const std::string &conversation)
{
    puddle_gevent g;
    const char *why;
    if (puddle_group_event_parse(ev.event, ev.event_len, &g, &why) != 0)
        return std::nullopt;
    Message m;
    m.id_ = ev.id;
    m.conversation_ = conversation;
    m.chat_ = conversation;
    m.sender_ = g.sender;
    m.ts_ = g.ts;
    m.rts_ = ev.rts;
    switch (g.type) {
    case PUDDLE_GE_CREATE: m.type_ = Type::GroupCreate; m.name_ = g.name; break;
    case PUDDLE_GE_RENAME: m.type_ = Type::GroupRename; m.name_ = g.name; break;
    case PUDDLE_GE_INVITE: m.type_ = Type::GroupInvite; m.member_ = g.member; break;
    case PUDDLE_GE_JOIN: m.type_ = Type::GroupJoin; break;
    case PUDDLE_GE_LEAVE: m.type_ = Type::GroupLeave; break;
    }
    return m;
}

std::optional<Message> Message::get(puddle_ctx *ctx, const std::string &conversation, const std::string &id)
{
    std::string chat = conversation;
    if (!isGroup(ctx, conversation)) {
        char dm[PUDDLE_USER_ID_LEN + 1];
        if (puddle_dm_chat_id(ownId(ctx).c_str(), conversation.c_str(), dm) != 0)
            return std::nullopt;
        chat = dm;
    }
    puddle_view_msg v;
    memset(&v, 0, sizeof v);
    if (!findInChat(ctx, chat, id, &v))
        return std::nullopt;
    Message m = fromView(v, conversation, chat);
    puddle_view_clear(&v);
    return m;
}

Message Message::edit(puddle_ctx *ctx, const std::string &text) const
{
    std::string me = ownId(ctx);
    bool group = chat_ == conversation_;
    if (group)
        requireMember(ctx, chat_);
    else
        requireOpen(ctx, conversation_);
    if (sender_ != me || type_ != Type::Default)
        throw Error::notFound(); // someone else's is not ours to touch
    if (deleted_)
        throw Error::conflict("deleted");
    char event[PUDDLE_EVENT_MAX_BYTES], id[PUDDLE_ID_HEX + 1];
    int64_t now = puddle_ctx_now_ms(ctx);
    int64_t ts = puddle_event_next_ts(now > ts_ ? now : ts_ + 1); // an edit has to be later than what it edits
    size_t el = puddle_edit_build(chat_.c_str(), me.c_str(), ts, id_.c_str(), text.data(), text.size(), event, sizeof event);
    if (!el)
        throw Error::badRequest();
    sendEvent(ctx, group, conversation_, chat_, event, el, id);
    Message m = *this;
    m.body_ = text;
    m.editedTs_ = ts;
    return m;
}

Message Message::remove(puddle_ctx *ctx) const
{
    std::string me = ownId(ctx);
    bool group = chat_ == conversation_;
    if (group)
        requireMember(ctx, chat_);
    else
        requireOpen(ctx, conversation_);
    if (sender_ != me || type_ != Type::Default)
        throw Error::notFound();
    if (deleted_)
        return *this; // already gone: nothing more to say
    char event[PUDDLE_EVENT_MAX_BYTES], id[PUDDLE_ID_HEX + 1];
    int64_t now = puddle_ctx_now_ms(ctx);
    int64_t ts = puddle_event_next_ts(now > ts_ ? now : ts_ + 1);
    size_t el = puddle_redact_build(chat_.c_str(), me.c_str(), ts, id_.c_str(), event, sizeof event);
    if (!el)
        throw Error::internal();
    sendEvent(ctx, group, conversation_, chat_, event, el, id);
    Message m = *this;
    m.body_.reset();
    m.editedTs_.reset();
    m.deleted_ = true;
    return m;
}

void Message::write(puddle_jw *w, puddle_ctx *ctx, PartialUsers &people, const Deliveries *deliveries) const
{
    std::string me = ownId(ctx);
    puddle_jw_object(w);
    puddle_jw_str(w, "id", id_.c_str());
    puddle_jw_str(w, "conversation", conversation_.c_str());
    puddle_jw_str(w, "type", typeName(type_));
    puddle_jw_key(w, "author");
    people.write(w, sender_);
    puddle_jw_num(w, "ts", ts_);
    if (body_)
        puddle_jw_str(w, "body", body_->c_str());
    if (!ref_.empty())
        puddle_jw_str(w, "reply_to", ref_.c_str());
    if (editedTs_)
        puddle_jw_num(w, "edited_at", *editedTs_);
    if (deleted_)
        puddle_jw_flag(w, "deleted", 1);
    if (!member_.empty()) {
        puddle_jw_key(w, "member");
        people.write(w, member_);
    }
    if (!name_.empty())
        puddle_jw_str(w, "name", name_.c_str());
    puddle_jw_flag(w, "outgoing", sender_ == me);
    puddle_jw_num(w, "received_at", rts_);
    const puddle_ob_item *it = deliveries && !deleted_ && sender_ == me && type_ == Type::Default ? deliveries->of(id_) : nullptr;
    if (it) {
        puddle_jw_key(w, "delivery");
        puddle_jw_object(w);
        puddle_jw_str(w, "state", it->state == PUDDLE_OB_FAILED ? "failed" : it->state == PUDDLE_OB_HELD ? "held" : it->attempts ? "retrying" : "queued");
        if (it->reason[0])
            puddle_jw_str(w, "reason", it->reason);
        if (it->state == PUDDLE_OB_QUEUED && it->attempts)
            puddle_jw_num(w, "next_at", it->next_ms);
        puddle_jw_num(w, "attempts", it->attempts);
        puddle_jw_end_object(w);
    }
    puddle_jw_end_object(w);
}

} // namespace puddle
