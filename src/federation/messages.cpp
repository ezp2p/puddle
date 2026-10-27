#include "federation/messages.h"

#include "base/encoding.h"
#include "federation/flow.h"
#include "base/hash.h"
#include "base/log.h"
#include "events/message.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- a peer delivers events ---- */

typedef struct {
    puddle_jw *w;
    int started;
} result_writer;

static void reject(puddle_jw *w, const char *id, const char *reason)
{
    puddle_jw_object(w);
    puddle_jw_str(w, "id", id);
    puddle_jw_str(w, "reason", reason);
    puddle_jw_end_object(w);
}

/* One delivered event, taken as the policy says. The envelope, the id (a hash of the exact bytes) and the signature are the same for
 * every chat; what makes an event acceptable in this chat, and what is done with it, is the policy's. Returns the reason it is
 * refused, or NULL if it was taken (`dup` says whether it was already there). */
const char *puddle_take_signed(const puddle_event_policy *pol, const unsigned char sender_pub[32], const char *sender_kid, puddle_span ev,
                               char id_out[PUDDLE_ID_HEX + 1], int *dup)
{
    puddle_span v;
    char claimed[PUDDLE_ID_HEX + 1], sigtext[PUDDLE_SIG_TEXT_MAX + 4];
    *dup = 0;
    id_out[0] = '\0';
    if (ev.len < 2 || ev.p[0] != '{' || !puddle_json_valid_object(ev.p, ev.len)) 
        return "malformed";
    if (puddle_json_member(ev.p, ev.len, "id", &v) != 0 || puddle_json_string(v, claimed, sizeof claimed) != 0 || strlen(claimed) != 64)
        return "malformed";
    snprintf(id_out, PUDDLE_ID_HEX + 1, "%s", claimed);
    if (puddle_json_member(ev.p, ev.len, "event", &v) != 0 || v.len < 2 || v.p[0] != '"' || v.len > PUDDLE_EVENT_MAX_BYTES * 6u)
        return "malformed";
    char *text = (char *)malloc(v.len + 1);
    if (!text || puddle_json_string(v, text, v.len + 1) != 0) {
        free(text);
        return "malformed";
    }
    size_t tl = strlen(text);
    const char *reason = NULL;
    char hash[65];
    unsigned char sig[64], pub[32];
    char kid[65];
    if (tl == 0 || tl > PUDDLE_EVENT_MAX_BYTES) {
        reason = "too_large";
        goto out;
    }
    puddle_sha256_hex(text, tl, hash);
    if (strcmp(hash, claimed) != 0) {
        reason = "bad_id";
        goto out;
    }
    if (puddle_json_member(ev.p, ev.len, "sig", &v) != 0 || puddle_json_string(v, sigtext, sizeof sigtext) != 0 ||
        puddle_event_sig_parse(sigtext, kid, sig) != 0) {
        reason = "bad_signature";
        goto out;
    }
    if ((reason = pol->judge(pol->user, text, tl)) != NULL)
        goto out;
    /* Only the key they use now. A retired key is what a stolen phone leaves behind; a message it signs after the
     * owner has changed keys is exactly what changing keys is meant to stop. */
    if (strcmp(kid, sender_kid) != 0) {
        reason = "unknown_key";
        goto out;
    }
    memcpy(pub, sender_pub, 32);
    if (!puddle_event_verify(pub, text, tl, sig)) {
        reason = "bad_signature";
        goto out;
    }
    reason = pol->commit(pol->user, text, tl, sigtext, id_out, dup);
out:
    free(text);
    return reason;
}

/* the policy of a direct message: a message, edit or redaction in the chat these two share, from the one who sent it */
typedef struct {
    puddle_ctx *ctx;
    const char *sender;
    const char *chat;
} dm_policy;

static const char *dm_judge(void *user, const char *text, size_t tl)
{
    dm_policy *p = (dm_policy *)user;
    puddle_message m;
    const char *reason = NULL;
    if (puddle_chat_event_parse(text, tl, &m, &reason) != 0)
        return reason;
    if (strcmp(m.chat, p->chat) != 0)
        reason = "wrong_chat";
    else if (strcmp(m.sender, p->sender) != 0)
        reason = "wrong_sender";
    puddle_message_free(&m);
    return reason;
}

const char *puddle_store_delivered(puddle_ctx *ctx, const char *chat, const char *sender, const char *text, size_t tl, const char *sigtext,
                                   char id_out[PUDDLE_ID_HEX + 1], int *dup)
{
    char err[300];
    int ar = puddle_store_append(ctx->store, chat, text, tl, sigtext, puddle_ctx_now_ms(ctx), id_out, err, sizeof err);
    if (ar < 0) {
        puddle_log(PUDDLE_LOG_WARN, "cannot store a message from %.12s…: %s", sender, err);
        return "storage_error";
    }
    *dup = ar == PUDDLE_APPEND_DUPLICATE;
    return NULL;
}

static const char *dm_commit(void *user, const char *text, size_t tl, const char *sigtext, char id_out[PUDDLE_ID_HEX + 1], int *dup)
{
    dm_policy *p = (dm_policy *)user;
    return puddle_store_delivered(p->ctx, p->chat, p->sender, text, tl, sigtext, id_out, dup);
}

/* Take every event of a delivery under `pol` and answer with what became of each. A delivery that is too long is refused whole rather
 * than half taken. `evs` is the "events" array. */
