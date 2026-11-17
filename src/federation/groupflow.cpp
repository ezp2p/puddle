#include "federation/profiles_api.h"
#include "federation/groupflow.h"

#include "federation/flow.h"
#include "base/hash.h"
#include "base/log.h"
#include "events/message.h"
#include "core/outbox.h"
#include "events/group_events.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- small things ---- */

int puddle_group_open_with(puddle_ctx *ctx, const char *peer)
{
    puddle_stream st;
    int have = puddle_streams_get(ctx->streams, peer, &st);
    int open = have == 0 && puddle_stream_carries_groups(&st);
    if (have == 0)
        puddle_stream_free(&st);
    return open && !puddle_streams_is_blocked(ctx->streams, peer);
}

static int is_friend(puddle_ctx *ctx, const char *peer)
{
    puddle_stream st;
    int have = puddle_streams_get(ctx->streams, peer, &st);
    int active = have == 0 && st.state == PUDDLE_STREAM_ACTIVE;
    if (have == 0)
        puddle_stream_free(&st);
    return active && !puddle_streams_is_blocked(ctx->streams, peer);
}

static int fail(const char **code, int status, const char *why)
{
    *code = why;
    return status;
}

/* {"chat":..,"events":[{"id","event","sig"}]}, malloc'd */
static char *delivery_body(const char *chat, const char *id, const char *event, size_t el, const char *sig, size_t *len)
{
    size_t cap = el * 6 + 1024;
    char *body = (char *)malloc(cap);
    if (!body)
        return NULL;
    puddle_jw d;
    puddle_jw_init(&d, body, cap);
    puddle_jw_object(&d);
    puddle_jw_str(&d, "chat", chat);
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
    *len = puddle_jw_finish(&d);
    if (!*len) {
        free(body);
        return NULL;
    }
    return body;
}

/* Queue an event for every member (but us and `skip`) we have an open stream with. Returns how many it was queued for;
 * `failed` counts the ones we meant to and could not. */
static int fanout(puddle_ctx *ctx, const puddle_group *g, const char *me, const char *id, const char *event, size_t el, const char *sig,
                  const char *skip, int to_invited, int *failed)
{
    size_t bl;
    char *body = delivery_body(g->id, id, event, el, sig, &bl);
    int sent = 0;
    *failed = 0;
    if (!body || !ctx->outbox) {
        free(body);
        for (size_t i = 0; i < g->nmembers; i++)
            *failed += (g->members[i].state == PUDDLE_GM_MEMBER || (to_invited && g->members[i].state == PUDDLE_GM_INVITED)) && strcmp(g->members[i].id, me) != 0 &&
                       puddle_group_open_with(ctx, g->members[i].id);
        return 0;
    }
    for (size_t i = 0; i < g->nmembers; i++) {
        const puddle_gmember *m = &g->members[i];
        char err[300];
        if ((m->state != PUDDLE_GM_MEMBER && !(to_invited && m->state == PUDDLE_GM_INVITED)) || !strcmp(m->id, me) || (skip && !strcmp(m->id, skip)) ||
            !puddle_group_open_with(ctx, m->id))
            continue;
        if (puddle_outbox_enqueue(ctx->outbox, m->id, "events", "/stream/v1/events", body, bl, err, sizeof err) == 0)
            sent++;
        else
            (*failed)++;
    }
    free(body);
    return sent;
}

/* Sign `event`, keep it in our log of the group, and send it to the members (and, for what concerns the group itself, to those invited
 * who have not answered yet: they need to know who is in it). */
static int emit(puddle_ctx *ctx, const puddle_group *g, const char *event, size_t el, const char *skip, int to_invited, char id[65],
                char sig[PUDDLE_SIG_TEXT_MAX], int *sent, const char **code)
{
    char me[PUDDLE_USER_ID_LEN + 1], err[300];
    puddle_identity_id(ctx->identity, me);
    if (puddle_event_sign(ctx->identity, event, el, sig) != 0)
        return fail(code, 500, "internal");
    if (puddle_store_append(ctx->store, g->id, event, el, sig, 0, id, err, sizeof err) < 0)
        return fail(code, 500, "internal");
    int failed;
    *sent = fanout(ctx, g, me, id, event, el, sig, skip, to_invited, &failed);
    if (failed && !*sent) {
        puddle_log(PUDDLE_LOG_WARN, "an event for group %.12s… is stored but could not be queued for delivery", g->id);
        return fail(code, 500, "not_queued");
    }
    return 0;
}

