#include "federation/access.h"

#include "http/client.h"
#include "base/encoding.h"
#include "events/message.h"
#include "wire/wireprofile.h"
#include "federation/flow.h"
#include "federation/groupflow.h"
#include "federation/profiles_api.h"
#include "core/people.h"
#include "base/log.h"
#include "base/text.h"
#include "events/group_events.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- reading a request ---- */

typedef struct {
    puddle_peer_info peer;
    char message[PUDDLE_NOTE_MAX_BYTES + 1];
    char invite[PUDDLE_INVITE_TOKEN_LEN + 1];
    char ticket[33];
    int has_scope;
    char scope_group[PUDDLE_USER_ID_LEN + 1]; /* a group-only request: the group, and our own member.join proving we were invited */
    char join_event[600];
    char join_sig[180];
} request;

static void request_free(request *r) { puddle_peer_info_free(&r->peer); }

static int string_member(puddle_span obj, const char *key, char *out, size_t cap)
{
    puddle_span v;
    return puddle_json_member(obj.p, obj.len, key, &v) == 0 && puddle_json_string(v, out, cap) == 0 ? 0 : -1;
}

/* ---- a person as one side tells it to the other: id, address, profile, chain ---- */

void puddle_peer_info_free(puddle_peer_info *p)
{
    free(p->chain);
    p->chain = NULL;
}

int puddle_peer_parse(const char *obj, size_t len, puddle_peer_info *p)
{
    memset(p, 0, sizeof *p);
    puddle_span o = { obj, len }, prof, v;
    if (len < 2 || obj[0] != '{')
        return -1;
    if (string_member(o, "id", p->id, sizeof p->id) != 0 || !puddle_id_valid(p->id))
        return -1;
    char addr[PUDDLE_ADDR_MAX];
    puddle_address pa;
    if (string_member(o, "address", addr, sizeof addr) != 0 || puddle_address_parse(addr, &pa) != 0 || !puddle_net_host_plausible(pa.host))
        return -1;
    snprintf(p->address, sizeof p->address, "%s", pa.address);
    int prc = puddle_json_member(obj, len, "profile", &prof);
    if (prc < 0)
        return -1;
    if (prc == 0 && !(prof.len == 4 && !memcmp(prof.p, "null", 4)) &&
        puddle_profile_wire_clean(prof.p, prof.len, p->profile, sizeof p->profile, p->profile_notes, sizeof p->profile_notes) != 0) {
        p->profile[0] = '\0'; /* not a profile at all: they are still who they say, only without one */
        snprintf(p->profile_notes, sizeof p->profile_notes, "profile: not something we can read; left out");
    }
    if (puddle_json_member(obj, len, "chain", &v) != 0 || v.len < 2 || v.p[0] != '"' || v.len > PUDDLE_ACCESS_CHAIN_MAX * 6u)
        return -1;
    p->chain = (char *)malloc(v.len + 1);
    if (!p->chain || puddle_json_string(v, p->chain, v.len + 1) != 0)
        goto bad;
    p->chain_len = strlen(p->chain);
    if (p->chain_len == 0 || p->chain_len > PUDDLE_ACCESS_CHAIN_MAX)
        goto bad;
    return 0;
bad:
    puddle_peer_info_free(p);
    return -1;
}

int puddle_peer_key(const puddle_peer_info *p, const char *sender_id, const char *kid, unsigned char sign_pub[32])
{
    if (!p->chain || strcmp(p->id, sender_id) != 0) /* the signature must be this person's own */
        return 0;
    puddle_chain_head head;
    char err[200], want[65];
    if (puddle_chain_verify(p->chain, p->chain_len, p->id, &head, err, sizeof err) != 0)
        return 0;
    puddle_key_id(head.sign_pub, want);
    if (strcmp(want, kid) != 0)
        return 0;
    memcpy(sign_pub, head.sign_pub, 32);
    return 1;
}

