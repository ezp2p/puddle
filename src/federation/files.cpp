#include "federation/files.h"

#include "federation/groupflow.h"
#include "federation/sender.h"
#include "core/group.h"
#include "core/identity.h"
#include "core/streams.h"
#include "core/transfers.h"
#include "events/message.h"
#include "storage/files.h"
#include "base/log.h"
#include "base/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- who may have what ---- */

static void my_id(puddle_ctx *ctx, char out[PUDDLE_USER_ID_LEN + 1])
{
    puddle_identity_id(ctx->identity, out);
}

/* Is `peer` in `chat` with us: the DM between us over an open stream, or a group we are both in? */
static int in_chat_with(puddle_ctx *ctx, const char *chat, const char *peer)
{
    char me[PUDDLE_USER_ID_LEN + 1], dm[PUDDLE_USER_ID_LEN + 1];
    my_id(ctx, me);
    if (puddle_dm_chat_id(me, peer, dm) == 0 && !strcmp(dm, chat)) {
        puddle_stream st;
        if (puddle_streams_get(ctx->streams, peer, &st) != 0)
            return 0;
        int ok = st.state == PUDDLE_STREAM_ACTIVE && !puddle_streams_is_blocked(ctx->streams, peer);
        puddle_stream_free(&st);
        return ok;
    }
    puddle_group g;
    if (!ctx->groups || puddle_groups_get(ctx->groups, chat, &g) != 0)
        return 0;
    const puddle_gmember *m = puddle_group_member(&g, peer);
    int ok = g.my == PUDDLE_GROUP_IN && m && m->state == PUDDLE_GM_MEMBER && puddle_group_open_with(ctx, peer);
    puddle_group_free(&g);
    return ok;
}

/* The files of message `message` in `chat` (malloc'd; free it), and who sent it. 0, or -1 if there is no such message. */
static int files_of(puddle_ctx *ctx, const char *chat, const char *message, puddle_file_ref **refs, size_t *n, char sender[PUDDLE_USER_ID_LEN + 1])
{
    puddle_event ev;
    char err[200];
    if (puddle_store_get(ctx->store, chat, message, &ev, err, sizeof err) != 0)
        return -1;
    puddle_message m;
    const char *why;
    int rc = -1;
    if (puddle_message_parse(ev.event, ev.event_len, &m, &why) == 0) {
        if (m.files && puddle_file_refs_parse(m.files, strlen(m.files), refs, n) == 0) {
            snprintf(sender, PUDDLE_USER_ID_LEN + 1, "%s", m.sender);
            rc = 0;
        }
        puddle_message_free(&m);
    }
    puddle_event_free(&ev);
    return rc;
}

static const puddle_file_ref *find_ref(const puddle_file_ref *refs, size_t n, const char *id)
{
    for (size_t i = 0; i < n; i++)
        if (!strcmp(refs[i].id, id))
            return &refs[i];
    return NULL;
}

/* Who to ask for a file in `chat`: its sender first, then everyone else in the group we can reach. */
static size_t sources_for(puddle_ctx *ctx, const char *chat, const char *sender, char out[][PUDDLE_USER_ID_LEN + 1], size_t cap)
{
    char me[PUDDLE_USER_ID_LEN + 1];
    my_id(ctx, me);
    size_t n = 0;
    if (strcmp(sender, me) != 0)
        snprintf(out[n++], PUDDLE_USER_ID_LEN + 1, "%s", sender);
    puddle_group g;
    if (ctx->groups && puddle_groups_get(ctx->groups, chat, &g) == 0) {
        for (size_t i = 0; i < g.nmembers && n < cap; i++) {
            const puddle_gmember *m = &g.members[i];
            if (m->state == PUDDLE_GM_MEMBER && strcmp(m->id, me) != 0 && strcmp(m->id, sender) != 0 && puddle_group_open_with(ctx, m->id))
                snprintf(out[n++], PUDDLE_USER_ID_LEN + 1, "%s", m->id);
        }
        puddle_group_free(&g);
    }
    return n;
}

static int start(puddle_ctx *ctx, const char *chat, const char *message, const char *sender, const puddle_file_ref *ref)
{
    char src[PUDDLE_TRANSFER_SOURCES][PUDDLE_USER_ID_LEN + 1];
    const char *list[PUDDLE_TRANSFER_SOURCES];
    size_t n = sources_for(ctx, chat, sender, src, PUDDLE_TRANSFER_SOURCES);
    for (size_t i = 0; i < n; i++)
        list[i] = src[i];
    return puddle_transfers_start(ctx->transfers, ref->id, ref->size, chat, message, list, n);
}