static int64_t stamp(puddle_ctx *ctx, int64_t after)
{
    int64_t ts = puddle_event_next_ts(puddle_ctx_now_ms(ctx));
    return ts <= after ? after + 1 : ts;
}

/* ---- changing our record ---- */

typedef struct {
    puddle_gevent ev;
    const char *why;
    int applied;      /* what puddle_group_apply said */
    puddle_group_my my;
    int set_my;
    const char *join_event, *join_sig;
} change;

static int change_fn(void *user, puddle_group *g, int exists)
{
    change *c = (change *)user;
    if (!exists)
        return 2;
    c->applied = puddle_group_apply(g, &c->ev, &c->why);
    if (c->applied < 0)
        return 3;
    if (c->set_my)
        g->my = c->my;
    if (c->join_event) {
        snprintf(g->join_event, sizeof g->join_event, "%s", c->join_event);
        snprintf(g->join_sig, sizeof g->join_sig, "%s", c->join_sig);
    }
    g->updated_ms = c->ev.ts;
    return 0;
}

static int load(puddle_ctx *ctx, const char *gid, puddle_group *g, const char **code, int *status)
{
    if (!ctx->groups || !ctx->store || !ctx->streams) {
        *code = "internal";
        *status = 500;
        return -1;
    }
    if (puddle_groups_get(ctx->groups, gid, g) != 0) {
        *code = "not_found";
        *status = 404;
        return -1;
    }
    return 0;
}

/* ---- create ---- */

int puddle_group_create(puddle_ctx *ctx, const char *name, char gid[PUDDLE_USER_ID_LEN + 1], const char **code)
{
    char me[PUDDLE_USER_ID_LEN + 1], nonce[33], ev[PUDDLE_GROUP_CREATE_MAX + 1], sig[PUDDLE_SIG_TEXT_MAX], id[65], err[300];
    if (!ctx->groups || !ctx->store)
        return fail(code, 500, "internal");
    if (!puddle_group_name_ok(name))
        return fail(code, 400, "bad_name");
    puddle_identity_id(ctx->identity, me);
    int64_t ts = puddle_event_next_ts(puddle_ctx_now_ms(ctx));
    size_t el;
    if (puddle_group_nonce(nonce) != 0 || !(el = puddle_group_create_build(me, ts, nonce, name, gid, ev, sizeof ev)))
        return fail(code, 500, "internal");
    puddle_gevent ge;
    const char *why;
    puddle_group g;
    if (puddle_group_event_parse(ev, el, &ge, &why) != 0 || puddle_event_sign(ctx->identity, ev, el, sig) != 0 ||
        puddle_group_init_created(&g, &ge, puddle_ctx_now_ms(ctx)) != 0)
        return fail(code, 500, "internal");
    snprintf(g.create_event, sizeof g.create_event, "%s", ev);
    snprintf(g.create_sig, sizeof g.create_sig, "%s", sig);
    int rc = 0;
    if (puddle_store_append(ctx->store, gid, ev, el, sig, 0, id, err, sizeof err) < 0 || puddle_groups_put(ctx->groups, &g, err, sizeof err) != 0)
        rc = fail(code, 500, "internal");
    puddle_group_free(&g);
    return rc;
}

/* ---- invite ---- */

