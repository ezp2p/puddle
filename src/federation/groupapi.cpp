#include "federation/profiles_api.h"
#include "federation/groupapi.h"

#include "federation/access.h"
#include "http/client.h"
#include "federation/groupflow.h"
#include "base/log.h"
#include "events/message.h"
#include "federation/messages.h"
#include "events/group_events.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- helpers ---- */

static int string_member(puddle_span obj, const char *key, char *out, size_t cap)
{
    puddle_span v;
    return puddle_json_member(obj.p, obj.len, key, &v) == 0 && puddle_json_string(v, out, cap) == 0 ? 0 : -1;
}

/* does `sigtext` sign `event` as the current key of the peer `sender`? */
static int signed_by(puddle_ctx *ctx, const char *sender, const char *event, size_t el, const char *sigtext)
{
    char kid[65];
    unsigned char sig[64], pub[32];
    if (puddle_event_sig_parse(sigtext, kid, sig) != 0 || !ctx->peer_key || !ctx->peer_key(ctx->peer_key_user, sender, kid, pub))
        return 0;
    return puddle_event_verify(pub, event, el, sig);
}

static int open_peer_state(puddle_ctx *ctx, const char *peer, int friends_only)
{
    puddle_stream st;
    if (puddle_streams_get(ctx->streams, peer, &st) != 0)
        return 0;
    int ok = friends_only ? st.state == PUDDLE_STREAM_ACTIVE : puddle_stream_carries_groups(&st);
    puddle_stream_free(&st);
    return ok;
}

/* ---- being invited ---- */

typedef struct {
    puddle_group fresh;       /* what the invitation makes of the group */
    int64_t invite_ts;
    char me[PUDDLE_USER_ID_LEN + 1];
    int accepted;
} invited;

static int invited_fn(void *user, puddle_group *g, int exists)
{
    invited *in = (invited *)user;
    if (exists) {
        if (g->my == PUDDLE_GROUP_IN || g->my == PUDDLE_GROUP_INVITED)
            return 1; /* we are in it, or already asked */
        /* left or declined: a later invitation asks again, an older one does not */
        puddle_gmember *mine = puddle_group_member(g, in->me);
        if (mine && in->invite_ts <= mine->ts)
            return 1;
    }
    puddle_group_free(g);
    *g = in->fresh;
    memset(&in->fresh, 0, sizeof in->fresh); /* the members now belong to the stored record */
    in->accepted = 1;
    return 0;
}

