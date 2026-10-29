#include "federation/ponds.h"

#include "base/hash.h"
#include "base/log.h"
#include "core/identity.h"
#include "core/ponds.h"
#include "core/profile.h"
#include "http/client.h"
#include "http/reqsig.h"
#include "platform/clock.h"

#include <openssl/rand.h>

#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#define HISTORY_PAGE 50
#define EVENTS_MAX_BODY (16u << 20)  /* 200 events, each up to a message or a member with a profile */
#define MEMBERS_MAX_BODY (64u << 20)
#define BACKOFF_MAX_S 60

/* ---------------------------------------------------------------- one call */

namespace {

/* Who may cut a call short, and the certificate it must show. */
struct Call {
    const char *expect_fp = nullptr; /* NULL or "": take whatever is shown (joining) */
    std::mutex *mu = nullptr;
    puddle_sock *sock = nullptr;
    const bool *stopping = nullptr;
};

} // namespace

static int fp_ok(void *u, const char *fp)
{
    Call *c = (Call *)u;
    return !c->expect_fp || !*c->expect_fp || !strcmp(c->expect_fp, fp);
}

static void on_socket(void *u, puddle_sock s)
{
    Call *c = (Call *)u;
    if (!c->mu)
        return;
    std::lock_guard<std::mutex> g(*c->mu);
    *c->sock = s;
    if (s != PUDDLE_SOCK_NONE && *c->stopping)
        puddle_sock_cut(s);
}

/* A signed request to a Pond. */
static void pond_call(puddle_ctx *ctx, const puddle_address *addr, const char *recipient, const char *method, const std::string &target,
                      const std::string &body, size_t max_body, int total_s, Call *call, puddle_net_reply *out)
{
    puddle_net_opts o;
    memset(&o, 0, sizeof o);
    o.connect_timeout_s = 8;
    o.io_timeout_s = total_s > 30 ? total_s : 15;
    o.total_timeout_s = total_s;
    o.max_body = max_body;
    o.fingerprint_ok = fp_ok;
    o.on_socket = on_socket;
    o.user = call;
    puddle_net_request(addr, method, target.c_str(), body.empty() ? NULL : body.data(), body.size(), ctx->identity, recipient, &o, out);
}

static std::string chain_json(puddle_ctx *ctx)
{
    size_t len;
    char *chain = puddle_identity_chain(ctx->identity, &len);
    if (!chain)
        return "";
    std::string buf(len * 2 + 64, '\0');
    puddle_jw w;
    puddle_jw_init(&w, buf.data(), buf.size());
    puddle_jw_string_n(&w, chain, len);
    free(chain);
    size_t n = puddle_jw_finish(&w);
    return n ? buf.substr(0, n) : "";
}

/* {"profile": ...}: what the owner chose for "pond:<id>" (`place`), or null. Pictures stay out: a Pond has no way to fetch them. */
static void write_profile(puddle_ctx *ctx, const char *place, const char *profile_id, puddle_jw *w)
{
    puddle_profile p;
    int have = !ctx->profiles ? 1
               : profile_id   ? puddle_profiles_get(ctx->profiles, profile_id, &p)
                              : puddle_profile_for_place(ctx->profiles, ctx->assignments, place, &p);
    puddle_jw_key(w, "profile");
    if (have != 0) {
        puddle_jw_null(w);
        return;
    }
    p.avatar.hash[0] = p.banner.hash[0] = p.background.hash[0] = '\0';
    puddle_jw_object(w);
    puddle_profile_write_fields(w, &p);
    puddle_jw_end_object(w);
    puddle_profile_free(&p);
}

/* The `error` a Pond answered with, if it is one we pass on; else `fallback`. */
static const char *pond_error(const puddle_net_reply *r, const char *fallback)
{
    static const char *const KNOWN[] = {"no_channel", "bad_text", "slow_down", "bad_query"};
    puddle_span v;
    char e[32];
    if (r->body && puddle_json_member(r->body, r->body_len, "error", &v) == 0 && puddle_json_string(v, e, sizeof e) == 0)
        for (const char *k : KNOWN)
            if (!strcmp(e, k))
                return k;
    return fallback;
}

/* A 404 that is not about a channel is the Pond's answer to someone it does not know: it no longer has us as a member. */
static bool refused(const puddle_net_reply *r)
{
    return r->result == PUDDLE_NET_OK && r->status == 404 && strcmp(pond_error(r, ""), "no_channel") != 0;
}

/* ---------------------------------------------------------------- the followers */

