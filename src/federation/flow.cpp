#include "federation/flow.h"

#include "base/text.h"
#include "federation/access.h"
#include "federation/keysync.h"
#include "federation/profiles_api.h"
#include "base/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- what we say about ourselves ---- */

/* {"id","address","profile","chain"}: who we are, with the profile `peer` is shown directly, if the owner chose one for them
 * (and no "profile" at all if not: nothing goes anywhere without that choice) */
int puddle_flow_write_self(puddle_ctx *ctx, const char *peer, puddle_jw *w)
{
    char me[PUDDLE_USER_ID_LEN + 1], addr[PUDDLE_ADDR_MAX];
    if (!puddle_my_address(ctx->config, addr, sizeof addr))
        return -1;
    size_t cl;
    char *chain = puddle_identity_chain(ctx->identity, &cl);
    if (!chain)
        return -1;
    puddle_identity_id(ctx->identity, me);
    char place[PUDDLE_PLACE_MAX];
    puddle_profile pr;
    int shown = puddle_place_dm(peer, place) == 0 && puddle_profile_for_place(ctx->profiles, ctx->assignments, place, &pr) == 0;
    puddle_jw_object(w);
    puddle_jw_str(w, "id", me);
    puddle_jw_str(w, "address", addr);
    if (shown) {
        puddle_jw_key(w, "profile");
        puddle_jw_object(w);
        puddle_profile_write_fields(w, &pr);
        puddle_jw_end_object(w);
        puddle_profile_free(&pr);
    }
    puddle_jw_key(w, "chain");
    puddle_jw_string_n(w, chain, cl);
    puddle_jw_end_object(w);
    free(chain);
    return 0;
}

#define BODY_ROOM(chain_estimate) ((chain_estimate) * 6 + PUDDLE_PROFILE_JSON_MAX + 24 * 1024)

static char *build_body(puddle_ctx *ctx, const char *peer, const char *kind, const char *message, const char *invite, const char *ticket,
                        int approved, int scoped, size_t *len)
{
    size_t cl = 0;
    char *c = puddle_identity_chain(ctx->identity, &cl);
    free(c);
    size_t cap = BODY_ROOM(cl);
    char *buf = (char *)malloc(cap);
    if (!buf)
        return NULL;
    puddle_jw w;
    puddle_jw_init(&w, buf, cap);
    puddle_jw_object(&w);
    if (!strcmp(kind, "access")) {
        puddle_jw_key(&w, "requester");
        if (puddle_flow_write_self(ctx, peer, &w) != 0)
            goto bad;
        if (message && *message)
            puddle_jw_str(&w, "message", message);
        if (invite && *invite)
            puddle_jw_str(&w, "invite", invite);
        if (ticket && *ticket)
            puddle_jw_str(&w, "ticket", ticket);
    } else if (!strcmp(kind, "result")) {
        puddle_jw_key(&w, "responder");
        if (puddle_flow_write_self(ctx, peer, &w) != 0)
            goto bad;
        puddle_jw_flag(&w, "approved", approved);
        if (scoped)
            puddle_jw_flag(&w, "scoped", 1); /* yes to a group-only connection: not an answer to anyone's friend request */
        if (ticket && *ticket && !scoped)
            puddle_jw_str(&w, "ticket", ticket); /* what they put in their request, so they know which one this answers */
    }
    puddle_jw_end_object(&w);
    size_t n; n = puddle_jw_finish(&w);
    if (!n)
        goto bad;
    *len = n;
    return buf;
bad:
    free(buf);
    return NULL;
}

/* ---- asking ---- */

typedef struct {
    const puddle_link *link;
    const char *message;
    const char *ticket;
    int64_t now;
} asking;

