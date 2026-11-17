#include "events/group_events.h"

#include "base/encoding.h"
#include "base/json.h"
#include "base/text.h"

#include <openssl/rand.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TS_MIN 946684800000LL   /* 2000-01-01: an event outside these years is refused */
#define TS_MAX 4102444800000LL  /* 2100-01-01 */

/* ---- ids ---- */

int puddle_group_nonce(char out[33])
{
    unsigned char raw[16];
    static const char hex[] = "0123456789abcdef";
    if (RAND_bytes(raw, sizeof raw) != 1)
        return -1;
    for (int i = 0; i < 16; i++) {
        out[2 * i] = hex[raw[i] >> 4];
        out[2 * i + 1] = hex[raw[i] & 15];
    }
    out[32] = '\0';
    return 0;
}

static int is_nonce(const char *s)
{
    unsigned char raw[16];
    return s && strlen(s) == 32 && puddle_hex_decode(s, raw, 16) == 0;
}

int puddle_group_chat_id(const char *creator, int64_t ts, const char *nonce, char out[PUDDLE_USER_ID_LEN + 1])
{
    char text[200];
    unsigned char h[32];
    if (!puddle_id_valid(creator) || !is_nonce(nonce) || ts < TS_MIN || ts >= TS_MAX)
        return -1;
    int n = snprintf(text, sizeof text, "group\n%s\n%lld\n%s", creator, (long long)ts, nonce);
    SHA256((const unsigned char *)text, (size_t)n, h);
    return puddle_base32_encode(h, 32, out, PUDDLE_USER_ID_LEN + 1) == PUDDLE_USER_ID_LEN ? 0 : -1;
}

int puddle_group_name_ok(const char *name)
{
    char clean[PUDDLE_GROUP_NAME_MAX_BYTES + 1];
    size_t chars;
    if (!name || !*name)
        return 0;
    /* what the cleaner would keep must be exactly what we were given, so a name is never silently changed on the way */
    return puddle_clean_text(name, strlen(name), PUDDLE_GROUP_NAME_MAX_CHARS, 0, clean, sizeof clean, &chars) == 0 && chars > 0 &&
           !strcmp(clean, name);
}

/* ---- events ---- */

static size_t build(puddle_ge_type type, const char *chat, const char *sender, int64_t ts, const char *name, const char *nonce, const char *member,
                    char *out, size_t cap)
{
    static const char *const names[] = { "chat.create", "chat.rename", "member.invite", "member.join", "member.leave" };
    puddle_jw w;
    puddle_jw_init(&w, out, cap);
    puddle_jw_object(&w);
    puddle_jw_str(&w, "type", names[type]);
    puddle_jw_str(&w, "chat", chat);
    puddle_jw_str(&w, "sender", sender);
    puddle_jw_num(&w, "ts", ts);
    if (type == PUDDLE_GE_CREATE) {
        puddle_jw_str(&w, "name", name);
        puddle_jw_str(&w, "nonce", nonce);
    } else if (type == PUDDLE_GE_RENAME) {
        puddle_jw_str(&w, "body", name);
    } else if (type == PUDDLE_GE_INVITE) {
        puddle_jw_str(&w, "member", member);
    }
    puddle_jw_end_object(&w);
    return puddle_jw_finish(&w);
}

size_t puddle_group_create_build(const char *creator, int64_t ts, const char *nonce, const char *name, char chat_out[PUDDLE_USER_ID_LEN + 1],
                                 char *out, size_t cap)
{
    if (!puddle_group_name_ok(name) || puddle_group_chat_id(creator, ts, nonce, chat_out) != 0)
        return 0;
    return build(PUDDLE_GE_CREATE, chat_out, creator, ts, name, nonce, NULL, out, cap);
}

size_t puddle_group_rename_build(const char *chat, const char *sender, int64_t ts, const char *name, char *out, size_t cap)
{
    if (!puddle_group_name_ok(name) || !puddle_id_valid(chat) || !puddle_id_valid(sender))
        return 0;
    return build(PUDDLE_GE_RENAME, chat, sender, ts, name, NULL, NULL, out, cap);
}