namespace {

struct Follower {
    std::string id;
    std::thread thread;
    std::mutex mu;
    std::condition_variable cv;
    puddle_sock sock = PUDDLE_SOCK_NONE;
    bool stopping = false;
    bool poked = false;        /* something to say: wake from a wait */
    bool send_profile = false; /* the owner chose another profile for it */
    bool send_keys = false;    /* our keys rolled */
};

} // namespace

struct puddle_pond_followers {
    puddle_ctx *ctx;
    std::mutex mu;
    std::map<std::string, std::unique_ptr<Follower>> all;
};

/* Wake `f` from a wait or a request held open, to say something. */
static void poke(Follower *f)
{
    std::lock_guard<std::mutex> g(f->mu);
    f->poked = true;
    if (f->sock != PUDDLE_SOCK_NONE)
        puddle_sock_cut(f->sock);
    f->cv.notify_all();
}

/* Sleep `ms`, unless stopped or poked first. false if stopped. */
static bool rest(Follower *f, int64_t ms)
{
    std::unique_lock<std::mutex> lk(f->mu);
    f->cv.wait_for(lk, std::chrono::milliseconds(ms), [&] { return f->stopping || f->poked; });
    f->poked = false;
    return !f->stopping;
}

enum Outcome { GOT, GONE, AWAY, CERT };

/* A call from the follower: what came of it, with the reply for GOT (the caller frees it). */
static Outcome follow_call(puddle_ctx *ctx, Follower *f, const char *method, const std::string &target, const std::string &body,
                           size_t max_body, int total_s, puddle_net_reply *r)
{
    puddle_pond_conn conn;
    memset(r, 0, sizeof *r);
    if (puddle_ponds_conn(ctx->ponds, f->id.c_str(), &conn) != 0)
        return GONE;
    Call call;
    call.expect_fp = conn.tls_fp;
    call.mu = &f->mu;
    call.sock = &f->sock;
    call.stopping = &f->stopping;
    pond_call(ctx, &conn.address, conn.id, method, target, body, max_body, total_s, &call, r);
    if (r->result == PUDDLE_NET_CERT_CHANGED)
        return CERT;
    if (r->result != PUDDLE_NET_OK)
        return AWAY;
    if (refused(r))
        return GONE;
    return (r->status >= 200 && r->status < 300) || r->status == 404 ? GOT : AWAY;
}

/* The newest messages of each channel not yet held: just joined, a new channel, or after a reset. */
static Outcome fill(puddle_ctx *ctx, Follower *f)
{
    int64_t *chans;
    size_t n;
    if (puddle_ponds_unfilled(ctx->ponds, f->id.c_str(), &chans, &n) != 0)
        return GONE;
    Outcome o = GOT;
    for (size_t i = 0; i < n && o == GOT; i++) {
        puddle_net_reply r;
        std::string target = "/pond/v1/channels/" + std::to_string(chans[i]) + "/messages?limit=" + std::to_string(HISTORY_PAGE);
        o = follow_call(ctx, f, "GET", target, "", 4u << 20, 20, &r);
        if (o == GOT && r.status == 200) /* a 404 here: that channel went meanwhile */
            puddle_ponds_history(ctx->ponds, f->id.c_str(), chans[i], 0, r.body, r.body_len, HISTORY_PAGE);
        puddle_net_reply_free(&r);
    }
    free(chans);
    return o;
}

/* Fetch what a Pond is now: /info, the members, and the newest messages of each channel not yet held. */
static Outcome sync_pond(puddle_ctx *ctx, Follower *f, bool reset)
{
    puddle_net_reply r;
    Outcome o = follow_call(ctx, f, "GET", "/pond/v1/info", "", 1u << 20, 20, &r);
    if (o == GOT && (r.status != 200 || puddle_ponds_info(ctx->ponds, f->id.c_str(), r.body, r.body_len, reset) != 0))
        o = AWAY;
    puddle_net_reply_free(&r);
    if (o != GOT)
        return o;
    o = follow_call(ctx, f, "GET", "/pond/v1/members", "", MEMBERS_MAX_BODY, 30, &r);
    if (o == GOT && r.status == 200)
        puddle_ponds_members(ctx->ponds, f->id.c_str(), r.body, r.body_len);
    puddle_net_reply_free(&r);
    return o == GOT ? fill(ctx, f) : o;
}

