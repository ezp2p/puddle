#include "service/server.h"

#include "core/auth.h"
#include "http/http.h"
#include "core/identity.h"
#include "federation/flow.h"
#include "federation/keysync.h"
#include "base/log.h"
#include "core/outbox.h"
#include "core/profile.h"
#include "http/reqsig.h"
#include "http/router.h"
#include "http/serve.h"
#include "storage/store.h"
#include "federation/groupflow.h"
#include "core/conversations.h"
#include "core/group.h"
#include "core/people.h"
#include "core/streams.h"
#include "http/tls.h"
#include "platform/net.h"
#include "platform/process.h"
#include "service/routes.h"
#include "federation/courier.h"
#include "api/api.h"
#include "federation/sender.h"

#include <errno.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <new>
#include <system_error>
#include <thread>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_CONNECTIONS 64
#define IO_TIMEOUT_SEC 10
#define REQUEST_DEADLINE_SEC 15

/* A wait on a condition variable while holding `mu`, which is locked and unlocked by hand around it. */
static void wait_on(std::condition_variable &cv, std::mutex &mu)
{
    std::unique_lock<std::mutex> held(mu, std::adopt_lock);
    cv.wait(held);
    held.release(); /* still locked: the caller unlocks it */
}

struct puddle_server {
    puddle_config cfg;
    SSL_CTX *tls;
    puddle_store *store;
    puddle_auth *auth;
    puddle_identity *identity;
    puddle_profiles *profiles;
    puddle_assignments *assignments;
    puddle_streams *streams;
    puddle_people *people;
    puddle_conversations *conversations;
    puddle_groups *groups;
    puddle_outbox *outbox;
    puddle_bucket *anon_bucket;
    puddle_keyed *access_limit;
    puddle_replay *replay;
    puddle_roll_slot *roll_slot;
    puddle_ctx ctx;
    puddle_router router;
    puddle_sock listener;
    puddle_waker stop; /* tripped by puddle_server_stop, which may be called from a signal handler */
    std::mutex mu;
    std::condition_variable drained;
    int active;
};

typedef struct {
    puddle_server *srv;
    puddle_sock sock;
    char peer[64];
} conn;

/* a profile kept in a stream record from before core/people.h, moved to its person record: the one they show us directly */
static int keep_old_profile(void *user, const char *id, const char *json, size_t len, int64_t updated_ms)
{
    return puddle_people_set_profile((puddle_people *)user, id, PUDDLE_PEOPLE_DM, json, len, NULL, updated_ms);
}

/* Profiles have just moved into the data folder. Before, `old_default` was shown to everyone the owner had chosen no
 * profile for: keep showing it to those who have it already (every stream not closed, every group we are in), so that
 * nobody's view of the owner changes. From now on, a new person or group sees a profile only once one is chosen. */
static void keep_old_default(puddle_server *s, const char *old_default)
{
    char place[PUDDLE_PLACE_MAX], have[PUDDLE_PROFILE_ID_MAX + 1], err[300];
    int kept = 0;
    puddle_stream *sl;
    size_t sn;
    if (puddle_streams_list(s->streams, &sl, &sn) == 0) {
        for (size_t i = 0; i < sn; i++)
            if (sl[i].state != PUDDLE_STREAM_CLOSED && !sl[i].provisional && puddle_place_dm(sl[i].id, place) == 0 &&
                puddle_assignments_get(s->assignments, place, have) != 0 &&
                puddle_assignments_set(s->assignments, place, old_default, err, sizeof err) == 0)
                kept++;
        puddle_stream_list_free(sl, sn);
    }
    puddle_group *gl;
    size_t gn;
    if (puddle_groups_list(s->groups, &gl, &gn) == 0) {
        for (size_t i = 0; i < gn; i++)
            if (gl[i].my == PUDDLE_GROUP_IN && puddle_place_group(gl[i].id, place) == 0 && puddle_assignments_get(s->assignments, place, have) != 0 &&
                puddle_assignments_set(s->assignments, place, old_default, err, sizeof err) == 0)
                kept++;
        puddle_group_list_free(gl, gn);
    }
    if (kept)
        puddle_log(PUDDLE_LOG_INFO, "profile '%s' stays shown in the %d place%s that saw it before; anywhere new sees a profile only once you choose one",
                   old_default, kept, kept == 1 ? "" : "s");
}