/* ---- delivered once, asked again (docs/files.md, "Asking again") ----
 * Notes on a file (storage/files.h), one line each:
 *   served <peer> <ms>                     we delivered it whole to `peer` (the last time)
 *   request <peer> <chat> <message> <ms>   `peer` asked for it again; waiting for the owner
 *   grant <peer> <until ms>                the owner said yes: one more delivery to `peer`
 *   asked <chat> <message> <ms>            we asked for it again (the owner pressed Download)
 *   refused <peer> <chat> <message> <max>  `peer` takes nothing over `max` bytes, so did not fetch it */

#define GRACE_MS (60LL * 60 * 1000)         /* after a delivery, a download picking up where it stopped is not a new one */
#define GRANT_MS (7LL * 24 * 60 * 60 * 1000) /* how long a yes waits to be used */

struct note_scan {
    const char *peer;
    int64_t served_ms, grant_until; /* -1: none */
};

static int scan_note(void *user, const char *line)
{
    note_scan *s = (note_scan *)user;
    char kind[16], peer[PUDDLE_USER_ID_LEN + 1];
    long long ms;
    if (sscanf(line, "%15s %52s %lld", kind, peer, &ms) != 3 || strcmp(peer, s->peer) != 0)
        return 0;
    if (!strcmp(kind, "served") && ms > s->served_ms)
        s->served_ms = ms;
    else if (!strcmp(kind, "grant") && ms > s->grant_until)
        s->grant_until = ms;
    return 0;
}

/* Would we send `file` to `peer` without asking the owner? */
static int free_to_send(puddle_ctx *ctx, const char *file, const char *peer)
{
    note_scan s = { peer, -1, -1 };
    puddle_files_note_each(ctx->files, file, "", scan_note, &s);
    int64_t now = puddle_ctx_now_ms(ctx);
    return s.served_ms < 0 || now - s.served_ms < GRACE_MS || s.grant_until > now;
}

static void line_prefix(char *out, size_t cap, const char *kind, const char *a, const char *b)
{
    snprintf(out, cap, b ? "%s %s %s " : "%s %s ", kind, a, b);
}

/* We just sent `peer` the last chunk: it has had the file whole from us, and a yes is used up. */
static void delivered(puddle_ctx *ctx, const char *file, const char *peer)
{
    char pre[128], line[160];
    line_prefix(pre, sizeof pre, "served", peer, NULL);
    puddle_files_note_remove(ctx->files, file, pre);
    line_prefix(pre, sizeof pre, "grant", peer, NULL);
    puddle_files_note_remove(ctx->files, file, pre);
    line_prefix(pre, sizeof pre, "request", peer, NULL);
    puddle_files_note_remove(ctx->files, file, pre);
    snprintf(line, sizeof line, "served %s %lld", peer, (long long)puddle_ctx_now_ms(ctx));
    puddle_files_note_add(ctx->files, file, line);
}

/* POST `kind` about `file` to `peer`: {chat, message} and maybe {max}. */
static int tell(puddle_ctx *ctx, const char *peer, const char *file, const char *kind, const char *chat, const char *message, int64_t max)
{
    if (!ctx->outbox)
        return -1;
    char path[160], body[300], err[200];
    snprintf(path, sizeof path, "/stream/v1/files/%s/%s", file, kind);
    int n = max > 0 ? snprintf(body, sizeof body, "{\"chat\":\"%s\",\"message\":\"%s\",\"max\":%lld}", chat, message, (long long)max)
                    : snprintf(body, sizeof body, "{\"chat\":\"%s\",\"message\":\"%s\"}", chat, message);
    char k[32];
    snprintf(k, sizeof k, "file_%s", kind);
    if (puddle_outbox_enqueue(ctx->outbox, peer, k, path, body, (size_t)n, err, sizeof err) != 0) {
        puddle_log(PUDDLE_LOG_WARN, "cannot tell %.12s… about file %.12s…: %s", peer, file, err);
        return -1;
    }
    return 0;
}

/* ---- the routes ---- */