static int apply_asking(void *user, puddle_stream *st, int exists)
{
    asking *a = (asking *)user;
    if (exists) {
        if (st->state == PUDDLE_STREAM_ACTIVE) return 2;
        if (st->state == PUDDLE_STREAM_PENDING_OUT) return 3;
        if (st->state == PUDDLE_STREAM_PENDING_IN) return 4;
    } else {
        st->created_ms = a->now;
    }
    st->state = PUDDLE_STREAM_PENDING_OUT;
    st->provisional = 1;
    snprintf(st->ticket, sizeof st->ticket, "%s", a->ticket);
    st->owed = PUDDLE_OWED_NONE;
    st->error[0] = '\0';
    st->updated_ms = a->now;
    snprintf(st->note, sizeof st->note, "%s", a->message);
    st->naddr = 0;
    st->addr_seq = 0;
    if (puddle_stream_add_address(st, a->link->address.address) != 0)
        return -1;
    return 0;
}

/* is there already a stream, or a request either way, with the Puddle at this address? */
static int known_at(puddle_ctx *ctx, const char *address, puddle_stream_state *state)
{
    puddle_stream *all;
    size_t n;
    int found = 0;
    if (puddle_streams_list(ctx->streams, &all, &n) != 0)
        return 0;
    for (size_t i = 0; i < n && !found; i++)
        for (int k = 0; k < all[i].naddr; k++)
            if (!strcmp(all[i].addresses[k], address) && all[i].state != PUDDLE_STREAM_CLOSED && all[i].state != PUDDLE_STREAM_SCOPED) {
                *state = all[i].state;
                found = 1;
                break;
            }
    puddle_stream_list_free(all, n);
    return found;
}

int puddle_flow_request(puddle_ctx *ctx, const puddle_link *link, const char *message, const char *profile_id,
                        char out_id[PUDDLE_USER_ID_LEN + 1], const char **code, const char **msg_out)
{
    char addr[PUDDLE_ADDR_MAX], clean[PUDDLE_NOTE_MAX_BYTES + 1], err[300];
    size_t chars;
    *msg_out = "";
    if (puddle_clean_text(message ? message : "", message ? strlen(message) : 0, PUDDLE_NOTE_MAX_CHARS, 1, clean, sizeof clean, &chars) != 0) {
        *code = "bad_request";
        *msg_out = "the message is too long (500 characters at most)";
        return 400;
    }
    if (profile_id && (!ctx->profiles || !puddle_profiles_has(ctx->profiles, profile_id))) {
        *code = "unknown_profile";
        return 400;
    }
    if (!puddle_my_address(ctx->config, addr, sizeof addr)) {
        *code = "no_address";
        *msg_out = "this Puddle doesn't know its public address, so the other side couldn't answer; set `advertise: host:port` in puddle.yaml";
        return 409;
    }
    if (!strcmp(addr, link->address.address)) {
        *code = "cannot_request_yourself";
        return 400;
    }
    if (!ctx->outbox) {
        *code = "internal";
        return 500;
    }
    puddle_stream_state have;
    if (known_at(ctx, link->address.address, &have)) {
        *code = have == PUDDLE_STREAM_ACTIVE ? "already_streaming" : have == PUDDLE_STREAM_PENDING_OUT ? "already_requested" : "they_asked";
        if (have == PUDDLE_STREAM_PENDING_IN)
            *msg_out = "they have already asked you; accept their request";
        return 409;
    }

    /* nobody has said who lives there: the request is filed under an id made up from a ticket, which their answer repeats */
    char ticket[33], pid[PUDDLE_USER_ID_LEN + 1];
    if (puddle_ticket_new(ticket) != 0 || puddle_provisional_id(ticket, pid) != 0) {
        *code = "internal";
        return 500;
    }
    asking a = { link, clean, ticket, puddle_ctx_now_ms(ctx) };
    int rc = puddle_streams_update(ctx->streams, pid, apply_asking, &a, err, sizeof err);
    if (rc != 0) {
        *code = "internal";
        return 500;
    }
    char place[PUDDLE_PLACE_MAX];
    if (profile_id && puddle_place_dm(pid, place) == 0) /* the owner's choice of who they are to them: what the request carries */
        puddle_assignments_set(ctx->assignments, place, profile_id, err, sizeof err);

    size_t len;
    char *body = build_body(ctx, pid, "access", clean, link->invite, ticket, 0, 0, &len);
    if (!body || puddle_outbox_enqueue(ctx->outbox, pid, PUDDLE_OB_KIND_OPEN_ACCESS, "/stream/v1/access", body, len, err, sizeof err) != 0) {
        free(body);
        puddle_streams_forget(ctx->streams, pid);
        *code = "internal";
        return 500;
    }
    free(body);
    memcpy(out_id, pid, PUDDLE_USER_ID_LEN + 1);
    return 0;
}

