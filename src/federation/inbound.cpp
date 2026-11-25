#include "federation/inbound.h"

#include "federation/access.h"
#include "federation/flow.h"
#include "base/hash.h"
#include "base/log.h"
#include "federation/profiles_api.h"
#include "core/people.h"
#include "core/outbox.h"
#include "events/message.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the answer to a request we made ---- */

static int result_chain_key(puddle_ctx *ctx, const unsigned char *body, size_t len, const char *sender_id, const char *kid,
                            unsigned char sign_pub[32])
{
    if (!puddle_id_valid(sender_id) || !kid)
        return 0;
    if (ctx->anon_bucket && !puddle_bucket_take(ctx->anon_bucket, puddle_ctx_now_ms(ctx)))
        return 0;
    puddle_span resp;
    puddle_peer_info p;
    if (puddle_json_member((const char *)body, len, "responder", &resp) != 0 || puddle_peer_parse(resp.p, resp.len, &p) != 0)
        return 0;
    int ok = puddle_peer_key(&p, sender_id, kid, sign_pub);
    puddle_peer_info_free(&p);
    return ok;
}

typedef struct {
    const puddle_peer_info *peer;
    int approved;
    int scoped;   /* the answer is yes to a group-only connection */
    int64_t now;
} answer;

static int apply_answer(void *user, puddle_stream *st, int exists)
{
    answer *a = (answer *)user;
    if (!exists || st->state != PUDDLE_STREAM_PENDING_OUT)
        return 1; /* not a request of ours: nothing to answer */
    st->updated_ms = a->now;
    st->error[0] = '\0';
    if (a->scoped && !st->scoped_req)
        return 1; /* a yes to something we did not ask for: it is no one's friend request */
    if (!a->approved) {
        st->state = PUDDLE_STREAM_CLOSED;
        st->scoped_req = 0;
        return 0;
    }
    if (a->scoped) {
        st->state = PUDDLE_STREAM_SCOPED; /* we asked only because of a group: it is for that */
        st->group_ok = 1;
    } else {
        st->state = PUDDLE_STREAM_ACTIVE; /* a friend's yes upgrades anything */
    }
    st->scoped_req = 0;
    puddle_stream_add_address(st, a->peer->address); /* the one from the link stays first */
    snprintf(st->levee_id, sizeof st->levee_id, "%s", a->peer->levee_id);
    snprintf(st->levee_key, sizeof st->levee_key, "%s", a->peer->levee_key);
    if (puddle_stream_set_chain(st, a->peer->chain, a->peer->chain_len) != 0)
        return -1;
    return 0;
}

static int owe_closed(void *user, puddle_stream *st, int exists)
{
    (void)user;
    if (!exists || st->state != PUDDLE_STREAM_CLOSED)
        return 1;
    st->owed = PUDDLE_OWED_CLOSED;
    return 0;
}

/* Whoever signs this has proved they hold the key of the id they claim, which any stranger can: what matters is
 * that we have a record of asking exactly that id (the link pinned it), and then the answer is theirs to give.
 * Everything else is dropped without a word. */