/* the invitation: {"group","create":{"event","sig"},"invite":{"event","sig"},"members":[{"id","state","ts","address"}]} */
static char *invite_package(puddle_ctx *ctx, const puddle_group *g, const char *me, const char *invite, size_t il, const char *invite_sig, size_t *len)
{
    size_t cap = 4096 + g->nmembers * 600 + (strlen(g->create_event) + il) * 6;
    char *buf = (char *)malloc(cap);
    if (!buf)
        return NULL;
    puddle_jw w;
    puddle_jw_init(&w, buf, cap);
    puddle_jw_object(&w);
    puddle_jw_str(&w, "group", g->id);
    puddle_jw_key(&w, "create");
    puddle_jw_object(&w);
    puddle_jw_str(&w, "event", g->create_event);
    puddle_jw_str(&w, "sig", g->create_sig);
    puddle_jw_end_object(&w);
    puddle_jw_key(&w, "invite");
    puddle_jw_object(&w);
    puddle_jw_key(&w, "event");
    puddle_jw_string_n(&w, invite, il);
    puddle_jw_str(&w, "sig", invite_sig);
    puddle_jw_end_object(&w);
    puddle_jw_key(&w, "members");
    puddle_jw_array(&w);
    for (size_t i = 0; i < g->nmembers; i++) {
        const puddle_gmember *m = &g->members[i];
        if (m->state == PUDDLE_GM_LEFT)
            continue;
        char addr[PUDDLE_ADDR_MAX] = ""; /* where we reach them, for the members, and for the invited too (they may have said yes by now) */
        if (!strcmp(m->id, me)) {
            puddle_my_address(ctx->config, addr, sizeof addr);
        } else {
            puddle_stream st;
            if (puddle_streams_get(ctx->streams, m->id, &st) == 0) {
                if (st.naddr > 0 && puddle_stream_carries_groups(&st))
                    snprintf(addr, sizeof addr, "%s", st.addresses[0]);
                puddle_stream_free(&st);
            }
        }
        puddle_jw_object(&w);
        puddle_jw_str(&w, "id", m->id);
        puddle_jw_str(&w, "state", puddle_gm_state_name(m->state));
        puddle_jw_num(&w, "ts", m->ts);
        if (addr[0])
            puddle_jw_str(&w, "address", addr);
        puddle_jw_end_object(&w);
    }
    puddle_jw_end_array(&w);
    puddle_jw_end_object(&w);
    *len = puddle_jw_finish(&w);
    if (!*len) {
        free(buf);
        return NULL;
    }
    return buf;
}

int puddle_group_invite(puddle_ctx *ctx, const char *gid, const char *invitee, const char **code)
{
    puddle_group g;
    int status;
    char me[PUDDLE_USER_ID_LEN + 1];
    if (load(ctx, gid, &g, code, &status) != 0)
        return status;
    puddle_identity_id(ctx->identity, me);
    int rc = 0;
    if (g.my != PUDDLE_GROUP_IN) {
        rc = fail(code, 409, "not_in_group");
        goto done;
    }
    if (!puddle_id_valid(invitee) || !strcmp(invitee, me)) {
        rc = fail(code, 400, "bad_request");
        goto done;
    }
    if (!is_friend(ctx, invitee)) {
        rc = fail(code, 403, "no_stream"); /* only a friend can be invited: the invitation travels over that stream */
        goto done;
    }
    puddle_gmember *m; m = puddle_group_member(&g, invitee);
    if (m && m->state != PUDDLE_GM_LEFT) {
        rc = fail(code, 409, "already_in");
        goto done;
    }
    if (puddle_group_count(&g, PUDDLE_GM_MEMBER) + puddle_group_count(&g, PUDDLE_GM_INVITED) >= PUDDLE_GROUP_MAX_MEMBERS) {
        rc = fail(code, 409, "group_full");
        goto done;
    }
    {
        char ev[512], sig[PUDDLE_SIG_TEXT_MAX], id[65], err[300];
        int64_t ts = stamp(ctx, m ? m->ts : 0);
        size_t el = puddle_group_invite_build(g.id, me, ts, invitee, ev, sizeof ev);
        change c = { .set_my = 0 };
        const char *why;
        if (!el || puddle_group_event_parse(ev, el, &c.ev, &why) != 0) {
            rc = fail(code, 500, "internal");
            goto done;
        }
        int ur = puddle_groups_update(ctx->groups, g.id, change_fn, &c, err, sizeof err);
        if (ur != 0) {
            rc = fail(code, ur == 3 ? 409 : 500, ur == 3 ? c.why : "internal");
            goto done;
        }
        puddle_group_free(&g);
        if (puddle_groups_get(ctx->groups, gid, &g) != 0) {
            rc = fail(code, 500, "internal");
            goto done;
        }
        int sent;
        rc = emit(ctx, &g, ev, el, invitee, 1, id, sig, &sent, code); /* the others hear; the invitee gets the invitation itself */
        if (rc != 0 && strcmp(*code, "not_queued") != 0)
            goto done;
        int member_rc = rc;
        size_t pl;
        char *pkg = invite_package(ctx, &g, me, ev, el, sig, &pl);
        if (!pkg || !ctx->outbox || puddle_outbox_enqueue(ctx->outbox, invitee, "group_invite", "/stream/v1/group/invite", pkg, pl, err, sizeof err) != 0) {
            free(pkg);
            rc = fail(code, 500, "not_queued");
            goto done;
        }
        free(pkg);
        if (member_rc != 0) {
            rc = fail(code, 500, "not_queued");
            goto done;
        }
        rc = 0;
    }
done:
    puddle_group_free(&g);
    return rc;
}