/* The answer to a provisional request has said who lives there: refile everything under their id. 0 if the record is now
 * theirs, else nonzero (a record for them already existed, or they are blocked, and the request is dropped). */
int puddle_flow_adopt(puddle_ctx *ctx, const char *ticket, const char *real_id)
{
    puddle_stream st;
    char err[200], mine[PUDDLE_PROFILE_ID_MAX + 2];
    if (puddle_streams_find_ticket(ctx->streams, ticket, &st) != 0)
        return 1;
    char pid[PUDDLE_USER_ID_LEN + 1];
    memcpy(pid, st.id, sizeof pid);
    puddle_stream_free(&st);
    char was[PUDDLE_PLACE_MAX], now[PUDDLE_PLACE_MAX];
    int have_profile = ctx->assignments && puddle_place_dm(pid, was) == 0 && puddle_assignments_get(ctx->assignments, was, mine) == 0;
    int rc = puddle_streams_adopt(ctx->streams, pid, real_id, err, sizeof err);
    if (have_profile) { /* the choice made for the made-up id is theirs now */
        if (rc == 0 && puddle_place_dm(real_id, now) == 0)
            puddle_assignments_set(ctx->assignments, now, mine, err, sizeof err);
        puddle_assignments_set(ctx->assignments, was, NULL, err, sizeof err);
    }
    return rc;
}

/* ---- owing ---- */

int puddle_flow_owe(puddle_ctx *ctx, const char *peer, puddle_owed owed)
{
    if (!ctx->outbox || owed == PUDDLE_OWED_NONE)
        return -1;
    char err[300];
    size_t len = 0;
    char *body = NULL;
    const char *kind = owed == PUDDLE_OWED_CLOSED ? "close" : "result";
    const char *path = owed == PUDDLE_OWED_CLOSED ? "/stream/v1/close" : "/stream/v1/access/result";
    if (owed == PUDDLE_OWED_CLOSED) {
        body = strdup("{}");
        len = 2;
    } else {
        puddle_stream st;
        char ticket[33] = "";
        if (puddle_streams_get(ctx->streams, peer, &st) == 0) {
            memcpy(ticket, st.ticket, sizeof ticket);
            puddle_stream_free(&st);
        }
        body = build_body(ctx, peer, "result", NULL, NULL, ticket, owed == PUDDLE_OWED_APPROVED || owed == PUDDLE_OWED_SCOPED, owed == PUDDLE_OWED_SCOPED, &len);
    }
    int rc = body && puddle_outbox_enqueue(ctx->outbox, peer, kind, path, body, len, err, sizeof err) == 0 ? 0 : -1;
    if (rc != 0)
        puddle_log(PUDDLE_LOG_WARN, "cannot queue the %s for %.12s…; it will be tried again at the next start", kind, peer);
    free(body);
    return rc;
}

int puddle_flow_resume(puddle_ctx *ctx)
{
    if (!ctx->outbox)
        return 0;
    puddle_stream *all;
    size_t n;
    puddle_ob_item *items;
    size_t ni;
    if (puddle_streams_list(ctx->streams, &all, &n) != 0)
        return 0;
    if (puddle_outbox_list(ctx->outbox, &items, &ni) != 0) {
        puddle_stream_list_free(all, n);
        return 0;
    }
    int queued = 0;
    for (size_t i = 0; i < n; i++) {
        if (all[i].owed == PUDDLE_OWED_NONE)
            continue;
        const char *kind = all[i].owed == PUDDLE_OWED_CLOSED ? "close" : "result";
        int waiting = 0;
        for (size_t k = 0; k < ni; k++)
            if (items[k].state != PUDDLE_OB_FAILED && !strcmp(items[k].peer, all[i].id) && !strcmp(items[k].kind, kind))
                waiting = 1;
        if (!waiting && puddle_flow_owe(ctx, all[i].id, all[i].owed) == 0)
            queued++;
    }
    puddle_outbox_list_free(items, ni);
    puddle_stream_list_free(all, n);
    return queued;
}