static int parse_request(const unsigned char *body, size_t len, request *r)
{
    memset(r, 0, sizeof *r);
    const char *b = (const char *)body;
    puddle_span req, v;
    if (puddle_json_member(b, len, "requester", &req) != 0 || puddle_peer_parse(req.p, req.len, &r->peer) != 0)
        return -1;
    int rc = puddle_json_member(b, len, "message", &v);
    if (rc < 0)
        goto bad;
    if (rc == 0) {
        char *raw = (char *)malloc(v.len + 1);
        size_t chars;
        int ok = raw && puddle_json_string(v, raw, v.len + 1) == 0 &&
                 puddle_clean_text(raw, strlen(raw), PUDDLE_NOTE_MAX_CHARS, 1, r->message, sizeof r->message, &chars) == 0;
        free(raw);
        if (!ok)
            goto bad;
    }
    rc = puddle_json_member(b, len, "invite", &v);
    if (rc < 0)
        goto bad;
    if (rc == 0 && (puddle_json_string(v, r->invite, sizeof r->invite) != 0 || !puddle_invite_token_valid(r->invite)))
        r->invite[0] = '\0'; /* a malformed token is just no token: the request still stands */
    rc = puddle_json_member(b, len, "ticket", &v);
    if (rc < 0)
        goto bad;
    {
        unsigned char raw[16];
        if (rc == 0 && (puddle_json_string(v, r->ticket, sizeof r->ticket) != 0 || puddle_hex_decode(r->ticket, raw, 16) != 0))
            r->ticket[0] = '\0'; /* not a ticket as we make them: they just get no ticket back */
    }
    rc = puddle_json_member(b, len, "scope", &v);
    if (rc < 0)
        goto bad;
    r->has_scope = rc == 0;
    if (r->has_scope) {
        puddle_span jv;
        if (string_member(v, "group", r->scope_group, sizeof r->scope_group) != 0 || !puddle_id_valid(r->scope_group) ||
            puddle_json_member(v.p, v.len, "join", &jv) != 0 || string_member(jv, "event", r->join_event, sizeof r->join_event) != 0 ||
            string_member(jv, "sig", r->join_sig, sizeof r->join_sig) != 0)
            goto bad;
    }
    return 0;
bad:
    request_free(r);
    return -1;
}

/* ---- the chain lookup for the signature check ---- */

int puddle_access_chain_key(puddle_ctx *ctx, const unsigned char *body, size_t body_len, const char *sender_id,
                            const char *kid, unsigned char sign_pub[32])
{
    /* Everything below costs signature checks on input from a stranger: cheap refusals first. */
    if (!puddle_id_valid(sender_id) || !kid)
        return 0;
    if (ctx->anon_bucket && !puddle_bucket_take(ctx->anon_bucket, puddle_ctx_now_ms(ctx)))
        return 0;
    request r;
    if (parse_request(body, body_len, &r) != 0)
        return 0;
    int ok = puddle_peer_key(&r.peer, sender_id, kid, sign_pub);
    request_free(&r);
    return ok;
}

/* ---- what is done with a request that is really from who it says ---- */

static void refuse(const char *why, const char *id)
{
    puddle_log(PUDDLE_LOG_INFO, "stream request from %.12s… dropped: %s", id, why);
}

typedef struct {
    const request *r;
    int approve;
    int64_t now;
} apply;

static int apply_request(void *user, puddle_stream *st, int exists)
{
    apply *a = (apply *)user;
    if (exists && st->state == PUDDLE_STREAM_ACTIVE)
        return 1;
    st->state = a->approve ? PUDDLE_STREAM_ACTIVE : PUDDLE_STREAM_PENDING_IN;
    st->owed = a->approve ? PUDDLE_OWED_APPROVED : PUDDLE_OWED_NONE; /* an approval has to be told to them */
    st->updated_ms = a->now;
    if (!exists)
        st->created_ms = a->now;
    snprintf(st->note, sizeof st->note, "%s", a->r->message);
    snprintf(st->ticket, sizeof st->ticket, "%s", a->r->ticket); /* handed back when we answer */
    st->addr_seq = 0;
    st->naddr = 0;
    puddle_stream_add_address(st, a->r->peer.address);
    if (puddle_stream_set_chain(st, a->r->peer.chain, a->r->peer.chain_len) != 0)
        return -1;
    return 0;
}

/* ---- a group-only connection ---- */

typedef struct {
    const request *r;
    int64_t now;
    int stranger; /* we held nothing of them: what they sent (their profile too) is what we keep */
} scoping;

static int apply_scoped(void *user, puddle_stream *st, int exists)
{
    scoping *a = (scoping *)user;
    if (!exists) {
        st->created_ms = a->now;
        st->state = PUDDLE_STREAM_SCOPED;
    } else if (st->state == PUDDLE_STREAM_CLOSED || st->state == PUDDLE_STREAM_SCOPED || (st->state == PUDDLE_STREAM_PENDING_OUT && st->scoped_req)) {
        st->state = PUDDLE_STREAM_SCOPED; /* anything else they are to us (a friend, or asking to be) stays as it is */
        st->scoped_req = 0;               /* and if we had asked them for the same, this is the answer */
    }
    st->group_ok = 1;
    st->owed = PUDDLE_OWED_SCOPED; /* the yes has to be told to them */
    st->updated_ms = a->now;
    if (st->naddr == 0 || !st->chain) { /* what we hold of a stranger is what they sent */
        st->addr_seq = 0;
        st->naddr = 0;
        puddle_stream_add_address(st, a->r->peer.address);
        if (puddle_stream_set_chain(st, a->r->peer.chain, a->r->peer.chain_len) != 0)
            return -1;
        a->stranger = 1;
    } else {
        puddle_stream_add_address(st, a->r->peer.address);
    }
    return 0;
}

