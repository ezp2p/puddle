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
    if (!allowed(c, chat, message))
        return puddle_fail(w, 404, "Not Found");
    unsigned char *data;
    size_t len;
    if (puddle_files_read_chunk(c->ctx->files, puddle_param(c, "file"), (uint32_t)strtoul(puddle_param(c, "n"), NULL, 10), &data, &len) != 0)
        return puddle_fail(w, 404, "Not Found");
    c->blob->data = data;
    c->blob->len = len;
    snprintf(c->blob->mime, sizeof c->blob->mime, "application/octet-stream");
    return 200;
}

const puddle_route puddle_file_routes[] = {
    { "GET", "/stream/v1/files/{file:event}/chunks",           PUDDLE_AUTH_PEER, 0, chunk_list, NULL },
    { "GET", "/stream/v1/files/{file:event}/chunks/{n:index}", PUDDLE_AUTH_PEER, 0, chunk,      NULL },
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

void puddle_files_arrived(puddle_ctx *ctx, const char *chat, const char *sender, const char *event, size_t len, const char *id)
{
    if (!ctx->transfers || !ctx->files)
        return;
    puddle_message m;
    const char *why;
    if (puddle_message_parse(event, len, &m, &why) != 0)
        return;
    puddle_file_ref *refs;
    size_t n;
    if (m.files && puddle_file_refs_parse(m.files, strlen(m.files), &refs, &n) == 0) {
        int64_t limit = puddle_files_auto_download(ctx->config);
        for (size_t i = 0; i < n; i++)
            if (refs[i].size <= limit && start(ctx, chat, id, sender, &refs[i]) != 0)
                puddle_log(PUDDLE_LOG_WARN, "cannot start fetching file %.12s…", refs[i].id);
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