/* The outbox's view of the streams: where a peer is, whether its certificate is the one we know, and what to
 * do when something reaches an end. (Acting on the ends is roadmap 3.5; for now they are logged.) */
static int ob_addresses(void *user, const char *peer, puddle_address *out, int max)
{
    puddle_stream st;
    if (puddle_streams_get(((puddle_server *)user)->streams, peer, &st) != 0)
        return 0;
    int n = 0;
    for (int i = 0; i < st.naddr && n < max; i++)
        if (puddle_address_parse(st.addresses[i], &out[n]) == 0)
            n++;
    puddle_stream_free(&st);
    return n;
}

static int ob_fingerprint(void *user, const char *peer, const char *fp)
{
    int ok = puddle_streams_check_fingerprint(((puddle_server *)user)->streams, peer, fp);
    if (!ok)
        puddle_log(PUDDLE_LOG_WARN, "%.12s… has a different certificate from before; nothing is sent until you say it is fine", peer);
    return ok;
}

static void ob_done(void *user, const puddle_ob_item *it, puddle_ob_outcome outcome, const puddle_net_reply *reply)
{
    puddle_flow_done(&((puddle_server *)user)->ctx, it, outcome, reply);
    puddle_log(outcome == PUDDLE_OB_DELIVERED ? PUDDLE_LOG_INFO : PUDDLE_LOG_WARN, "outbox: %s to %.12s… %s%s%s", it->kind, it->peer,
               outcome == PUDDLE_OB_DELIVERED ? "delivered" : outcome == PUDDLE_OB_HOLD ? "held" : "failed",
               outcome == PUDDLE_OB_DELIVERED ? "" : ": ", outcome == PUDDLE_OB_DELIVERED ? "" : it->reason);
}