/* the file, chat and message of a request, if `c->sender` may have that file: 1, else 0 (answer 404) */
static int allowed(const puddle_call *c, char chat[PUDDLE_QUERY_VALUE_MAX], char message[PUDDLE_QUERY_VALUE_MAX])
{
    const char *keys[2] = { "chat", "message" };
    char v[2][PUDDLE_QUERY_VALUE_MAX];
    if (!c->ctx->files || puddle_query_parse(c->req->target, keys, 2, v) != 0 || strlen(v[0]) != PUDDLE_USER_ID_LEN || strlen(v[1]) != 64)
        return 0;
    if (!in_chat_with(c->ctx, v[0], c->sender))
        return 0;
    puddle_file_ref *refs;
    size_t n;
    char sender[PUDDLE_USER_ID_LEN + 1];
    if (files_of(c->ctx, v[0], v[1], &refs, &n, sender) != 0)
        return 0;
    int ok = find_ref(refs, n, puddle_param(c, "file")) != NULL;
    free(refs);
    memcpy(chat, v[0], PUDDLE_QUERY_VALUE_MAX);
    memcpy(message, v[1], PUDDLE_QUERY_VALUE_MAX);
    return ok;
}

static int chunk_list(const puddle_call *c, puddle_jw *w)
{
    char chat[PUDDLE_QUERY_VALUE_MAX], message[PUDDLE_QUERY_VALUE_MAX];
    if (!allowed(c, chat, message))
        return puddle_fail(w, 404, "Not Found");
    int64_t size;
    unsigned char *hashes;
    uint32_t count;
    if (puddle_files_chunk_list(c->ctx->files, puddle_param(c, "file"), &size, &hashes, &count) != 0)
        return puddle_fail(w, 404, "Not Found");
    c->blob->data = hashes;
    c->blob->len = (size_t)count * 32;
    snprintf(c->blob->mime, sizeof c->blob->mime, "application/octet-stream");
    return 200;
}

static int chunk(const puddle_call *c, puddle_jw *w)
{
    char chat[PUDDLE_QUERY_VALUE_MAX], message[PUDDLE_QUERY_VALUE_MAX];
    const char *file = puddle_param(c, "file");
    if (!allowed(c, chat, message) || !free_to_send(c->ctx, file, c->sender)) /* had it whole from us, and not asked again */
        return puddle_fail(w, 404, "Not Found");
    unsigned char *data;
    size_t len;
    uint32_t n = (uint32_t)strtoul(puddle_param(c, "n"), NULL, 10);
    int64_t size = 0;
    if (puddle_files_read_chunk(c->ctx->files, file, n, &data, &len) != 0)
        return puddle_fail(w, 404, "Not Found");
    if (puddle_files_state(c->ctx->files, file, &size, NULL) == PUDDLE_FILE_WHOLE && n + 1 == puddle_file_chunk_count(size))
        delivered(c->ctx, file, c->sender);
    c->blob->data = data;
    c->blob->len = len;
    snprintf(c->blob->mime, sizeof c->blob->mime, "application/octet-stream");
    return 200;
}

/* the chat and message a POST names, if `c->sender` may know of that file there (as allowed()): 1, else 0 */
static int allowed_body(const puddle_call *c, char chat[PUDDLE_USER_ID_LEN + 1], char message[65], char sender[PUDDLE_USER_ID_LEN + 1])
{
    puddle_span v;
    const char *b = (const char *)c->body;
    if (!c->ctx->files || puddle_json_member(b, c->body_len, "chat", &v) != 0 || puddle_json_string(v, chat, PUDDLE_USER_ID_LEN + 1) != 0 ||
        strlen(chat) != PUDDLE_USER_ID_LEN || puddle_json_member(b, c->body_len, "message", &v) != 0 || puddle_json_string(v, message, 65) != 0 ||
        strlen(message) != 64 || !puddle_file_id_valid(message))
        return 0;
    if (!in_chat_with(c->ctx, chat, c->sender))
        return 0;
    puddle_file_ref *refs;
    size_t n;
    if (files_of(c->ctx, chat, message, &refs, &n, sender) != 0)
        return 0;
    int ok = find_ref(refs, n, puddle_param(c, "file")) != NULL;
    free(refs);
    return ok;
}