static int group_invite_post(const puddle_call *c, puddle_jw *w)
{
    puddle_ctx *ctx = c->ctx;
    const char *b = (const char *)c->body;
    puddle_span create, inv, roster, item, v;
    char gid[PUDDLE_USER_ID_LEN + 1], me[PUDDLE_USER_ID_LEN + 1];
    if (!ctx->groups || !ctx->store)
        return puddle_fail(w, 404, "not_found");
    if (!open_peer_state(ctx, c->sender, 1)) /* an invitation travels over a friendship */
        return puddle_fail(w, 403, "no_stream");
    puddle_identity_id(ctx->identity, me);
    if (!puddle_json_valid_object(b, c->body_len) || puddle_json_member(b, c->body_len, "group", &v) != 0 ||
        puddle_json_string(v, gid, sizeof gid) != 0 || !puddle_id_valid(gid) || puddle_json_member(b, c->body_len, "create", &create) != 0 ||
        puddle_json_member(b, c->body_len, "invite", &inv) != 0 || puddle_json_member(b, c->body_len, "members", &roster) != 0)
        return puddle_fail(w, 400, "bad_request");

    invited in;
    memset(&in, 0, sizeof in);
    puddle_group *g = &in.fresh;
    char ce[PUDDLE_GROUP_CREATE_MAX + 1], cs[180], ie[600], is[180];
    const char *why;
    puddle_gevent cev, iev;
    puddle_span tv;
    if (puddle_json_member(create.p, create.len, "event", &tv) != 0 || puddle_json_string(tv, ce, sizeof ce) != 0 ||
        string_member(create, "sig", cs, sizeof cs) != 0 || puddle_group_event_parse(ce, strlen(ce), &cev, &why) != 0 || cev.type != PUDDLE_GE_CREATE ||
        strcmp(cev.chat, gid) != 0)
        return puddle_fail(w, 400, "bad_request");
    if (puddle_json_member(inv.p, inv.len, "event", &tv) != 0 || puddle_json_string(tv, ie, sizeof ie) != 0 || string_member(inv, "sig", is, sizeof is) != 0 ||
        puddle_group_event_parse(ie, strlen(ie), &iev, &why) != 0 || iev.type != PUDDLE_GE_INVITE || strcmp(iev.chat, gid) != 0 ||
        strcmp(iev.sender, c->sender) != 0 || strcmp(iev.member, me) != 0)
        return puddle_fail(w, 400, "bad_request");
    if (!signed_by(ctx, c->sender, ie, strlen(ie), is)) /* the invitation is the sender's own word */
        return puddle_fail(w, 400, "bad_request");
    if (!strcmp(cev.sender, c->sender) && !signed_by(ctx, c->sender, ce, strlen(ce), cs))
        return puddle_fail(w, 400, "bad_request");

    snprintf(g->id, sizeof g->id, "%s", gid);
    snprintf(g->name, sizeof g->name, "%s", cev.name);
    g->name_ts = cev.ts;
    snprintf(g->creator, sizeof g->creator, "%s", cev.sender);
    g->created_ms = g->updated_ms = puddle_call_now_ms(c);
    g->my = PUDDLE_GROUP_INVITED;
    snprintf(g->invited_by, sizeof g->invited_by, "%s", c->sender);
    snprintf(g->create_event, sizeof g->create_event, "%s", ce);
    snprintf(g->create_sig, sizeof g->create_sig, "%s", cs);

    int have_sender = 0, have_me = 0, have_creator = 0;
    size_t cursor = 0;
    int r;
    while ((r = puddle_json_array_next(roster, &cursor, &item)) == 1) {
        char id[PUDDLE_USER_ID_LEN + 1], st[16], addr[PUDDLE_ADDR_MAX] = "";
        int64_t ts;
        if (string_member(item, "id", id, sizeof id) != 0 || !puddle_id_valid(id) || puddle_group_member(g, id) ||
            string_member(item, "state", st, sizeof st) != 0 || (strcmp(st, "member") && strcmp(st, "invited")) || g->nmembers >= PUDDLE_GROUP_MAX_MEMBERS) {
            puddle_group_free(g);
            return puddle_fail(w, 400, "bad_request");
        }
        puddle_span ts_v, av;
        if (puddle_json_member(item.p, item.len, "ts", &ts_v) != 0 || puddle_json_int64(ts_v, &ts) != 0 || ts < 0) {
            puddle_group_free(g);
            return puddle_fail(w, 400, "bad_request");
        }
        int arc = puddle_json_member(item.p, item.len, "address", &av);
        if (arc < 0 || (arc == 0 && (puddle_json_string(av, addr, sizeof addr) != 0))) {
            puddle_group_free(g);
            return puddle_fail(w, 400, "bad_request");
        }
        puddle_address pa;
        if (addr[0] && (puddle_address_parse(addr, &pa) != 0 || !puddle_net_host_plausible(pa.host)))
            addr[0] = '\0'; /* an address we would never connect to is not kept */
        puddle_gmember *m = (puddle_gmember *)realloc(g->members, (g->nmembers + 1) * sizeof *m);
        if (!m) {
            puddle_group_free(g);
            return puddle_fail(w, 500, "internal");
        }
        g->members = m;
        m = &g->members[g->nmembers++];
        memset(m, 0, sizeof *m);
        snprintf(m->id, sizeof m->id, "%s", id);
        m->state = !strcmp(st, "member") ? PUDDLE_GM_MEMBER : PUDDLE_GM_INVITED;
        m->ts = ts;
        snprintf(m->address, sizeof m->address, "%s", addr);
        if (!strcmp(id, c->sender))
            have_sender = m->state == PUDDLE_GM_MEMBER;
        if (!strcmp(id, me)) {
            have_me = m->state == PUDDLE_GM_INVITED;
            m->ts = iev.ts; /* our answer has to be newer than this invitation */
            snprintf(m->by, sizeof m->by, "%s", c->sender);
        }
        if (!strcmp(id, cev.sender))
            have_creator = 1;
    }
    if (r < 0 || !have_sender || !have_me || !have_creator) {
        puddle_group_free(g);
        return puddle_fail(w, 400, "bad_request");
    }

    if (puddle_groups_pending(ctx->groups) >= PUDDLE_GROUP_MAX_PENDING_IN) {
        puddle_group_free(g);
        puddle_log(PUDDLE_LOG_INFO, "group invitation from %.12s… dropped: too many waiting", c->sender);
        return 202;
    }
    in.invite_ts = iev.ts;
    snprintf(in.me, sizeof in.me, "%s", me);
    char err[300], id[65];
    int ur = puddle_groups_update(ctx->groups, gid, invited_fn, &in, err, sizeof err);
    puddle_group_free(&in.fresh);
    if (ur == 0) {
        puddle_store_append(ctx->store, gid, ce, strlen(ce), cs, 0, id, err, sizeof err);
        puddle_store_append(ctx->store, gid, ie, strlen(ie), is, 0, id, err, sizeof err);
        puddle_log(PUDDLE_LOG_INFO, "%.12s… invited us to a group", c->sender);
    }
    return 202;
}