size_t puddle_group_invite_build(const char *chat, const char *sender, int64_t ts, const char *invitee, char *out, size_t cap)
{
    if (!puddle_id_valid(chat) || !puddle_id_valid(sender) || !puddle_id_valid(invitee))
        return 0;
    return build(PUDDLE_GE_INVITE, chat, sender, ts, NULL, NULL, invitee, out, cap);
}

size_t puddle_group_join_build(const char *chat, const char *sender, int64_t ts, char *out, size_t cap)
{
    if (!puddle_id_valid(chat) || !puddle_id_valid(sender))
        return 0;
    return build(PUDDLE_GE_JOIN, chat, sender, ts, NULL, NULL, NULL, out, cap);
}

size_t puddle_group_leave_build(const char *chat, const char *sender, int64_t ts, char *out, size_t cap)
{
    if (!puddle_id_valid(chat) || !puddle_id_valid(sender))
        return 0;
    return build(PUDDLE_GE_LEAVE, chat, sender, ts, NULL, NULL, NULL, out, cap);
}

static int member_str(const char *e, size_t n, const char *key, char *out, size_t cap)
{
    puddle_span v;
    return puddle_json_member(e, n, key, &v) == 0 && puddle_json_string(v, out, cap) == 0 ? 0 : -1;
}

int puddle_group_event_parse(const char *event, size_t len, puddle_gevent *g, const char **why)
{
    memset(g, 0, sizeof *g);
    *why = "malformed";
    if (len == 0 || len > 16 * 1024 || !puddle_json_valid_object(event, len))
        return -1;
    char type[20];
    if (member_str(event, len, "type", type, sizeof type) != 0) {
        /* a type longer than any of ours is not one of ours */
        puddle_span tv;
        if (puddle_json_member(event, len, "type", &tv) == 0 && tv.len >= 2)
            *why = "unsupported_type";
        return -1;
    }
    if (!strcmp(type, "chat.create")) g->type = PUDDLE_GE_CREATE;
    else if (!strcmp(type, "chat.rename")) g->type = PUDDLE_GE_RENAME;
    else if (!strcmp(type, "member.invite")) g->type = PUDDLE_GE_INVITE;
    else if (!strcmp(type, "member.join")) g->type = PUDDLE_GE_JOIN;
    else if (!strcmp(type, "member.leave")) g->type = PUDDLE_GE_LEAVE;
    else {
        *why = "unsupported_type";
        return -1;
    }
    if (member_str(event, len, "chat", g->chat, sizeof g->chat) != 0 || member_str(event, len, "sender", g->sender, sizeof g->sender) != 0 ||
        !puddle_id_valid(g->chat) || !puddle_id_valid(g->sender))
        return -1;
    puddle_span v;
    if (puddle_json_member(event, len, "ts", &v) != 0 || puddle_json_int64(v, &g->ts) != 0)
        return -1;
    if (g->ts < TS_MIN || g->ts >= TS_MAX) {
        *why = "bad_time";
        return -1;
    }
    int has_body = puddle_json_member(event, len, "body", &v) == 0;
    int has_name = puddle_json_member(event, len, "name", &v) == 0;
    int has_nonce = puddle_json_member(event, len, "nonce", &v) == 0;
    int has_member = puddle_json_member(event, len, "member", &v) == 0;
    int has_ref = puddle_json_member(event, len, "ref", &v) == 0;
    if (has_ref)
        return -1; /* these events point at nothing */
    switch (g->type) {
    case PUDDLE_GE_CREATE:
        if (has_body || has_member || !has_name || !has_nonce || member_str(event, len, "name", g->name, sizeof g->name) != 0 ||
            member_str(event, len, "nonce", g->nonce, sizeof g->nonce) != 0 || !puddle_group_name_ok(g->name) || !is_nonce(g->nonce))
            return -1;
        {
            char want[PUDDLE_USER_ID_LEN + 1];
            if (puddle_group_chat_id(g->sender, g->ts, g->nonce, want) != 0 || strcmp(want, g->chat) != 0) {
                *why = "bad_group_id"; /* the id has to be the one its creator, time and nonce make */
                return -1;
            }
        }
        break;
    case PUDDLE_GE_RENAME:
        if (!has_body || has_name || has_nonce || has_member || member_str(event, len, "body", g->name, sizeof g->name) != 0 ||
            !puddle_group_name_ok(g->name))
            return -1;
        break;
    case PUDDLE_GE_INVITE:
        if (has_body || has_name || has_nonce || !has_member || member_str(event, len, "member", g->member, sizeof g->member) != 0 ||
            !puddle_id_valid(g->member) || !strcmp(g->member, g->sender))
            return -1;
        break;
    case PUDDLE_GE_JOIN:
    case PUDDLE_GE_LEAVE:
        if (has_body || has_name || has_nonce || has_member)
            return -1;
        break;
    }
    *why = NULL;
    return 0;
}