/* ---- accept, decline, leave, rename ---- */

int puddle_group_accept(puddle_ctx *ctx, const char *gid, const char **code)
{
    puddle_group g;
    int status;
    char me[PUDDLE_USER_ID_LEN + 1];
    if (load(ctx, gid, &g, code, &status) != 0)
        return status;
    puddle_identity_id(ctx->identity, me);
    int rc = 0;
    if (g.my != PUDDLE_GROUP_INVITED) {
        rc = fail(code, 409, "not_invited");
        goto done;
    }
    for (size_t i = 0; i < g.nmembers; i++)
        if (g.members[i].state != PUDDLE_GM_LEFT && strcmp(g.members[i].id, me) != 0 && puddle_streams_is_blocked(ctx->streams, g.members[i].id)) {
            rc = fail(code, 403, "blocked_member"); /* someone you have blocked is in it */
            goto done;
        }
    {
        char ev[512], sig[PUDDLE_SIG_TEXT_MAX], id[65], err[300];
        puddle_gmember *mine = puddle_group_member(&g, me);
        int64_t ts = stamp(ctx, mine ? mine->ts : 0);
        size_t el = puddle_group_join_build(g.id, me, ts, ev, sizeof ev);
        change c = { .my = PUDDLE_GROUP_IN, .set_my = 1 };
        const char *why;
        if (!el || puddle_group_event_parse(ev, el, &c.ev, &why) != 0 || puddle_event_sign(ctx->identity, ev, el, sig) != 0) {
            rc = fail(code, 500, "internal");
            goto done;
        }
        c.join_event = ev;
        c.join_sig = sig;
        int ur = puddle_groups_update(ctx->groups, g.id, change_fn, &c, err, sizeof err);
        if (ur != 0) {
            rc = fail(code, ur == 3 ? 409 : 500, ur == 3 ? c.why : "internal");
            goto done;
        }
        puddle_group_free(&g);
        if (puddle_groups_get(ctx->groups, gid, &g) != 0) {
            rc = fail(code, 500, "internal");
            goto done;
        }
        /* our join is kept in our log; the members hear it from reconcile, as they become reachable (it also connects us to them) */
        if (puddle_store_append(ctx->store, g.id, ev, el, sig, 0, id, err, sizeof err) < 0) {
            rc = fail(code, 500, "internal");
            goto done;
        }
    }
    puddle_group_reconcile(ctx);
done:
    puddle_group_free(&g);
    return rc;
}

/* our own leave, for decline and leave alike: `to` NULL = every member, else just that one */
static int leave_group(puddle_ctx *ctx, const char *gid, puddle_group_my from, puddle_group_my to_state, const char *only, const char **code)
{
    puddle_group g;
    int status;
    char me[PUDDLE_USER_ID_LEN + 1];
    if (load(ctx, gid, &g, code, &status) != 0)
        return status;
    puddle_identity_id(ctx->identity, me);
    int rc = 0;
    if (g.my != from) {
        rc = fail(code, 409, from == PUDDLE_GROUP_INVITED ? "not_invited" : "not_in_group");
        goto done;
    }
    {
        char ev[512], sig[PUDDLE_SIG_TEXT_MAX], id[65], err[300];
        puddle_gmember *mine = puddle_group_member(&g, me);
        int64_t ts = stamp(ctx, mine ? mine->ts : 0);
        size_t el = puddle_group_leave_build(g.id, me, ts, ev, sizeof ev);
        change c = { .my = to_state, .set_my = 1 };
        const char *why;
        if (!el || puddle_group_event_parse(ev, el, &c.ev, &why) != 0) {
            rc = fail(code, 500, "internal");
            goto done;
        }
        int ur = puddle_groups_update(ctx->groups, g.id, change_fn, &c, err, sizeof err);
        if (ur != 0) {
            rc = fail(code, ur == 3 ? 409 : 500, ur == 3 ? c.why : "internal");
            goto done;
        }
        /* who is told is decided from the group as it was before we left */
        if (only) {
            /* a refusal goes to whoever asked, and to nobody else */
            puddle_group one;
            memset(&one, 0, sizeof one);
            snprintf(one.id, sizeof one.id, "%s", g.id);
            puddle_gmember *m = puddle_group_member(&g, only);
            if (m && puddle_group_open_with(ctx, only)) {
                puddle_gmember copy = *m;
                copy.state = PUDDLE_GM_MEMBER;
                one.members = &copy;
                one.nmembers = 1;
                int sent;
                rc = emit(ctx, &one, ev, el, NULL, 1, id, sig, &sent, code);
            } else {
                if (puddle_event_sign(ctx->identity, ev, el, sig) == 0)
                    puddle_store_append(ctx->store, g.id, ev, el, sig, 0, id, err, sizeof err);
            }
        } else {
            int sent;
            rc = emit(ctx, &g, ev, el, NULL, 1, id, sig, &sent, code);
        }
        if (rc != 0 && strcmp(*code, "not_queued") != 0)
            goto done;
        rc = 0;
    }
    puddle_group_reconcile(ctx);
done:
    puddle_group_free(&g);
    return rc;
}