static int result_post(const puddle_call *c, puddle_jw *w)
{
    (void)w;
    puddle_span resp, ap;
    puddle_peer_info p;
    const char *b = (const char *)c->body;
    if (puddle_json_member(b, c->body_len, "responder", &resp) != 0 || puddle_peer_parse(resp.p, resp.len, &p) != 0)
        return 202;
    int approved = puddle_json_member(b, c->body_len, "approved", &ap) == 0 && ap.len == 4 && !memcmp(ap.p, "true", 4);
    int valid_bool = puddle_json_member(b, c->body_len, "approved", &ap) == 0 && (approved || (ap.len == 5 && !memcmp(ap.p, "false", 5)));
    /* An answer to a request we sent to an address says who they are; the ticket in it is the one only they were given. */
    puddle_span tv;
    char ticket[33] = "";
    puddle_span sv;
    int scoped = puddle_json_member(b, c->body_len, "scoped", &sv) == 0 && sv.len == 4 && !memcmp(sv.p, "true", 4);
    if (!scoped && valid_bool && !strcmp(p.id, c->sender) && puddle_json_member(b, c->body_len, "ticket", &tv) == 0 &&
        puddle_json_string(tv, ticket, sizeof ticket) == 0 && ticket[0]) {
        /* a link that gave a sealing key named whose Puddle it is: an answer with another key is someone else's */
        puddle_stream asked;
        if (puddle_streams_find_ticket(c->ctx->streams, ticket, &asked) == 0) {
            int other = 0;
            if (asked.link_key[0]) {
                puddle_chain_head head;
                char herr[160], have[65];
                other = puddle_chain_verify(p.chain, p.chain_len, p.id, &head, herr, sizeof herr) != 0;
                if (!other) {
                    puddle_hex_encode(head.enc_pub, 32, have);
                    other = strcmp(have, asked.link_key) != 0;
                }
            }
            puddle_stream_free(&asked);
            if (other) {
                puddle_log(PUDDLE_LOG_WARN, "an answer to our request to an address came from a key other than its link's; ignored");
                return 202;
            }
        }
        int ar = puddle_flow_adopt(c->ctx, ticket, p.id);
        if (ar == 0) {
            puddle_log(PUDDLE_LOG_DEBUG, "%.12s… answered our request", p.id);
            /* we withdrew the request, and they have said yes anyway: tell them the stream is closed */
            char oerr[200];
            if (approved && puddle_streams_update(c->ctx->streams, p.id, owe_closed, NULL, oerr, sizeof oerr) == 0)
                puddle_flow_owe(c->ctx, p.id, PUDDLE_OWED_CLOSED);
        }
    }
    if (valid_bool && !strcmp(p.id, c->sender)) {
        answer a = { &p, approved, scoped, puddle_call_now_ms(c) };
        char err[300];
        int rc = puddle_streams_update(c->ctx->streams, p.id, apply_answer, &a, err, sizeof err);
        if (rc == 0) {
            puddle_log(PUDDLE_LOG_INFO, "%.12s… %s our request", p.id, approved ? "accepted" : "declined");
            if (approved) { /* a yes carries the profile they show us, if they show us one */
                puddle_profile_keep(c->ctx, p.id, PUDDLE_PEOPLE_DM, p.profile, p.profile_notes);
                puddle_profile_push_groups_to(c->ctx, p.id); /* and now they can hear what we show them in the groups we share */
                puddle_media_want(c->ctx, p.id);
            }
        }
        else if (rc < 0)
            puddle_log(PUDDLE_LOG_WARN, "an answer from %.12s… could not be recorded", p.id);
    }
    puddle_peer_info_free(&p);
    return 202;
}

/* ---- a question about a request they made ---- */

/* Asked by a requester whose Puddle missed our answer. It is chain-proven like the request, because we may hold
 * nothing about them (a request we ignored leaves no trace), and everything that is not a plain yes or no is
 * "pending", so no answer reveals a block, a closed door or even that we saw the request. */
static int status_post(const puddle_call *c, puddle_jw *w)
{
    puddle_span req;
    puddle_peer_info p;
    if (puddle_json_member((const char *)c->body, c->body_len, "requester", &req) != 0 ||
        puddle_peer_parse(req.p, req.len, &p) != 0 || strcmp(p.id, c->sender) != 0) {
        puddle_peer_info_free(&p);
        return puddle_fail(w, 400, "bad_request");
    }
    const char *state = "pending";
    puddle_stream st;
    if (!puddle_streams_is_blocked(c->ctx->streams, p.id) && puddle_streams_get(c->ctx->streams, p.id, &st) == 0) {
        /* their chain is proven to be this id's, which is all "who is asking" needs */
        if (st.state == PUDDLE_STREAM_ACTIVE)
            state = "approved";
        else if (st.state == PUDDLE_STREAM_CLOSED)
            state = "declined"; /* closed by us, whichever way: their request will not go ahead */
        puddle_stream_free(&st);
    }
    puddle_peer_info_free(&p);
    puddle_jw_object(w);
    puddle_jw_str(w, "state", state);
    if (!strcmp(state, "approved")) {
        puddle_jw_key(w, "responder");
        if (puddle_flow_write_self(c->ctx, c->sender, w) != 0)
            return 500;
    }
    puddle_jw_end_object(w);
    return 200;
}

/* ---- "this stream is closed" ---- */

typedef struct { int64_t now; } closing;

static int apply_close(void *user, puddle_stream *st, int exists)
{
    closing *cl = (closing *)user;
    if (!exists || st->state == PUDDLE_STREAM_CLOSED)
        return 1;
    st->state = PUDDLE_STREAM_CLOSED;
    st->owed = PUDDLE_OWED_NONE; /* the stream is over; nothing more to say to them */
    st->updated_ms = cl->now;
    return 0;
}

/* Signed with a key we already hold, so the sender is known and can be told no_stream. */
static int close_post(const puddle_call *c, puddle_jw *w)
{
    closing cl = { puddle_call_now_ms(c) };
    char err[300];
    int rc = puddle_streams_update(c->ctx->streams, c->sender, apply_close, &cl, err, sizeof err);
    if (rc == 0)
        puddle_log(PUDDLE_LOG_INFO, "%.12s… closed the stream", c->sender);
    if (rc == 0 || rc == 1) {
        if (rc == 1)
            return puddle_fail(w, 403, "no_stream");
        return 204;
    }
    return puddle_fail(w, 500, "internal");
}