typedef struct {
    puddle_gevent ev;
    const char *why;
} joining;

static int join_fn(void *user, puddle_group *g, int exists)
{
    joining *j = (joining *)user;
    /* In, or invited and not yet answered: whoever said yes first asks the others, who may not have got to it. A group we have left or
     * turned down is not one we are in at all. */
    if (!exists || (g->my != PUDDLE_GROUP_IN && g->my != PUDDLE_GROUP_INVITED)) {
        j->why = "not_in_group";
        return 3;
    }
    int rc = puddle_group_apply(g, &j->ev, &j->why);
    if (rc < 0)
        return 3;
    if (rc > 0)
        return 1; /* they were in already */
    g->updated_ms = j->ev.ts;
    return 0;
}

/* Someone asks for a connection that is only for a group we are both in (or are invited to and have not answered yet): their own member.join, signed, is the
 * proof that someone in the group invited them. Whatever the outcome the answer is the usual empty 202. */
static void scoped_request(puddle_ctx *ctx, const request *r, int64_t now)
{
    const char *id = r->peer.id;
    char me[PUDDLE_USER_ID_LEN + 1], err[300], eid[65];
    puddle_identity_id(ctx->identity, me);
    if (!ctx->groups || !ctx->store || !strcmp(id, me)) {
        refuse("not a group request", id);
        return;
    }
    /* the join is theirs: the right chat, from them, and signed by the key in the chain they sent */
    joining j;
    const char *why;
    if (puddle_group_event_parse(r->join_event, strlen(r->join_event), &j.ev, &why) != 0 || j.ev.type != PUDDLE_GE_JOIN ||
        strcmp(j.ev.chat, r->scope_group) != 0 || strcmp(j.ev.sender, id) != 0) {
        refuse("the join is not theirs", id);
        return;
    }
    {
        puddle_chain_head head;
        char kid[65], want[65];
        unsigned char sig[64];
        if (puddle_chain_verify(r->peer.chain, r->peer.chain_len, id, &head, err, sizeof err) != 0 || puddle_event_sig_parse(r->join_sig, kid, sig) != 0) {
            refuse("the join is not signed", id);
            return;
        }
        puddle_key_id(head.sign_pub, want);
        if (strcmp(kid, want) != 0 || !puddle_event_verify(head.sign_pub, r->join_event, strlen(r->join_event), sig)) {
            refuse("the join is not signed by them", id);
            return;
        }
    }
    if (puddle_streams_size(ctx->streams) >= (ctx->max_peers ? ctx->max_peers : PUDDLE_MAX_PEERS)) {
        puddle_stream have;
        int known = puddle_streams_get(ctx->streams, id, &have) == 0;
        if (known)
            puddle_stream_free(&have);
        if (!known) {
            refuse("too many people on record", id);
            return;
        }
    }
    int gr = puddle_groups_update(ctx->groups, r->scope_group, join_fn, &j, err, sizeof err);
    if (gr == 3 || gr < 0) {
        refuse(gr == 3 && j.why ? j.why : "unknown group", id);
        return;
    }
    if (gr == 0) {
        puddle_store_append(ctx->store, r->scope_group, r->join_event, strlen(r->join_event), r->join_sig, now, eid, err, sizeof err);
        puddle_log(PUDDLE_LOG_INFO, "%.12s… joined group %.12s…", id, r->scope_group);
    }
    scoping s = { r, now, 0 };
    int urc = puddle_streams_update(ctx->streams, id, apply_scoped, &s, err, sizeof err);
    if (urc == 0) {
        puddle_profile_keep(ctx, id, PUDDLE_PEOPLE_DM, r->peer.profile, r->peer.profile_notes); /* what they show us directly, if anything */
        puddle_flow_owe(ctx, id, PUDDLE_OWED_SCOPED);
        puddle_profile_push_groups_to(ctx, id); /* and, after the yes, what we show them in the groups we share */
        puddle_media_want(ctx, id);
        puddle_group_reconcile(ctx); /* now we can tell them that we are in the group too */
    } else {
        puddle_log(PUDDLE_LOG_WARN, "a group connection from %.12s… could not be recorded", id);
    }
}