/* ---- events in a group ---- */

typedef struct {
    puddle_ctx *ctx;
    const char *sender;
    const char *chat;
    char me[PUDDLE_USER_ID_LEN + 1];
} gpolicy;

static const char *group_judge(void *user, const char *text, size_t tl)
{
    gpolicy *p = (gpolicy *)user;
    puddle_gevent ge;
    const char *why;
    puddle_group g;
    if (puddle_group_event_parse(text, tl, &ge, &why) == 0) {
        if (ge.type == PUDDLE_GE_CREATE)
            return "use_invite"; /* a group is first heard of through an invitation */
        if (strcmp(ge.chat, p->chat) != 0)
            return "wrong_chat";
        if (strcmp(ge.sender, p->sender) != 0)
            return "wrong_sender";
        return NULL; /* the rules are applied when it is taken */
    }
    if (strcmp(why, "unsupported_type") != 0)
        return why;
    puddle_message m;
    if (puddle_chat_event_parse(text, tl, &m, &why) != 0)
        return why;
    const char *reason = NULL;
    if (strcmp(m.chat, p->chat) != 0)
        reason = "wrong_chat";
    else if (strcmp(m.sender, p->sender) != 0)
        reason = "wrong_sender";
    puddle_message_free(&m);
    if (reason)
        return reason;
    if (puddle_groups_get(p->ctx->groups, p->chat, &g) != 0)
        return "not_in_group";
    puddle_gmember *from = puddle_group_member(&g, p->sender);
    if (g.my != PUDDLE_GROUP_IN)
        reason = "not_in_group";
    else if (!from || from->state != PUDDLE_GM_MEMBER)
        reason = "not_a_member";
    puddle_group_free(&g);
    return reason;
}

typedef struct {
    puddle_gevent ev;
    const char *why;
    int applied;
} applying;