/* They will not fetch a file we sent: it is over the most they take. */
static int refused_post(const puddle_call *c, puddle_jw *w)
{
    char chat[PUDDLE_USER_ID_LEN + 1], message[65], sender[PUDDLE_USER_ID_LEN + 1], me[PUDDLE_USER_ID_LEN + 1], line[300];
    puddle_span v;
    int64_t max = 0;
    my_id(c->ctx, me);
    if (!allowed_body(c, chat, message, sender) || strcmp(sender, me) != 0 ||
        puddle_json_member((const char *)c->body, c->body_len, "max", &v) != 0 || puddle_json_int64(v, &max) != 0 || max <= 0)
        return puddle_fail(w, 404, "Not Found");
    const char *file = puddle_param(c, "file");
    snprintf(line, sizeof line, "refused %s %s %s ", c->sender, chat, message);
    puddle_files_note_remove(c->ctx->files, file, line);
    snprintf(line, sizeof line, "refused %s %s %s %lld", c->sender, chat, message, (long long)max);
    puddle_files_note_add(c->ctx->files, file, line);
    return 200;
}

/* They want a file again. Sent by itself if we would anyway (D10); otherwise it waits for the owner's yes. */
static int request_post(const puddle_call *c, puddle_jw *w)
{
    char chat[PUDDLE_USER_ID_LEN + 1], message[65], sender[PUDDLE_USER_ID_LEN + 1], pre[200], line[300];
    const char *file = puddle_param(c, "file");
    if (!allowed_body(c, chat, message, sender) || puddle_files_state(c->ctx->files, file, NULL, NULL) != PUDDLE_FILE_WHOLE)
        return puddle_fail(w, 404, "Not Found");
    if (free_to_send(c->ctx, file, c->sender))
        return tell(c->ctx, c->sender, file, "granted", chat, message, 0) == 0 ? 200 : puddle_fail(w, 500, "internal");
    snprintf(pre, sizeof pre, "request %s %s %s ", c->sender, chat, message);
    puddle_files_note_remove(c->ctx->files, file, pre);
    snprintf(line, sizeof line, "request %s %s %s %lld", c->sender, chat, message, (long long)puddle_ctx_now_ms(c->ctx));
    if (puddle_files_note_add(c->ctx->files, file, line) != 0)
        return puddle_fail(w, 404, "Not Found");
    puddle_log(PUDDLE_LOG_INFO, "%.12s… asked for file %.12s… again", c->sender, file);
    return 200; /* the same answer whatever happens next, so nothing is learnt from it */
}

/* Someone we asked will send it: fetch it now, from them first. */
static int granted_post(const puddle_call *c, puddle_jw *w)
{
    char chat[PUDDLE_USER_ID_LEN + 1], message[65], sender[PUDDLE_USER_ID_LEN + 1], pre[100];
    const char *file = puddle_param(c, "file");
    if (!allowed_body(c, chat, message, sender) || !c->ctx->transfers)
        return puddle_fail(w, 404, "Not Found");
    snprintf(pre, sizeof pre, "asked %s %s ", chat, message);
    int asked = 0;
    puddle_files_note_each(c->ctx->files, file, pre, [](void *u, const char *) { *(int *)u = 1; return 1; }, &asked);
    if (!asked || puddle_files_state(c->ctx->files, file, NULL, NULL) == PUDDLE_FILE_WHOLE)
        return 200; /* not wanted (any more) */
    puddle_file_ref *refs;
    size_t n;
    if (files_of(c->ctx, chat, message, &refs, &n, sender) != 0)
        return puddle_fail(w, 404, "Not Found");
    const puddle_file_ref *ref = find_ref(refs, n, file);
    const char *list[1] = { c->sender };
    if (ref && puddle_transfers_start(c->ctx->transfers, file, ref->size, chat, message, list, 1) == 0)
        puddle_transfers_retry_now(c->ctx->transfers);
    free(refs);
    return 200;
}

/* It reached them (from us or someone else): stop waiting for the owner about it. */
static int done_post(const puddle_call *c, puddle_jw *w)
{
    char chat[PUDDLE_USER_ID_LEN + 1], message[65], sender[PUDDLE_USER_ID_LEN + 1], pre[200];
    if (!allowed_body(c, chat, message, sender))
        return puddle_fail(w, 404, "Not Found");
    snprintf(pre, sizeof pre, "request %s %s %s ", c->sender, chat, message);
    puddle_files_note_remove(c->ctx->files, puddle_param(c, "file"), pre);
    return 200;
}