/* What the owner asked to be said to this Pond meanwhile. */
static void say_what_waits(puddle_ctx *ctx, Follower *f)
{
    bool profile, keys;
    {
        std::lock_guard<std::mutex> g(f->mu);
        profile = f->send_profile, keys = f->send_keys;
    }
    puddle_net_reply r;
    if (keys) {
        std::string body = "{\"chain\":" + chain_json(ctx) + "}";
        if (follow_call(ctx, f, "POST", "/pond/v1/keys", body, 4096, 20, &r) == GOT) {
            std::lock_guard<std::mutex> g(f->mu);
            f->send_keys = false;
        }
        puddle_net_reply_free(&r);
    }
    if (profile) {
        char place[PUDDLE_PLACE_MAX];
        std::string body(PUDDLE_PROFILE_JSON_MAX + 64, '\0');
        puddle_jw w;
        puddle_jw_init(&w, body.data(), body.size());
        puddle_jw_object(&w);
        puddle_place_pond(f->id.c_str(), place);
        write_profile(ctx, place, NULL, &w);
        puddle_jw_end_object(&w);
        size_t n = puddle_jw_finish(&w);
        if (n && follow_call(ctx, f, "POST", "/pond/v1/profile", body.substr(0, n), 4096, 20, &r) == GOT) {
            std::lock_guard<std::mutex> g(f->mu);
            f->send_profile = false;
        }
        puddle_net_reply_free(&r);
    }
}

static void follow(puddle_ctx *ctx, Follower *f)
{
    const char *id = f->id.c_str();
    bool synced = false, reset = false;
    int backoff = 0;
    while (true) {
        {
            std::lock_guard<std::mutex> g(f->mu);
            if (f->stopping)
                break;
            f->poked = false;
        }
        say_what_waits(ctx, f);
        Outcome o = GOT;
        if (!synced) {
            o = sync_pond(ctx, f, reset);
            if (o == GOT)
                synced = true, reset = false;
        }
        puddle_net_reply r;
        memset(&r, 0, sizeof r);
        if (o == GOT) {
            puddle_pond_conn conn;
            if (puddle_ponds_conn(ctx->ponds, id, &conn) != 0)
                break;
            puddle_ponds_set_status(ctx->ponds, id, "connected");
            o = follow_call(ctx, f, "GET", "/pond/v1/events?after=" + std::to_string(conn.cursor), "", EVENTS_MAX_BODY, 40, &r);
            if (o == GOT) {
                int e = r.status == 200 ? puddle_ponds_events(ctx->ponds, id, r.body, r.body_len) : -1;
                if (e == 1)
                    synced = false, reset = true;
                else if (e == 0)
                    o = fill(ctx, f); /* a channel that is new has nothing held yet */
                else
                    o = AWAY;
            }
        }
        puddle_net_reply_free(&r);
        bool cut;
        {
            std::lock_guard<std::mutex> g(f->mu);
            cut = f->stopping || f->poked;
        }
        if (o == GOT || cut) {
            backoff = 0;
            continue;
        }
        if (o == GONE) {
            puddle_ponds_set_status(ctx->ponds, id, "removed");
            puddle_log(PUDDLE_LOG_INFO, "pond %.12s… no longer knows us; not following it", id);
            break;
        }
        puddle_ponds_set_status(ctx->ponds, id, o == CERT ? "certificate_changed" : "unreachable");
        backoff = backoff ? std::min(backoff * 2, BACKOFF_MAX_S) : 2;
        if (!rest(f, o == CERT ? BACKOFF_MAX_S * 1000 : backoff * 1000LL))
            break;
    }
}

static void start_one(puddle_pond_followers *fs, const char *id)
{
    std::lock_guard<std::mutex> g(fs->mu);
    if (fs->all.count(id))
        return;
    auto f = std::make_unique<Follower>();
    f->id = id;
    Follower *raw = f.get();
    try {
        f->thread = std::thread(follow, fs->ctx, raw);
    } catch (const std::system_error &) {
        puddle_log(PUDDLE_LOG_ERROR, "cannot start following pond %.12s…", id);
        return;
    }
    fs->all[id] = std::move(f);
}

static void stop_one(std::unique_ptr<Follower> &f)
{
    {
        std::lock_guard<std::mutex> g(f->mu);
        f->stopping = true;
        if (f->sock != PUDDLE_SOCK_NONE)
            puddle_sock_cut(f->sock);
        f->cv.notify_all();
    }
    if (f->thread.joinable())
        f->thread.join();
}

puddle_pond_followers *puddle_pond_followers_start(puddle_ctx *ctx)
{
    auto *fs = new puddle_pond_followers();
    fs->ctx = ctx;
    char(*ids)[PUDDLE_USER_ID_LEN + 1];
    size_t n;
    if (ctx->ponds && puddle_ponds_ids(ctx->ponds, &ids, &n) == 0) {
        for (size_t i = 0; i < n; i++)
            start_one(fs, ids[i]);
        free(ids);
    }
    return fs;
}