static void seterr(char *err, size_t n, const char *fmt, ...)
{
    if (!err || !n)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

static int open_listener(puddle_server *s, char *err, size_t errlen)
{
    return puddle_sock_listen(s->cfg.address, s->cfg.port, &s->listener, err, errlen);
}

puddle_server *puddle_server_create(const puddle_config *cfg, char *err, size_t errlen)
{
    puddle_ignore_broken_pipe();

    puddle_server *s = new (std::nothrow) puddle_server();
    if (!s) {
        seterr(err, errlen, "out of memory");
        return NULL;
    }
    s->cfg = *cfg;
    s->listener = PUDDLE_SOCK_NONE;
    s->stop.read_end = s->stop.write_end = PUDDLE_SOCK_NONE;

    s->store = puddle_store_open(cfg->data_dir, err, errlen);
    if (!s->store)
        goto fail;
    s->auth = puddle_auth_open(cfg->data_dir, err, errlen);
    if (!s->auth)
        goto fail;
    s->identity = puddle_identity_open(cfg->data_dir, err, errlen);
    if (!s->identity)
        goto fail;
    {
        char uid[PUDDLE_USER_ID_LEN + 1];
        puddle_identity_id(s->identity, uid);
        puddle_log(PUDDLE_LOG_INFO, "identity %s", uid);
    }
    char old_default[PUDDLE_PROFILE_ID_MAX + 1];
    s->profiles = puddle_profiles_open(cfg, old_default, err, errlen);
    if (!s->profiles)
        goto fail;
    s->assignments = puddle_assignments_open(cfg->data_dir, err, errlen);
    if (!s->assignments)
        goto fail;
    s->streams = puddle_streams_open(cfg->data_dir, err, errlen);
    if (!s->streams)
        goto fail;
    s->people = puddle_people_open(cfg->data_dir, err, errlen);
    if (!s->people)
        goto fail;
    s->conversations = puddle_conversations_open(cfg->data_dir, err, errlen);
    if (!s->conversations)
        goto fail;
    puddle_store_on_append(s->store, puddle_conversations_appended, s->conversations);
    {
        int moved = puddle_streams_take_profiles(s->streams, keep_old_profile, s->people);
        if (moved > 0)
            puddle_log(PUDDLE_LOG_INFO, "moved %d profile%s from stream records to people", moved, moved == 1 ? "" : "s");
        else if (moved < 0)
            puddle_log(PUDDLE_LOG_WARN, "some profiles could not be moved out of their stream records; trying again next start");
    }
    s->groups = puddle_groups_open(cfg->data_dir, err, errlen);
    if (!s->groups)
        goto fail;
    if (old_default[0])
        keep_old_default(s, old_default);
    s->replay = puddle_replay_new(0);
    if (!s->replay) {
        seterr(err, errlen, "out of memory");
        goto fail;
    }
    {
        puddle_outbox_hooks hooks = { .send = puddle_federation_send, .identity = s->identity, .addresses = ob_addresses, .fingerprint = ob_fingerprint,
                                      .done = ob_done, .user = s };
        s->outbox = puddle_outbox_open(cfg->data_dir, &hooks, err, errlen);
        if (!s->outbox)
            goto fail;
    }
    /* checking a stranger's key chain costs signatures: 50 a second on average, 100 in a burst. A requester
     * may ask five times an hour. */
    s->anon_bucket = puddle_bucket_new(50, 100);
    s->access_limit = puddle_keyed_new(1024, 5, 3600 * 1000LL);
    if (!s->anon_bucket || !s->access_limit) {
        seterr(err, errlen, "out of memory");
        goto fail;
    }
    s->roll_slot = puddle_roll_slot_new();
    if (!s->roll_slot) {
        seterr(err, errlen, "out of memory");
        goto fail;
    }
    s->ctx = (puddle_ctx){ .config = &s->cfg, .identity = s->identity, .store = s->store, .profiles = s->profiles,
                           .assignments = s->assignments, .streams = s->streams, .people = s->people, .conversations = s->conversations, .groups = s->groups, .outbox = s->outbox, .replay = s->replay,
                           .anon_bucket = s->anon_bucket, .access_limit = s->access_limit, .roll_slot = s->roll_slot,
                           .peer_key = puddle_streams_peer_key, .peer_key_user = s->streams, .courier = &puddle_federation_courier };
    s->router = (puddle_router){ .ctx = &s->ctx };
    s->router.routes = puddle_all_routes(&s->router.count);
    {
        size_t np = puddle_profiles_ids(s->profiles, NULL, 0);
        puddle_log(PUDDLE_LOG_INFO, "%zu profile%s in %s/profiles", np, np == 1 ? "" : "s", cfg->data_dir);
    }
    s->tls = puddle_tls_context(cfg->dir, err, errlen);
    if (!s->tls)
        goto fail;
    puddle_tls_fingerprint_hex(s->tls, s->ctx.own_tls_fp);
    if (open_listener(s, err, errlen) != 0)
        goto fail;
    if (puddle_waker_open(&s->stop) != 0) {
        seterr(err, errlen, "cannot make the stop signal: %s", strerror(errno));
        goto fail;
    }

    if (strchr(cfg->address, ':'))
        puddle_log(PUDDLE_LOG_INFO, "listening on https://[%s]:%u", cfg->address, cfg->port);
    else
        puddle_log(PUDDLE_LOG_INFO, "listening on https://%s:%u", cfg->address, cfg->port);
    return s;
fail:
    puddle_server_destroy(s);
    return NULL;
}

puddle_store *puddle_server_store(puddle_server *s)
{
    return s->store;
}

puddle_profiles *puddle_server_profiles(puddle_server *s)
{
    return s->profiles;
}

puddle_identity *puddle_server_identity(puddle_server *s)
{
    return s->identity;
}

puddle_assignments *puddle_server_assignments(puddle_server *s)
{
    return s->assignments;
}

puddle_streams *puddle_server_streams(puddle_server *s)
{
    return s->streams;
}

puddle_people *puddle_server_people(puddle_server *s)
{
    return s->people;
}

puddle_groups *puddle_server_groups(puddle_server *s)
{
    return s->groups;
}

puddle_outbox *puddle_server_outbox(puddle_server *s)
{
    return s->outbox;
}

void puddle_server_stop(puddle_server *s)
{
    if (s)
        puddle_waker_wake(&s->stop);
}

void puddle_server_destroy(puddle_server *s)
{
    if (!s)
        return;
    puddle_sock_close(s->listener);
    puddle_waker_close(&s->stop);
    SSL_CTX_free(s->tls);
    puddle_replay_free(s->replay);
    puddle_roll_slot_free(s->roll_slot);
    puddle_outbox_close(s->outbox); /* stops its worker before what the worker uses goes away */
    puddle_streams_close(s->streams);
    puddle_people_close(s->people);
    puddle_groups_close(s->groups);
    puddle_bucket_free(s->anon_bucket);
    puddle_keyed_free(s->access_limit);
    puddle_assignments_close(s->assignments);
    puddle_identity_close(s->identity);
    puddle_auth_close(s->auth);
    puddle_profiles_close(s->profiles);
    puddle_store_close(s->store);
    puddle_conversations_close(s->conversations); /* after the store, which tells it of appends */
    delete s;
}

/* TLS as a connection for puddle_http_serve(). The socket's receive timeout makes a stalled read fail with
 * EAGAIN, which is reported as a timeout (< 0) rather than as the other side closing (0). */
static int ssl_read_cb(void *io, void *buf, size_t cap)
{
    SSL *ssl = (SSL *)io;
    errno = 0;
    int n = SSL_read(ssl, buf, (int)cap);
    if (n > 0)
        return n;
    int e = errno;
    if (SSL_get_error(ssl, n) == SSL_ERROR_ZERO_RETURN)
        return 0;
    return (e == EAGAIN || e == EWOULDBLOCK) ? -1 : 0;
}

static int ssl_write_cb(void *io, const void *buf, size_t len)
{
    SSL *ssl = (SSL *)io;
    const char *p = (const char *)buf;
    while (len) {
        int w = SSL_write(ssl, p, (int)len);
        if (w <= 0)
            return -1;
        p += w;
        len -= (size_t)w;
    }
    return 0;
}

static int owner_check_cb(void *user, const char *authorization)
{
    return puddle_auth_check_bearer(((puddle_server *)user)->auth, authorization);
}

static size_t body_limit_cb(void *user, const puddle_request *req, int owner_ok)
{
    return puddle_router_body_limit(&((puddle_server *)user)->router, req, owner_ok);
}

static size_t route_big_cb(void *user, const puddle_request *req, int owner_ok, const unsigned char *body, size_t body_len, char *out,
                           size_t cap, int *status, puddle_big *big)
{
    return puddle_router_handle_big(&((puddle_server *)user)->router, req, owner_ok, body, body_len, out, cap, status, big);
}

static size_t route_cb(void *user, const puddle_request *req, int owner_ok, const unsigned char *body,
                       size_t body_len, char *out, size_t cap, int *status)
{
    return puddle_router_handle(&((puddle_server *)user)->router, req, owner_ok, body, body_len, out, cap, status);
}

static void handle_request(puddle_server *srv, SSL *ssl, const char *peer)
{
    puddle_http_handler h = { .owner_check = owner_check_cb, .body_limit = body_limit_cb, .route = route_cb, .route_big = route_big_cb,
                              .user = srv, .deadline_seconds = REQUEST_DEADLINE_SEC };
    puddle_io io = { .read = ssl_read_cb, .write = ssl_write_cb, .io = ssl };
    puddle_serve_info info;
    puddle_http_serve(&h, &io, &info);
    if (info.status) /* the owner reads this log, so say why a 404 happened; the client is told nothing */
        puddle_log(PUDDLE_LOG_INFO, "%s %s %s [%s] -> %d", peer, info.method, info.path, info.auth, info.status);
}

static void *conn_main(void *arg)
{
    conn *c = (conn *)arg;
    puddle_server *s = c->srv;

    SSL *ssl = SSL_new(s->tls);
    if (ssl && SSL_set_fd(ssl, puddle_sock_fd(c->sock)) == 1) {
        if (SSL_accept(ssl) == 1) {
            handle_request(s, ssl, c->peer);
            SSL_shutdown(ssl);
            puddle_sock_finish(c->sock); /* so closing does not reset the connection under the answer */
        } else {
            /* expected whenever a client rejects the self-signed cert */
            puddle_log(PUDDLE_LOG_INFO, "%s TLS handshake failed", c->peer);
        }
    }
    SSL_free(ssl);
    puddle_sock_close(c->sock);
    free(c);

    s->mu.lock();
    if (--s->active == 0)
        s->drained.notify_all();
    s->mu.unlock();
    return NULL;
}

int puddle_server_run(puddle_server *s)
{
    int rc = 0;
    int synced = puddle_sync_resume(&s->ctx);
    if (synced)
        puddle_log(PUDDLE_LOG_INFO, "queued %d update%s for the people we are connected to", synced, synced == 1 ? "" : "s");
    int resumed = puddle_flow_resume(&s->ctx);
    if (resumed)
        puddle_log(PUDDLE_LOG_INFO, "queued %d answer%s that were still owed", resumed, resumed == 1 ? "" : "s");
    puddle_group_reconcile(&s->ctx); /* connect to group members we are not yet connected to */
    if (puddle_outbox_start(s->outbox) != 0)
        puddle_log(PUDDLE_LOG_WARN, "cannot start the outbox; nothing will be sent to other Puddles");

    for (;;) {
        puddle_sock socks[2] = { s->listener, s->stop.read_end };
        int which[2];
        int ready = puddle_sock_poll(socks, 2, 60 * 1000, which);
        if (ready == 0) { /* a quiet minute: look again for group members we asked and who never answered */
            puddle_group_reconcile(&s->ctx);
            continue;
        }
        if (ready < 0) {
            puddle_log(PUDDLE_LOG_ERROR, "poll: %s", strerror(errno));
            rc = -1;
            break;
        }
        if (which[1])
            break;
        if (which[0] == 2) {
            puddle_log(PUDDLE_LOG_ERROR, "listening socket failed");
            rc = -1;
            break;
        }
        if (!which[0])
            continue;

        char peer[64];
        int out_of_fds;
        puddle_sock sock = puddle_sock_accept(s->listener, IO_TIMEOUT_SEC, peer, sizeof peer, &out_of_fds);
        if (sock == PUDDLE_SOCK_NONE) {
            if (out_of_fds) {
                puddle_log(PUDDLE_LOG_WARN, "accept: out of file descriptors");
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }
        conn *c = (conn *)calloc(1, sizeof *c);
        if (!c) {
            puddle_sock_close(sock);
            continue;
        }
        c->srv = s;
        c->sock = sock;
        snprintf(c->peer, sizeof c->peer, "%s", peer);

        s->mu.lock();
        int busy = s->active >= MAX_CONNECTIONS;
        if (!busy)
            s->active++;
        s->mu.unlock();
        if (busy) {
            puddle_log(PUDDLE_LOG_WARN, "%s rejected: too many connections", c->peer);
            puddle_sock_close(sock);
            free(c);
            continue;
        }

        bool started = true;
        try {
            std::thread(conn_main, c).detach();
        } catch (const std::system_error &e) {
            started = false;
            puddle_log(PUDDLE_LOG_ERROR, "cannot start a connection thread: %s", e.what());
        }
        if (!started) {
            s->mu.lock();
            s->active--;
            s->mu.unlock();
            puddle_sock_close(sock);
            free(c);
        }
    }

    puddle_sock_close(s->listener);
    s->listener = PUDDLE_SOCK_NONE;

    /* in-flight connections are bounded by IO_TIMEOUT_SEC / REQUEST_DEADLINE_SEC */
    s->mu.lock();
    while (s->active > 0)
        wait_on(s->drained, s->mu);
    s->mu.unlock();

    puddle_log(PUDDLE_LOG_INFO, "stopped");
    return rc;
}