const puddle_route puddle_file_routes[] = {
    { "GET",  "/stream/v1/files/{file:event}/chunks",           PUDDLE_AUTH_PEER, 0,    chunk_list,   NULL },
    { "GET",  "/stream/v1/files/{file:event}/chunks/{n:index}", PUDDLE_AUTH_PEER, 0,    chunk,        NULL },
    { "POST", "/stream/v1/files/{file:event}/refused",          PUDDLE_AUTH_PEER, 1024, refused_post, NULL },
    { "POST", "/stream/v1/files/{file:event}/request",          PUDDLE_AUTH_PEER, 1024, request_post, NULL },
    { "POST", "/stream/v1/files/{file:event}/granted",          PUDDLE_AUTH_PEER, 1024, granted_post, NULL },
    { "POST", "/stream/v1/files/{file:event}/done",             PUDDLE_AUTH_PEER, 1024, done_post,    NULL },
};
const size_t puddle_file_routes_count = sizeof puddle_file_routes / sizeof puddle_file_routes[0];

/* ---- fetching ---- */

void puddle_federation_fetch(void *user, const char *peer, const char *path, size_t max, puddle_net_reply *reply)
{
    puddle_ob_item it;
    memset(&it, 0, sizeof it);
    snprintf(it.peer, sizeof it.peer, "%s", peer);
    snprintf(it.kind, sizeof it.kind, "file");
    snprintf(it.method, sizeof it.method, "GET");
    if ((size_t)snprintf(it.path, sizeof it.path, "%s", path) >= sizeof it.path) { /* cut short, it would ask for something else */
        memset(reply, 0, sizeof *reply);
        reply->result = PUDDLE_NET_FAILED;
        snprintf(reply->reason, sizeof reply->reason, "the request is too long");
        return;
    }
    it.max_reply = max + 1024;
    puddle_federation_send((const puddle_outbox_hooks *)user, &it, reply);
}

/* Its author took a message back: its files go with it, unless another message still carries them. */
static void taken_back(puddle_ctx *ctx, const char *chat, const char *sender, const char *target)
{
    puddle_file_ref *refs;
    size_t n;
    char author[PUDDLE_USER_ID_LEN + 1];
    if (files_of(ctx, chat, target, &refs, &n, author) != 0)
        return;
    if (!strcmp(author, sender)) /* only the author's own redaction counts (events/message.h) */
        for (size_t i = 0; i < n; i++)
            if (puddle_files_unref(ctx->files, refs[i].id, chat, target) == 0)
                puddle_transfers_forget(ctx->transfers, ctx->files, refs[i].id);
    free(refs);
}

void puddle_files_arrived(puddle_ctx *ctx, const char *chat, const char *sender, const char *event, size_t len, const char *id)
{
    if (!ctx->files)
        return;
    puddle_message m;
    const char *why;
    if (puddle_chat_event_parse(event, len, &m, &why) != 0)
        return;
    if (m.type == PUDDLE_EV_REDACT)
        taken_back(ctx, chat, sender, m.ref);
    puddle_file_ref *refs;
    size_t n;
    if (m.type == PUDDLE_EV_MESSAGE && m.files && puddle_file_refs_parse(m.files, strlen(m.files), &refs, &n) == 0) {
        int64_t limit = puddle_files_auto_download(ctx->config), now = puddle_ctx_now_ms(ctx);
        int64_t max = puddle_files_max_size(ctx->config);
        char me[PUDDLE_USER_ID_LEN + 1];
        my_id(ctx, me);
        for (size_t i = 0; i < n; i++) {
            puddle_files_ref(ctx->files, refs[i].id, chat, id, now); /* kept while the message is, fetched or not */
            if (refs[i].size > max && strcmp(sender, me) != 0)
                tell(ctx, sender, refs[i].id, "refused", chat, id, max); /* said openly, so nobody wonders why it never came */
            else if (ctx->transfers && refs[i].size <= limit && start(ctx, chat, id, sender, &refs[i]) != 0)
                puddle_log(PUDDLE_LOG_WARN, "cannot start fetching file %.12s…", refs[i].id);
        }
        free(refs);
    }
    puddle_message_free(&m);
}