void puddle_pond_followers_stop(puddle_pond_followers *fs)
{
    if (!fs)
        return;
    std::map<std::string, std::unique_ptr<Follower>> all;
    {
        std::lock_guard<std::mutex> g(fs->mu);
        all.swap(fs->all);
    }
    for (auto &[id, f] : all)
        stop_one(f);
    delete fs;
}

static void stop_following(puddle_ctx *ctx, const char *id)
{
    if (!ctx->pond_followers)
        return;
    std::unique_ptr<Follower> f;
    {
        std::lock_guard<std::mutex> g(ctx->pond_followers->mu);
        auto it = ctx->pond_followers->all.find(id);
        if (it == ctx->pond_followers->all.end())
            return;
        f = std::move(it->second);
        ctx->pond_followers->all.erase(it);
    }
    stop_one(f);
}

/* Ask a Pond's follower to say something: `profile` or `keys`. */
static int ask_follower(puddle_ctx *ctx, const char *id, bool profile)
{
    if (!ctx->pond_followers)
        return -1;
    std::lock_guard<std::mutex> g(ctx->pond_followers->mu);
    auto it = ctx->pond_followers->all.find(id);
    if (it == ctx->pond_followers->all.end())
        return -1;
    Follower *f = it->second.get();
    {
        std::lock_guard<std::mutex> fg(f->mu);
        (profile ? f->send_profile : f->send_keys) = true;
    }
    poke(f);
    return 0;
}

/* ---------------------------------------------------------------- what the owner does */

int puddle_pond_join(puddle_ctx *ctx, const puddle_link *link, const char *profile_id, char out_id[PUDDLE_USER_ID_LEN + 1],
                     const char **code)
{
    if (!ctx->ponds || !link->invite[0]) {
        *code = "failed";
        return -1;
    }
    if (profile_id && (!ctx->profiles || !puddle_profiles_has(ctx->profiles, profile_id))) {
        *code = "no_profile";
        return -1;
    }
    std::string chain = chain_json(ctx), body(PUDDLE_PROFILE_JSON_MAX + chain.size() + 256, '\0');
    puddle_jw w;
    puddle_jw_init(&w, body.data(), body.size());
    puddle_jw_object(&w);
    puddle_jw_str(&w, "invite", link->invite);
    puddle_jw_key(&w, "chain");
    puddle_jw_raw(&w, chain.data(), chain.size());
    if (profile_id)
        write_profile(ctx, NULL, profile_id, &w);
    puddle_jw_end_object(&w);
    size_t n = puddle_jw_finish(&w);
    if (!n || chain.empty()) {
        *code = "failed";
        return -1;
    }
    Call call; /* first contact: whatever certificate it shows is the one we keep */
    puddle_net_reply r;
    pond_call(ctx, &link->address, PUDDLE_RECIPIENT_ANY, "POST", "/pond/v1/join", body.substr(0, n), 1u << 20, 30, &call, &r);
    int rc = -1;
    if (r.result == PUDDLE_NET_OFFLINE)
        *code = "unreachable";
    else if (r.result != PUDDLE_NET_OK || (r.status != 200 && r.status != 404))
        *code = "not_a_pond";
    else if (r.status == 404)
        *code = "invite_refused";
    else if (puddle_ponds_add(ctx->ponds, &link->address, r.fingerprint, r.body, r.body_len, out_id) != 0)
        *code = "not_a_pond";
    else
        rc = 0;
    puddle_net_reply_free(&r);
    if (rc != 0)
        return rc;
    char place[PUDDLE_PLACE_MAX], err[160];
    if (ctx->assignments && puddle_place_pond(out_id, place) == 0)
        puddle_assignments_set(ctx->assignments, place, profile_id, err, sizeof err);
    puddle_log(PUDDLE_LOG_INFO, "joined pond %.12s… at %s", out_id, link->address.address);
    if (ctx->pond_followers)
        start_one(ctx->pond_followers, out_id);
    return 0;
}

/* A call the owner is waiting on. */
static bool owner_call(puddle_ctx *ctx, const char *pond, const char *method, const std::string &target, const std::string &body,
                       size_t max_body, puddle_net_reply *r, const char **code)
{
    puddle_pond_conn conn;
    memset(r, 0, sizeof *r);
    if (!ctx->ponds || puddle_ponds_conn(ctx->ponds, pond, &conn) != 0) {
        *code = "no_pond";
        return false;
    }
    Call call;
    call.expect_fp = conn.tls_fp;
    pond_call(ctx, &conn.address, conn.id, method, target, body, max_body, 20, &call, r);
    if (r->result == PUDDLE_NET_OK)
        return true;
    *code = r->result == PUDDLE_NET_OFFLINE ? "unreachable" : r->result == PUDDLE_NET_CERT_CHANGED ? "certificate_changed" : "failed";
    return false;
}