/* ---- how it turned out ---- */

typedef struct {
    puddle_owed settled;      /* the debt this item was paying */
    const char *error;        /* "" or what to show the owner */
    int closes_request;       /* an `access` that was refused */
    int only_error;           /* just note the trouble: change nothing else */
} outcome_note;

static int apply_outcome(void *user, puddle_stream *st, int exists)
{
    outcome_note *o = (outcome_note *)user;
    if (!exists)
        return 1;
    if (o->only_error) {
        snprintf(st->error, sizeof st->error, "%s", o->error);
        return 0;
    }
    if (o->closes_request) {
        if (st->state != PUDDLE_STREAM_PENDING_OUT)
            return 1;
        st->state = PUDDLE_STREAM_CLOSED;
        snprintf(st->error, sizeof st->error, "%s", o->error);
        return 0;
    }
    if (st->owed != o->settled)
        return 1; /* something newer has been decided since; that one is still owed */
    st->owed = PUDDLE_OWED_NONE;
    snprintf(st->error, sizeof st->error, "%s", o->error);
    return 0;
}

void puddle_flow_done(void *user, const puddle_ob_item *item, puddle_ob_outcome outcome, const puddle_net_reply *reply)
{
    puddle_ctx *ctx = (puddle_ctx *)user;
    puddle_sync_done(ctx, item, outcome);
    puddle_profiles_done(ctx, item, outcome, reply);
    if (outcome == PUDDLE_OB_HOLD)
        return; /* waiting for the owner to trust the new certificate; the stream says so */
    char err[300], why[200];
    outcome_note o = { PUDDLE_OWED_NONE, "", 0, 0 };
    if (!strcmp(item->kind, "access") || !strcmp(item->kind, PUDDLE_OB_KIND_OPEN_ACCESS)) {
        if (outcome == PUDDLE_OB_DELIVERED)
            return; /* it stays pending_out until they answer */
        snprintf(why, sizeof why, "they could not be asked: %.150s", item->reason);
        o.error = why;
        o.closes_request = 1;
    } else if (!strcmp(item->kind, "result") || !strcmp(item->kind, "close")) {
        /* the body says which answer this was; "close" is its own */
        if (!strcmp(item->kind, "close"))
            o.settled = PUDDLE_OWED_CLOSED;
        else
            o.settled = strstr(item->body, "\"scoped\":true") ? PUDDLE_OWED_SCOPED : strstr(item->body, "\"approved\":true") ? PUDDLE_OWED_APPROVED : PUDDLE_OWED_DECLINED;
        if (outcome != PUDDLE_OB_DELIVERED) {
            snprintf(why, sizeof why, "they could not be told: %.150s", item->reason);
            o.error = why;
        }
    } else if (!strcmp(item->kind, "events")) {
        /* messages: a delivery that got through can still have had some refused; say so, and keep going */
        const char *rej = reply && reply->body ? strstr(reply->body, "\"rejected\":[{") : NULL;
        if (outcome == PUDDLE_OB_DELIVERED && !rej)
            return;
        if (outcome == PUDDLE_OB_DELIVERED) {
            const char *r = strstr(rej, "\"reason\":\"");
            char code[40] = "unknown";
            if (r) {
                r += 10;
                snprintf(code, sizeof code, "%.*s", (int)strcspn(r, "\""), r);
            }
            snprintf(why, sizeof why, "a message was refused by them: %s", code);
        } else {
            snprintf(why, sizeof why, "a message could not be delivered: %.150s", item->reason);
        }
        o.error = why;
        o.only_error = 1;
    } else {
        return;
    }
    puddle_streams_update(ctx->streams, item->peer, apply_outcome, &o, err, sizeof err);
}