int puddle_file_fetch(puddle_ctx *ctx, const char *chat, const char *message, const char *file, const char **code)
{
    *code = "not_found";
    puddle_file_ref *refs;
    size_t n;
    char sender[PUDDLE_USER_ID_LEN + 1];
    if (!ctx->transfers || files_of(ctx, chat, message, &refs, &n, sender) != 0)
        return 404;
    const puddle_file_ref *ref = find_ref(refs, n, file);
    int rc = 0;
    if (!ref) {
        rc = 404;
    } else if (ref->size > puddle_files_max_size(ctx->config)) {
        *code = "too_large";
        rc = 409;
    } else if (start(ctx, chat, message, sender, ref) != 0) {
        *code = "internal";
        rc = 500;
    }
    free(refs);
    return rc;
}

int puddle_file_request(puddle_ctx *ctx, const char *chat, const char *message, const char *file, const char **code)
{
    *code = "not_found";
    puddle_file_ref *refs;
    size_t n;
    char sender[PUDDLE_USER_ID_LEN + 1];
    if (!ctx->files || files_of(ctx, chat, message, &refs, &n, sender) != 0)
        return 404;
    const puddle_file_ref *ref = find_ref(refs, n, file);
    int rc = 0;
    if (!ref) {
        rc = 404;
    } else if (ref->size > puddle_files_max_size(ctx->config)) {
        *code = "too_large";
        rc = 409;
    } else if (puddle_files_state(ctx->files, file, NULL, NULL) == PUDDLE_FILE_WHOLE) {
        *code = "already_here";
        rc = 409;
    } else {
        /* everyone who may have it: one who delivered it whole to us before asks its owner (D10); the rest just send it */
        char src[PUDDLE_TRANSFER_SOURCES][PUDDLE_USER_ID_LEN + 1], pre[100], line[160];
        size_t k = sources_for(ctx, chat, sender, src, PUDDLE_TRANSFER_SOURCES);
        snprintf(pre, sizeof pre, "asked %s %s ", chat, message);
        puddle_files_note_remove(ctx->files, file, pre);
        snprintf(line, sizeof line, "asked %s %s %lld", chat, message, (long long)puddle_ctx_now_ms(ctx));
        puddle_files_note_add(ctx->files, file, line);
        for (size_t i = 0; i < k; i++)
            tell(ctx, src[i], file, "request", chat, message, 0);
        if (k == 0) {
            *code = "nobody_to_ask";
            rc = 409;
        }
    }
    free(refs);
    return rc;
}

/* A file we asked for again is here: tell everyone we asked, so none of them is left asking its owner. */
void puddle_file_finished(void *user, const char *file, const char *chat, const char *message)
{
    puddle_ctx *ctx = (puddle_ctx *)user;
    char pre[100];
    snprintf(pre, sizeof pre, "asked %s %s ", chat, message);
    if (!ctx->files || puddle_files_note_remove(ctx->files, file, pre) <= 0)
        return;
    puddle_file_ref *refs;
    size_t n;
    char sender[PUDDLE_USER_ID_LEN + 1], src[PUDDLE_TRANSFER_SOURCES][PUDDLE_USER_ID_LEN + 1];
    if (files_of(ctx, chat, message, &refs, &n, sender) != 0)
        return;
    free(refs);
    size_t k = sources_for(ctx, chat, sender, src, PUDDLE_TRANSFER_SOURCES);
    for (size_t i = 0; i < k; i++)
        tell(ctx, src[i], file, "done", chat, message, 0);
}

/* The owner's answer to `peer` asking for `file` (of `message` in `chat`) again. */
int puddle_file_answer(puddle_ctx *ctx, const char *chat, const char *message, const char *file, const char *peer, int yes, const char **code)
{
    *code = "not_found";
    char pre[200], line[160];
    snprintf(pre, sizeof pre, "request %s %s %s ", peer, chat, message);
    if (!ctx->files || puddle_files_note_remove(ctx->files, file, pre) <= 0)
        return 404;
    if (!yes)
        return 0; /* they are not told: a no looks like nobody answering (docs/files.md) */
    snprintf(line, sizeof line, "grant %s %lld", peer, (long long)(puddle_ctx_now_ms(ctx) + GRANT_MS));
    if (puddle_files_note_add(ctx->files, file, line) != 0 || tell(ctx, peer, file, "granted", chat, message, 0) != 0) {
        *code = "internal";
        return 500;
    }
    return 0;
}