int puddle_pond_send(puddle_ctx *ctx, const char *pond, int64_t channel, const char *text, const char **code)
{
    if (!ctx->ponds || !puddle_ponds_has_channel(ctx->ponds, pond, channel)) {
        *code = ctx->ponds && puddle_ponds_conn(ctx->ponds, pond, NULL) == 0 ? "no_channel" : "no_pond";
        return -1;
    }
    unsigned char rnd[12];
    char nonce[25];
    RAND_bytes(rnd, sizeof rnd);
    puddle_hex_encode(rnd, sizeof rnd, nonce);
    size_t tl = strlen(text);
    std::string body(tl * 6 + 128, '\0');
    puddle_jw w;
    puddle_jw_init(&w, body.data(), body.size());
    puddle_jw_object(&w);
    puddle_jw_str(&w, "text", text);
    puddle_jw_str(&w, "nonce", nonce);
    puddle_jw_end_object(&w);
    size_t n = puddle_jw_finish(&w);
    puddle_net_reply r;
    std::string target = "/pond/v1/channels/" + std::to_string(channel) + "/send";
    int rc = -1;
    /* the nonce makes a second try safe: if the first was posted and only its answer lost, the Pond posts nothing new */
    for (int attempt = 0; attempt < 2 && rc != 0; attempt++) {
        if (attempt)
            puddle_net_reply_free(&r);
        if (!owner_call(ctx, pond, "POST", target, body.substr(0, n), 1u << 16, &r, code))
            continue;
        if (r.status == 200 && puddle_ponds_sent(ctx->ponds, pond, r.body, r.body_len) == 0)
            rc = 0;
        else
            *code = refused(&r) ? "removed" : pond_error(&r, "failed");
        break;
    }
    puddle_net_reply_free(&r);
    return rc;
}

int puddle_pond_history(puddle_ctx *ctx, const char *pond, int64_t channel, int64_t before, int limit, puddle_jw *w, const char **code)
{
    puddle_net_reply r;
    std::string target = "/pond/v1/channels/" + std::to_string(channel) + "/messages?limit=" + std::to_string(limit);
    if (before > 0)
        target += "&before=" + std::to_string(before);
    if (!owner_call(ctx, pond, "GET", target, "", 4u << 20, &r, code)) {
        puddle_net_reply_free(&r);
        return -1;
    }
    int rc = -1;
    if (r.status != 200)
        *code = refused(&r) ? "removed" : pond_error(&r, "failed");
    else if (puddle_ponds_write_page(r.body, r.body_len, limit, w) != 0)
        *code = "failed";
    else {
        puddle_ponds_history(ctx->ponds, pond, channel, before, r.body, r.body_len, limit); /* kept, if it joins onto what is */
        rc = 0;
    }
    puddle_net_reply_free(&r);
    return rc;
}

int puddle_pond_leave(puddle_ctx *ctx, const char *pond, const char **code)
{
    if (!ctx->ponds || puddle_ponds_conn(ctx->ponds, pond, NULL) != 0) {
        *code = "no_pond";
        return -1;
    }
    stop_following(ctx, pond);
    puddle_net_reply r;
    const char *ignored;
    if (owner_call(ctx, pond, "POST", "/pond/v1/leave", "", 4096, &r, &ignored) && r.status != 204)
        puddle_log(PUDDLE_LOG_INFO, "pond %.12s… answered %d to leaving; forgetting it anyway", pond, r.status);
    puddle_net_reply_free(&r);
    puddle_ponds_remove(ctx->ponds, pond);
    char place[PUDDLE_PLACE_MAX], err[160];
    if (ctx->assignments && puddle_place_pond(pond, place) == 0)
        puddle_assignments_set(ctx->assignments, place, NULL, err, sizeof err);
    return 0;
}

int puddle_pond_profile_push(puddle_ctx *ctx, const char *pond)
{
    return ask_follower(ctx, pond, true);
}

void puddle_ponds_keys_rolled(puddle_ctx *ctx)
{
    char(*ids)[PUDDLE_USER_ID_LEN + 1];
    size_t n;
    if (!ctx->ponds || puddle_ponds_ids(ctx->ponds, &ids, &n) != 0)
        return;
    for (size_t i = 0; i < n; i++)
        ask_follower(ctx, ids[i], false);
    free(ids);
}