int puddle_group_decline(puddle_ctx *ctx, const char *gid, const char **code)
{
    puddle_group g;
    int status;
    char inviter[PUDDLE_USER_ID_LEN + 1] = "";
    if (load(ctx, gid, &g, code, &status) != 0)
        return status;
    snprintf(inviter, sizeof inviter, "%s", g.invited_by);
    puddle_group_free(&g);
    return leave_group(ctx, gid, PUDDLE_GROUP_INVITED, PUDDLE_GROUP_DECLINED, inviter[0] ? inviter : NULL, code);
}

int puddle_group_leave(puddle_ctx *ctx, const char *gid, const char **code)
{
    return leave_group(ctx, gid, PUDDLE_GROUP_IN, PUDDLE_GROUP_LEFT, NULL, code);
}

int puddle_group_rename(puddle_ctx *ctx, const char *gid, const char *name, const char **code)
{
    puddle_group g;
    int status;
    char me[PUDDLE_USER_ID_LEN + 1];
    if (!puddle_group_name_ok(name))
        return fail(code, 400, "bad_name");
    if (load(ctx, gid, &g, code, &status) != 0)
        return status;
    puddle_identity_id(ctx->identity, me);
    int rc = 0;
    if (g.my != PUDDLE_GROUP_IN) {
        rc = fail(code, 409, "not_in_group");
        goto done;
    }
    {
        char ev[1024], sig[PUDDLE_SIG_TEXT_MAX], id[65], err[300];
        int64_t ts = stamp(ctx, g.name_ts);
        size_t el = puddle_group_rename_build(g.id, me, ts, name, ev, sizeof ev);
        change c = { .set_my = 0 };
        const char *why;
        if (!el || puddle_group_event_parse(ev, el, &c.ev, &why) != 0) {
            rc = fail(code, 500, "internal");
            goto done;
        }
        int ur = puddle_groups_update(ctx->groups, g.id, change_fn, &c, err, sizeof err);
        if (ur != 0) {
            rc = fail(code, ur == 3 ? 409 : 500, ur == 3 ? c.why : "internal");
            goto done;
        }
        int sent;
        rc = emit(ctx, &g, ev, el, NULL, 1, id, sig, &sent, code);
        if (rc != 0 && strcmp(*code, "not_queued") != 0)
            goto done;
        rc = 0;
    }
done:
    puddle_group_free(&g);
    return rc;
}

int puddle_group_send_event(puddle_ctx *ctx, const char *gid, const char *event, size_t len, char id[65], int *sent, const char **code)
{
    puddle_group g;
    int status;
    char sig[PUDDLE_SIG_TEXT_MAX];
    if (load(ctx, gid, &g, code, &status) != 0)
        return status;
    int rc = 0;
    if (g.my != PUDDLE_GROUP_IN)
        rc = fail(code, 409, "not_in_group");
    else
        rc = emit(ctx, &g, event, len, NULL, 0, id, sig, sent, code);
    puddle_group_free(&g);
    return rc;
}

/* ---- group-only connections ---- */