/* ---- a Levee gave up on something of ours (spec/06, "When the Levee gives up") ---- */

/* signed by a Levee, under its own id: one whose custody receipts we hold */
static int undelivered_chain_key(puddle_ctx *ctx, const unsigned char *body, size_t len, const char *sender_id, const char *kid,
                                 unsigned char sign_pub[32])
{
    (void)body;
    (void)len;
    char want[65];
    if (!ctx->outbox || !kid || !puddle_id_valid(sender_id) || !puddle_outbox_custody_levee(ctx->outbox, sender_id, sign_pub))
        return 0;
    pp_key_id(sign_pub, want);
    return strcmp(want, kid) == 0;
}

/* a message of the owner's is theirs to resend or delete; anything else (an edit, a group's news, an address record) goes again
 * by itself once they are back */
static int resend_by_itself(const puddle_ob_item *it, void *user)
{
    (void)user;
    if (strcmp(it->kind, "events") != 0 || !it->body)
        return 1;
    puddle_span evs, one, v;
    size_t cur = 0;
    if (puddle_json_member(it->body, it->body_len, "events", &evs) != 0)
        return 1;
    while (puddle_json_array_next(evs, &cur, &one) == 1) {
        if (puddle_json_member(one.p, one.len, "event", &v) != 0 || v.len < 2)
            continue;
        char *ev = (char *)malloc(v.len + 1);
        puddle_message m;
        const char *why = NULL;
        int message = ev && puddle_json_string(v, ev, v.len + 1) == 0 && puddle_chat_event_parse(ev, strlen(ev), &m, &why) == 0 &&
                      m.type == PUDDLE_EV_MESSAGE;
        if (ev && message)
            puddle_message_free(&m);
        free(ev);
        if (message)
            return 0;
    }
    return 1;
}

static int undelivered_post(const puddle_call *c, puddle_jw *w)
{
    const char *b = (const char *)c->body;
    size_t n = c->body_len;
    puddle_span items, it, v;
    char for_id[PUDDLE_USER_ID_LEN + 1] = "", levee[PUDDLE_USER_ID_LEN + 1] = "";
    (void)w;
    if (!puddle_json_valid_object(b, n) || puddle_json_member(b, n, "for", &v) != 0 || puddle_json_string(v, for_id, sizeof for_id) != 0 ||
        puddle_json_member(b, n, "levee", &v) != 0 || puddle_json_string(v, levee, sizeof levee) != 0 || strcmp(levee, c->sender) != 0 ||
        puddle_json_member(b, n, "items", &items) != 0)
        return 204; /* answered alike whatever it says: a Levee is told nothing */
    size_t cur = 0;
    int taken = 0, seen = 0;
    while (puddle_json_array_next(items, &cur, &it) == 1 && seen++ < 1000) {
        char nonce[64] = "", hash[72] = "", sig[128] = "", kind[32] = "";
        uint64_t seq = 0;
        if (puddle_json_member(it.p, it.len, "nonce", &v) != 0 || puddle_json_string(v, nonce, sizeof nonce) != 0 ||
            puddle_json_member(it.p, it.len, "sha256", &v) != 0 || puddle_json_string(v, hash, sizeof hash) != 0 ||
            puddle_json_member(it.p, it.len, "sig", &v) != 0 || puddle_json_string(v, sig, sizeof sig) != 0)
            continue;
        /* only an item we hold a custody receipt from this Levee for counts; the rest are ignored */
        if (puddle_outbox_undelivered(c->ctx->outbox, for_id, levee, nonce, hash, sig, resend_by_itself, NULL, kind, &seq) == 1) {
            taken++;
            puddle_log(PUDDLE_LOG_INFO, "%.12s…'s Levee gave up on our %s before they came back: not delivered", for_id, kind);
        }
    }
    if (!taken)
        puddle_log(PUDDLE_LOG_DEBUG, "a notice from the Levee %.12s… named nothing we hold its custody receipt for", levee);
    return 204;
}

const puddle_route puddle_inbound_routes[] = {
    { "POST", "/stream/v1/access/result", PUDDLE_AUTH_CHAIN, PUDDLE_ACCESS_BODY_MAX, result_post, result_chain_key },
    { "POST", "/stream/v1/access/status", PUDDLE_AUTH_CHAIN_ANON, PUDDLE_ACCESS_BODY_MAX, status_post, puddle_access_chain_key },
    { "POST", "/stream/v1/close",         PUDDLE_AUTH_PEER,  4096, close_post, NULL },
    { "POST", "/stream/v1/undelivered",   PUDDLE_AUTH_CHAIN_ANON, 128 * 1024, undelivered_post, undelivered_chain_key },
};
const size_t puddle_inbound_routes_count = sizeof puddle_inbound_routes / sizeof puddle_inbound_routes[0];