int puddle_process_delivery(const puddle_call *c, puddle_jw *w, puddle_span evs, const puddle_event_policy *pol, const unsigned char sender_pub[32],
                            const char *sender_kid)
{
    puddle_span ev;
    /* first pass: count, so a delivery that is too long is refused whole rather than half taken */
    size_t cursor = 0, count = 0;
    int r;
    while ((r = puddle_json_array_next(evs, &cursor, &ev)) == 1)
        if (++count > PUDDLE_DELIVERY_MAX_EVENTS)
            return puddle_fail(w, 400, "too_many_events");
    if (r < 0)
        return puddle_fail(w, 400, "bad_request");

    char (*acc)[PUDDLE_ID_HEX + 1] = (char (*)[65])calloc(count ? count : 1, sizeof *acc);
    char (*dup)[PUDDLE_ID_HEX + 1] = (char (*)[65])calloc(count ? count : 1, sizeof *dup);
    char (*rej_id)[PUDDLE_ID_HEX + 1] = (char (*)[65])calloc(count ? count : 1, sizeof *rej_id);
    const char **rej_why = (const char **)calloc(count ? count : 1, sizeof *rej_why);
    if (!acc || !dup || !rej_id || !rej_why) {
        free(acc); free(dup); free(rej_id); free(rej_why);
        return puddle_fail(w, 500, "internal");
    }
    size_t na = 0, nd = 0, nr = 0;
    cursor = 0;
    while (puddle_json_array_next(evs, &cursor, &ev) == 1) {
        char id[PUDDLE_ID_HEX + 1];
        int is_dup;
        const char *why = puddle_take_signed(pol, sender_pub, sender_kid, ev, id, &is_dup);
        if (why) {
            snprintf(rej_id[nr], sizeof rej_id[nr], "%s", id);
            rej_why[nr++] = why;
        } else if (is_dup) {
            snprintf(dup[nd++], sizeof dup[0], "%s", id);
        } else {
            snprintf(acc[na++], sizeof acc[0], "%s", id);
        }
    }
    puddle_jw_object(w);
    puddle_jw_key(w, "accepted");
    puddle_jw_array(w);
    for (size_t i = 0; i < na; i++) puddle_jw_string(w, acc[i]);
    puddle_jw_end_array(w);
    puddle_jw_key(w, "duplicate");
    puddle_jw_array(w);
    for (size_t i = 0; i < nd; i++) puddle_jw_string(w, dup[i]);
    puddle_jw_end_array(w);
    puddle_jw_key(w, "rejected");
    puddle_jw_array(w);
    for (size_t i = 0; i < nr; i++) reject(w, rej_id[i], rej_why[i]);
    puddle_jw_end_array(w);
    puddle_jw_end_object(w);
    free(acc); free(dup); free(rej_id); free(rej_why);
    if (na)
        puddle_log(PUDDLE_LOG_INFO, "%zu message%s from %.12s…", na, na == 1 ? "" : "s", c->sender);
    return 200;
}

static int events_post(const puddle_call *c, puddle_jw *w)
{
    const char *b = (const char *)c->body;
    puddle_span chat_v, evs;
    char chat[PUDDLE_USER_ID_LEN + 1], me[PUDDLE_USER_ID_LEN + 1], want[PUDDLE_USER_ID_LEN + 1];

    /* a delivery for a group chat is judged by the group's rules */
    if (puddle_json_valid_object(b, c->body_len) && puddle_json_member(b, c->body_len, "chat", &chat_v) == 0 &&
        puddle_json_string(chat_v, chat, sizeof chat) == 0) {
        puddle_identity_id(c->ctx->identity, me);
        puddle_dm_chat_id(me, c->sender, want);
        if (strcmp(chat, want) != 0 && puddle_id_valid(chat))
            return puddle_group_events_post(c, w, chat);
    }

    /* the sender is a known peer (their key is how the request was checked), but may not have an open stream */
    puddle_stream st;
    if (puddle_streams_get(c->ctx->streams, c->sender, &st) != 0)
        return puddle_fail(w, 403, "no_stream");
    int active = st.state == PUDDLE_STREAM_ACTIVE && st.chain;
    char *chain = active ? strdup(st.chain) : NULL;
    size_t chain_len = active ? st.chain_len : 0;
    puddle_stream_free(&st);
    if (!chain)
        return puddle_fail(w, 403, "no_stream");

    puddle_identity_id(c->ctx->identity, me);
    puddle_dm_chat_id(me, c->sender, want);
    if (!puddle_json_valid_object(b, c->body_len) || puddle_json_member(b, c->body_len, "chat", &chat_v) != 0 ||
        puddle_json_string(chat_v, chat, sizeof chat) != 0 || strcmp(chat, want) != 0 ||
        puddle_json_member(b, c->body_len, "events", &evs) != 0) {
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

    dm_policy dp = { c->ctx, c->sender, chat };
    puddle_event_policy pol = { dm_judge, dm_commit, &dp };
    return puddle_process_delivery(c, w, evs, &pol, head.sign_pub, kid);
}

const puddle_route puddle_message_routes[] = {
    { "POST", "/stream/v1/events",   PUDDLE_AUTH_PEER,  PUDDLE_DELIVERY_MAX_BYTES, events_post, NULL },
};
const size_t puddle_message_routes_count = sizeof puddle_message_routes / sizeof puddle_message_routes[0];