/* {"requester":<who we are to them>,"scope":{"group","join":{"event","sig"}}} */
static char *scoped_body(puddle_ctx *ctx, const puddle_group *g, const char *peer, size_t *len)
{
    size_t cl = 0;
    char *c = puddle_identity_chain(ctx->identity, &cl);
    free(c);
    size_t cap = cl * 6 + 24 * 1024;
    char *buf = (char *)malloc(cap);
    if (!buf)
        return NULL;
    puddle_jw w;
    puddle_jw_init(&w, buf, cap);
    puddle_jw_object(&w);
    puddle_jw_key(&w, "requester");
    if (puddle_flow_write_self(ctx, peer, &w) != 0) {
        free(buf);
        return NULL;
    }
    puddle_jw_key(&w, "scope");
    puddle_jw_object(&w);
    puddle_jw_str(&w, "group", g->id);
    puddle_jw_key(&w, "join");
    puddle_jw_object(&w);
    puddle_jw_str(&w, "event", g->join_event);
    puddle_jw_str(&w, "sig", g->join_sig);
    puddle_jw_end_object(&w);
    puddle_jw_end_object(&w);
    puddle_jw_end_object(&w);
    *len = puddle_jw_finish(&w);
    if (!*len) {
        free(buf);
        return NULL;
    }
    return buf;
}

typedef struct {
    const char *address;
    int64_t now;
    int resend;
} asking_scoped;

static int ask_fn(void *user, puddle_stream *st, int exists)
{
    asking_scoped *a = (asking_scoped *)user;
    if (exists) {
        if (st->state == PUDDLE_STREAM_CLOSED && !st->group_ok) {
            st->state = PUDDLE_STREAM_PENDING_OUT; /* a closed stream asks again, as a group-only one */
        } else if (st->state == PUDDLE_STREAM_PENDING_OUT && st->scoped_req && a->resend) {
            /* an old request that was never answered: ask again */
        } else {
            return 1;
        }
    } else {
        st->created_ms = a->now;
        st->state = PUDDLE_STREAM_PENDING_OUT;
    }
    st->scoped_req = 1;
    st->owed = PUDDLE_OWED_NONE;
    st->error[0] = '\0';
    st->updated_ms = a->now;
    if (a->address && a->address[0] && puddle_stream_add_address(st, a->address) != 0 && st->naddr == 0)
        return -1;
    return st->naddr > 0 ? 0 : -1;
}

static void request_scoped(puddle_ctx *ctx, const puddle_group *g, const puddle_gmember *m, int resend)
{
    char err[300];
    puddle_address pa;
    const char *addr = m->address[0] && puddle_address_parse(m->address, &pa) == 0 ? pa.address : NULL;
    asking_scoped a = { addr, puddle_ctx_now_ms(ctx), resend };
    if (puddle_streams_update(ctx->streams, m->id, ask_fn, &a, err, sizeof err) != 0)
        return; /* nothing to ask (already asked, or no address to ask at) */
    size_t len;
    char *body = scoped_body(ctx, g, m->id, &len);
    if (!body || puddle_outbox_enqueue(ctx->outbox, m->id, "access", "/stream/v1/access", body, len, err, sizeof err) != 0)
        puddle_log(PUDDLE_LOG_WARN, "cannot queue a group connection to %.12s…", m->id);
    else
        puddle_log(PUDDLE_LOG_INFO, "asking %.12s… for a group connection (group %.12s…)", m->id, g->id);
    free(body);
}

typedef struct {
    int64_t now;
} closing_scoped;

static int release_fn(void *user, puddle_stream *st, int exists)
{
    closing_scoped *c = (closing_scoped *)user;
    if (!exists)
        return 1;
    if (st->state == PUDDLE_STREAM_PENDING_OUT && st->scoped_req) { /* an unanswered group request for a group we are no longer in */
        st->state = PUDDLE_STREAM_CLOSED;
        st->scoped_req = 0;
        st->updated_ms = c->now;
        return 0;
    }
    if (!st->group_ok)
        return 1;
    st->group_ok = 0;
    st->updated_ms = c->now;
    if (st->state == PUDDLE_STREAM_SCOPED) { /* a connection that was only for groups ends with the last of them */
        st->state = PUDDLE_STREAM_CLOSED;
        st->owed = PUDDLE_OWED_CLOSED;
    }
    return 0;
}

typedef struct { const char *peer; } told_ctx;

static int told_fn(void *user, puddle_group *g, int exists)
{
    told_ctx *t = (told_ctx *)user;
    puddle_gmember *m = exists ? puddle_group_member(g, t->peer) : NULL;
    if (!m || m->told)
        return 1;
    m->told = 1;
    return 0;
}