int puddle_access_post(const puddle_call *call, puddle_jw *w)
{
    (void)w; /* the answer is always empty */
    puddle_ctx *ctx = call->ctx;
    int64_t now = puddle_call_now_ms(call);
    request r;
    if (parse_request(call->body, call->body_len, &r) != 0 || strcmp(r.peer.id, call->sender) != 0)
        return 202; /* cannot happen after authentication, but never trust that */
    const char *id = r.peer.id;
    puddle_stream rec;
    int have = -1;

    if (puddle_streams_is_blocked(ctx->streams, id)) {
        refuse("blocked", id);
        goto done;
    }
    if (ctx->access_limit && !puddle_keyed_hit(ctx->access_limit, id, now)) {
        refuse("too many requests from them", id);
        goto done;
    }
    if (r.has_scope) {
        scoped_request(ctx, &r, now);
        goto done;
    }

    {
        char me[PUDDLE_USER_ID_LEN + 1];
        puddle_identity_id(ctx->identity, me);
        if (!strcmp(id, me)) {
            refuse("that is ourselves", id);
            goto done;
        }
    }
    /* We may have asked this very Puddle, by its address, while it asked us: their request tells us who they are */
    if (puddle_streams_find_provisional(ctx->streams, r.peer.address, &rec) == 0) {
        char pid[PUDDLE_USER_ID_LEN + 1];
        memcpy(pid, rec.id, sizeof pid);
        puddle_stream_free(&rec);
        char err[200];
        if (puddle_streams_adopt(ctx->streams, pid, id, err, sizeof err) == 0)
            puddle_log(PUDDLE_LOG_INFO, "%.12s… is the Puddle we asked at %s", id, r.peer.address);
    }

    have = puddle_streams_get(ctx->streams, id, &rec);
    int mutual; mutual = have == 0 && rec.state == PUDDLE_STREAM_PENDING_OUT; /* we asked them, and now they ask us */
    puddle_access policy; policy = puddle_streams_access(ctx->streams);
    if (have == 0 && rec.state == PUDDLE_STREAM_ACTIVE) {
        refuse("already a stream", id);
        goto done;
    }
    if (policy == PUDDLE_ACCESS_CLOSED && !mutual) { /* "closed" means ignore strangers, not people we asked */
        refuse("the Puddle is not taking requests", id);
        goto done;
    }
    if (have != 0 && puddle_streams_size(ctx->streams) >= (ctx->max_peers ? ctx->max_peers : PUDDLE_MAX_PEERS)) {
        refuse("too many people on record", id);
        goto done;
    }

    /* Who gets in at once: someone we asked ourselves, anyone if the policy is open, whoever brings a good
     * invite (spent only when it is what lets them in); everyone else waits for the owner. */
    int approve; approve = mutual || policy == PUDDLE_ACCESS_OPEN ||
                  (r.invite[0] && puddle_streams_invite_consume(ctx->streams, r.invite, now));
    if (!approve && !(have == 0 && rec.state == PUDDLE_STREAM_PENDING_IN) &&
        puddle_streams_count(ctx->streams, PUDDLE_STREAM_PENDING_IN) >= PUDDLE_MAX_PENDING_IN) {
        refuse("too many requests waiting", id);
        goto done;
    }

    {
        /* fold the request into whatever is on file at the moment of writing, so what other threads have set
         * (a certificate noted, an answer owed) is kept and a stream that became active meanwhile is not touched */
        apply a = { &r, approve, now };
        char err[300];
        int urc = puddle_streams_update(ctx->streams, id, apply_request, &a, err, sizeof err);
        if (urc == 0) {
            puddle_log(PUDDLE_LOG_INFO, "stream request from %.12s… %s", id, approve ? "approved" : "waiting for the owner");
            puddle_profile_keep(ctx, id, PUDDLE_PEOPLE_DM, r.peer.profile, r.peer.profile_notes); /* the owner sees who is asking */
            if (approve) {
                puddle_flow_owe(ctx, id, PUDDLE_OWED_APPROVED);
                puddle_profile_push_groups_to(ctx, id);
                puddle_media_want(ctx, id);
            }
        }
        else if (urc < 0)
            puddle_log(PUDDLE_LOG_WARN, "stream request from %.12s… could not be recorded", id);
    }
done:
    if (have == 0)
        puddle_stream_free(&rec);
    request_free(&r);
    return 202;
}

const puddle_route puddle_access_routes[] = {
    { "POST", "/stream/v1/access", PUDDLE_AUTH_CHAIN_OPEN, PUDDLE_ACCESS_BODY_MAX, puddle_access_post, puddle_access_chain_key },
};
const size_t puddle_access_routes_count = sizeof puddle_access_routes / sizeof puddle_access_routes[0];