/* ---- folding them into the group ---- */

int puddle_group_init_created(puddle_group *g, const puddle_gevent *create, int64_t now_ms)
{
    memset(g, 0, sizeof *g);
    snprintf(g->id, sizeof g->id, "%s", create->chat);
    snprintf(g->name, sizeof g->name, "%s", create->name);
    g->name_ts = create->ts;
    snprintf(g->creator, sizeof g->creator, "%s", create->sender);
    g->created_ms = g->updated_ms = now_ms;
    g->my = PUDDLE_GROUP_IN;
    puddle_gmember *m = puddle_group_add_member(g, create->sender);
    if (!m)
        return -1;
    m->state = PUDDLE_GM_MEMBER;
    m->ts = create->ts;
    return 0;
}

static size_t live_count(const puddle_group *g)
{
    return puddle_group_count(g, PUDDLE_GM_MEMBER) + puddle_group_count(g, PUDDLE_GM_INVITED);
}

int puddle_group_apply(puddle_group *g, const puddle_gevent *e, const char **why)
{
    *why = NULL;
    puddle_gmember *sender = puddle_group_member(g, e->sender);
    switch (e->type) {
    case PUDDLE_GE_CREATE:
        *why = "bad_event";
        return -1;
    case PUDDLE_GE_RENAME:
        if (!sender || sender->state != PUDDLE_GM_MEMBER) {
            *why = "not_a_member";
            return -1;
        }
        if (e->ts < g->name_ts || (e->ts == g->name_ts && strcmp(e->name, g->name) <= 0))
            return 1; /* an older name, or the same moment with a name that loses the tie: every Puddle breaks it the same way */
        snprintf(g->name, sizeof g->name, "%s", e->name);
        g->name_ts = e->ts;
        return 0;
    case PUDDLE_GE_INVITE: {
        if (!sender || sender->state != PUDDLE_GM_MEMBER) {
            *why = "not_a_member";
            return -1;
        }
        puddle_gmember *m = puddle_group_member(g, e->member);
        if (m && m->state != PUDDLE_GM_LEFT)
            return 1; /* already invited or in */
        if (m && e->ts <= m->ts)
            return 1; /* a stale invitation: they have left since */
        if (live_count(g) >= PUDDLE_GROUP_MAX_MEMBERS) {
            *why = "group_full";
            return -1;
        }
        if (!m && !(m = puddle_group_add_member(g, e->member))) {
            *why = "bad_event";
            return -1;
        }
        m->state = PUDDLE_GM_INVITED;
        snprintf(m->by, sizeof m->by, "%s", e->sender);
        m->ts = e->ts;
        return 0;
    }
    case PUDDLE_GE_JOIN:
        if (!sender || sender->state != PUDDLE_GM_INVITED) {
            if (sender && sender->state == PUDDLE_GM_MEMBER)
                return 1; /* already in */
            *why = "not_invited";
            return -1;
        }
        if (e->ts <= sender->ts) {
            *why = "not_invited"; /* an answer cannot be older than the question */
            return -1;
        }
        sender->state = PUDDLE_GM_MEMBER;
        sender->ts = e->ts;
        return 0;
    case PUDDLE_GE_LEAVE:
        if (!sender || sender->state == PUDDLE_GM_LEFT) {
            if (sender)
                return 1;
            *why = "not_a_member";
            return -1;
        }
        if (e->ts <= sender->ts)
            return 1; /* older than their last change: stale */
        sender->state = PUDDLE_GM_LEFT;
        sender->ts = e->ts;
        return 0;
    }
    *why = "bad_event";
    return -1;
}