void puddle_group_reconcile(puddle_ctx *ctx)
{
    if (!ctx->groups || !ctx->streams || !ctx->outbox)
        return;
    puddle_group *gl;
    size_t gn;
    if (puddle_groups_list(ctx->groups, &gl, &gn) != 0)
        return;
    char me[PUDDLE_USER_ID_LEN + 1];
    puddle_identity_id(ctx->identity, me);
    int64_t now = puddle_ctx_now_ms(ctx);

    /* connect to the members who have no stream with us: whoever joins reaches the ones already there */
    for (size_t i = 0; i < gn; i++) {
        const puddle_group *g = &gl[i];
        if (g->my != PUDDLE_GROUP_IN || !g->join_event[0])
            continue; /* we made it (everyone connects to us), or we are not in it */
        for (size_t k = 0; k < g->nmembers; k++) {
            const puddle_gmember *m = &g->members[k];
            if (m->state == PUDDLE_GM_LEFT || !strcmp(m->id, me) || puddle_streams_is_blocked(ctx->streams, m->id) || puddle_group_open_with(ctx, m->id))
                continue;
            puddle_stream st;
            int have = puddle_streams_get(ctx->streams, m->id, &st);
            int resend = have == 0 && st.state == PUDDLE_STREAM_PENDING_OUT && st.scoped_req && now - st.updated_ms > 3 * 60 * 1000;
            int waiting = have == 0 && (st.state == PUDDLE_STREAM_PENDING_OUT || st.state == PUDDLE_STREAM_PENDING_IN) && !resend;
            if (have == 0)
                puddle_stream_free(&st);
            if (!waiting)
                request_scoped(ctx, g, m, resend);
        }
    }

    /* tell those we can reach our join, once: a member we meet this way has no other way to learn that we said yes */
    for (size_t i = 0; i < gn; i++) {
        const puddle_group *g = &gl[i];
        if (g->my != PUDDLE_GROUP_IN || !g->join_event[0])
            continue;
        for (size_t k = 0; k < g->nmembers; k++) {
            const puddle_gmember *m = &g->members[k];
            if (m->state == PUDDLE_GM_LEFT || m->told || !strcmp(m->id, me) || !puddle_group_open_with(ctx, m->id))
                continue;
            char id[65], err[300];
            size_t bl;
            puddle_sha256_hex(g->join_event, strlen(g->join_event), id);
            char *body = delivery_body(g->id, id, g->join_event, strlen(g->join_event), g->join_sig, &bl);
            int ok = body && puddle_outbox_enqueue(ctx->outbox, m->id, "events", "/stream/v1/events", body, bl, err, sizeof err) == 0;
            free(body);
            if (ok) {
                told_ctx t = { m->id };
                puddle_groups_update(ctx->groups, g->id, told_fn, &t, err, sizeof err);
                char place[PUDDLE_PLACE_MAX];
                if (puddle_place_group(g->id, place) == 0) /* and who we are in it, if we chose */
                    puddle_profile_push_to(ctx, m->id, place);
            }
        }
    }

    /* let go of the ones we no longer share a group with */
    puddle_stream *sl;
    size_t sn;
    if (puddle_streams_list(ctx->streams, &sl, &sn) == 0) {
        for (size_t i = 0; i < sn; i++) {
            if (!sl[i].group_ok && !(sl[i].state == PUDDLE_STREAM_PENDING_OUT && sl[i].scoped_req))
                continue;
            int shared = 0;
            for (size_t k = 0; k < gn && !shared; k++) {
                const puddle_gmember *m = puddle_group_member(&gl[k], sl[i].id);
                shared = (gl[k].my == PUDDLE_GROUP_IN || gl[k].my == PUDDLE_GROUP_INVITED) && m && m->state != PUDDLE_GM_LEFT; /* a group we are invited to counts: whoever said yes first may have connected to us already */
            }
            if (shared)
                continue;
            closing_scoped c = { now };
            char err[300];
            puddle_streams_update(ctx->streams, sl[i].id, release_fn, &c, err, sizeof err);
            puddle_stream after;
            if (puddle_streams_get(ctx->streams, sl[i].id, &after) == 0) {
                if (after.owed == PUDDLE_OWED_CLOSED && after.state == PUDDLE_STREAM_CLOSED)
                    puddle_flow_owe(ctx, sl[i].id, PUDDLE_OWED_CLOSED);
                puddle_stream_free(&after);
            }
        }
        puddle_stream_list_free(sl, sn);
    }
    puddle_group_list_free(gl, gn);
}