static int apply_fn(void *user, puddle_group *g, int exists)
{
    applying *a = (applying *)user;
    if (!exists || (g->my != PUDDLE_GROUP_IN && g->my != PUDDLE_GROUP_INVITED)) {
        a->why = "not_in_group";
        return 3;
    }
    a->applied = puddle_group_apply(g, &a->ev, &a->why);
    if (a->applied < 0)
        return 3;
    if (a->applied > 0)
        return 1; /* nothing changed: nothing to store */
    g->updated_ms = a->ev.ts;
    return 0;
}

static const char *group_commit(void *user, const char *text, size_t tl, const char *sigtext, char id_out[PUDDLE_ID_HEX + 1], int *dup)
{
    gpolicy *p = (gpolicy *)user;
    puddle_gevent ge;
    const char *why;
    if (puddle_group_event_parse(text, tl, &ge, &why) == 0) {
        applying a = { .ev = ge };
        char err[300];
        int ur = puddle_groups_update(p->ctx->groups, p->chat, apply_fn, &a, err, sizeof err);
        if (ur == 3)
            return a.why ? a.why : "bad_event";
        if (ur < 0)
            return "storage_error";
        const char *stored = puddle_store_delivered(p->ctx, p->chat, p->sender, text, tl, sigtext, id_out, dup);
        if (!stored && ur == 0) {
            puddle_log(PUDDLE_LOG_INFO, "group %.12s…: %s from %.12s…", p->chat, ge.type == PUDDLE_GE_JOIN ? "a join" : ge.type == PUDDLE_GE_LEAVE ? "a leave" :
                       ge.type == PUDDLE_GE_INVITE ? "an invitation" : "a new name", p->sender);
            puddle_group_reconcile(p->ctx);
            char place[PUDDLE_PLACE_MAX];
            if (ge.type == PUDDLE_GE_JOIN && puddle_place_group(p->chat, place) == 0) /* someone new there: who we are in it */
                puddle_profile_push_to(p->ctx, p->sender, place);
        }
        return stored;
    }
    return puddle_store_delivered(p->ctx, p->chat, p->sender, text, tl, sigtext, id_out, dup);
}

int puddle_group_events_post(const puddle_call *c, puddle_jw *w, const char *chat)
{
    puddle_ctx *ctx = c->ctx;
    const char *b = (const char *)c->body;
    puddle_span evs;
    puddle_group g;
    if (!ctx->groups || puddle_groups_get(ctx->groups, chat, &g) != 0) /* no such chat here: what a direct message would say */
        return puddle_fail(w, 400, "bad_request");
    puddle_group_free(&g);

    puddle_stream st;
    if (puddle_streams_get(ctx->streams, c->sender, &st) != 0)
        return puddle_fail(w, 403, "no_stream");
    int open = puddle_stream_carries_groups(&st) && st.chain;
    char *chain = open ? strdup(st.chain) : NULL;
    size_t chain_len = open ? st.chain_len : 0;
    puddle_stream_free(&st);
    if (!chain)
        return puddle_fail(w, 403, "no_stream");
    if (puddle_json_member(b, c->body_len, "events", &evs) != 0) {
        free(chain);
        return puddle_fail(w, 400, "bad_request");
    }
    puddle_chain_head head;
    char err[200], kid[65];
    int cv = puddle_chain_verify(chain, chain_len, c->sender, &head, err, sizeof err);
    free(chain);
    if (cv != 0)
        return puddle_fail(w, 500, "internal");
    puddle_key_id(head.sign_pub, kid);

    gpolicy gp = { ctx, c->sender, chat, "" };
    puddle_identity_id(ctx->identity, gp.me);
    puddle_event_policy pol = { group_judge, group_commit, &gp };
    return puddle_process_delivery(c, w, evs, &pol, head.sign_pub, kid);
}

const puddle_route puddle_group_routes[] = {
    { "POST", "/stream/v1/group/invite", PUDDLE_AUTH_PEER, 64 * 1024, group_invite_post, NULL },
};
const size_t puddle_group_routes_count = sizeof puddle_group_routes / sizeof puddle_group_routes[0];
