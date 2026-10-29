#include "federation/ponds.h"

#include "base/encoding.h"
#include "base/hash.h"
#include "base/log.h"
#include "core/identity.h"
#include "core/ponds.h"
#include "core/profile.h"
#include "events/message.h"
#include "http/client.h"
#include "http/reqsig.h"
#include "platform/clock.h"
#include "platform/fsutil.h"
#include "storage/files.h"

#include <openssl/rand.h>

#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <set>
#include <sstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#define HISTORY_PAGE 50
#define EVENTS_MAX_BODY (16u << 20)  /* 200 events, each up to a message or a member with a profile */
#define MEMBERS_MAX_BODY (64u << 20)
#define BACKOFF_MAX_S 60
#define FETCH_REST_S 60                            /* how often the fetcher looks again when nothing wakes it */
#define ASKED_KEEP_MS (7LL * 24 * 3600 * 1000)     /* a file the owner asked for is kept this long past its message's */

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
    static const char *const KNOWN[] = {"no_channel", "bad_text", "slow_down", "bad_query", "files_off", "too_large", "no_file", "no_chunk"};
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
    const char *e = pond_error(r, "");
    return r->result == PUDDLE_NET_OK && r->status == 404 && strcmp(e, "no_channel") != 0 && strcmp(e, "no_file") != 0 &&
           strcmp(e, "no_chunk") != 0;
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

    /* the fetcher: keeps the files of the kept messages here (docs/ponds.md, "Files"), in a thread of its own so a large file
     * never holds up the events */
    std::thread fetch_thread;
    std::condition_variable fetch_cv;
    puddle_sock fetch_sock = PUDDLE_SOCK_NONE;
    bool fetch_poked = false;
    std::vector<std::string> asked; /* "<file> <channel> <message>" the owner asked for, not yet noted in files.txt */
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
                           size_t max_body, int total_s, puddle_net_reply *r, bool fetcher = false)
{
    puddle_pond_conn conn;
    memset(r, 0, sizeof *r);
    if (puddle_ponds_conn(ctx->ponds, f->id.c_str(), &conn) != 0)
        return GONE;
    Call call;
    call.expect_fp = conn.tls_fp;
    call.mu = &f->mu;
    call.sock = fetcher ? &f->fetch_sock : &f->sock;
    call.stopping = &f->stopping;
    pond_call(ctx, &conn.address, conn.id, method, target, body, max_body, total_s, &call, r);
    if (r->result == PUDDLE_NET_CERT_CHANGED)
        return CERT;
    if (r->result != PUDDLE_NET_OK)
        return AWAY;
    if (refused(r))
        return GONE;
    return (r->status >= 200 && r->status < 300) || r->status == 404 || r->status == 429 ? GOT : AWAY; /* 429: slow down, it is there */
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


/* ---------------------------------------------------------------- the fetcher: files of the kept messages (docs/ponds.md, "Files") */

/* Each file this Pond's messages hold in the file store: which message (so the store keeps it while that message is kept), and
 * until when if the owner asked for it (0: while the message is kept). In <data_dir>/ponds/<pond>/files.txt, a line each:
 * "<file> <channel> <message> <until>". */
struct Held {
    std::string file;
    int64_t channel = 0, message = 0, until = 0;
    std::string key() const { return file + " " + std::to_string(channel) + " " + std::to_string(message); }
};

static std::string held_path(puddle_ctx *ctx, const std::string &pond)
{
    return std::string(ctx->config->data_dir) + "/ponds/" + pond + "/files.txt";
}

static std::string chat_of(const std::string &pond, int64_t channel)
{
    return "pond:" + pond + "/" + std::to_string(channel);
}

static std::map<std::string, Held> read_held(puddle_ctx *ctx, const std::string &pond)
{
    std::map<std::string, Held> out;
    std::ifstream in(held_path(ctx, pond));
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        Held h;
        if (ls >> h.file >> h.channel >> h.message >> h.until && puddle_file_id_valid(h.file.c_str()))
            out[h.key()] = h;
    }
    return out;
}

static void write_held(puddle_ctx *ctx, const std::string &pond, const std::map<std::string, Held> &held)
{
    std::string data;
    for (auto &[k, h] : held)
        data += k + " " + std::to_string(h.until) + "\n";
    std::string dir = std::string(ctx->config->data_dir) + "/ponds/" + pond;
    if (puddle_path_is_dir(dir.c_str())) /* not after leaving */
        puddle_write_file_atomic(dir.c_str(), held_path(ctx, pond).c_str(), data.data(), data.size());
}

/* The store no longer needs to keep `h`'s file for this Pond: let it go if nothing else carries it. */
static void let_go(puddle_ctx *ctx, const std::string &pond, const Held &h)
{
    std::string msg = std::to_string(h.message);
    if (puddle_files_unref(ctx->files, h.file.c_str(), chat_of(pond, h.channel).c_str(), msg.c_str()) == 0)
        puddle_files_remove(ctx->files, h.file.c_str());
}

/* Fetch one file whole from the Pond, a chunk at a time, checking each; a download that stopped picks up where it was. */
static Outcome fetch_file(puddle_ctx *ctx, Follower *f, const std::string &file, int64_t size)
{
    if (puddle_files_state(ctx->files, file.c_str(), nullptr, nullptr) == PUDDLE_FILE_WHOLE)
        return GOT;
    uint32_t count = puddle_file_chunk_count(size);
    puddle_net_reply r;
    std::string F = "/pond/v1/files/" + file + "/chunks";
    if (puddle_files_fetch_next(ctx->files, file.c_str()) < 0) { /* not begun yet */
        Outcome o = follow_call(ctx, f, "GET", F, "", (size_t)count * 32 + 64, 30, &r, true);
        bool ok = o == GOT && r.status == 200 && r.body_len == (size_t)count * 32 &&
                  puddle_files_fetch_begin(ctx->files, file.c_str(), size, (const unsigned char *)r.body, count) == 0;
        puddle_net_reply_free(&r);
        if (!ok)
            return o == GOT ? GONE : o; /* GONE here: this file only (the Pond will not give it, or not as the message says) */
    }
    for (int64_t n; (n = puddle_files_fetch_next(ctx->files, file.c_str())) >= 0;) {
        {
            std::lock_guard<std::mutex> g(f->mu);
            if (f->stopping)
                return AWAY;
        }
        Outcome o = follow_call(ctx, f, "GET", F + "/" + std::to_string(n), "", PUDDLE_FILE_CHUNK + 64, 120, &r, true);
        int rc = o == GOT && r.status == 200 ? puddle_files_fetch_put(ctx->files, file.c_str(), (uint32_t)n, (const unsigned char *)r.body, r.body_len) : 1;
        int status = r.status;
        puddle_net_reply_free(&r);
        if (o != GOT)
            return o;
        if (status == 429) { /* the Pond asks us to slow down */
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }
        if (rc != 0)
            return GONE;
    }
    if (puddle_files_fetch_finish(ctx->files, file.c_str()) != 0)
        return GONE;
    puddle_ponds_touch(ctx->ponds, f->id.c_str());
    return GOT;
}

/* The size of a file message `message` carries, asked of the Pond (a message older than what is kept). 0 if it does not carry it. */
static int64_t size_from_pond(puddle_ctx *ctx, Follower *f, int64_t channel, int64_t message, const std::string &file)
{
    puddle_net_reply r;
    std::string target = "/pond/v1/channels/" + std::to_string(channel) + "/messages?limit=1&before=" + std::to_string(message + 1);
    int64_t size = 0;
    if (follow_call(ctx, f, "GET", target, "", 1u << 20, 20, &r, true) == GOT && r.status == 200) {
        puddle_span all = {r.body, r.body_len}, arr, item, files, fv, v;
        size_t cur = 0, fcur = 0;
        if (r.body && puddle_json_member(all.p, all.len, "messages", &arr) == 0 && puddle_json_array_next(arr, &cur, &item) == 1 &&
            puddle_json_member(item.p, item.len, "id", &v) == 0 && puddle_json_member(item.p, item.len, "files", &files) == 0) {
            int64_t id = 0;
            puddle_json_int64(v, &id);
            while (id == message && puddle_json_array_next(files, &fcur, &fv) == 1) {
                char fid[PUDDLE_FILE_ID_LEN + 1];
                if (puddle_json_member(fv.p, fv.len, "id", &v) == 0 && puddle_json_string(v, fid, sizeof fid) == 0 && file == fid &&
                    puddle_json_member(fv.p, fv.len, "size", &v) == 0)
                    puddle_json_int64(v, &size);
            }
        }
    }
    puddle_net_reply_free(&r);
    return size;
}

/* One look: hold the files of the kept messages (and those the owner asked for) in the store, let go of the rest, and fetch
 * what is not here yet: the owner's asks first, then the newest messages' files up to files.auto_download each. */
static void fetch_round(puddle_ctx *ctx, Follower *f)
{
    const std::string &pond = f->id;
    int64_t now = puddle_wall_ms(), autodl = puddle_files_auto_download(ctx->config), max = puddle_files_max_size(ctx->config);
    std::map<std::string, Held> old = read_held(ctx, pond), now_held;
    struct Want {
        Held h;
        int64_t size;
        bool asked;
    };
    std::vector<Want> want;
    std::vector<std::string> asked;
    {
        std::lock_guard<std::mutex> g(f->mu);
        asked.swap(f->asked);
    }
    for (auto &a : asked) {
        std::istringstream ls(a);
        Held h;
        if (!(ls >> h.file >> h.channel >> h.message))
            continue;
        h.until = now + ASKED_KEEP_MS;
        now_held[h.key()] = h;
        int64_t size = 0;
        if (puddle_ponds_message_file(ctx->ponds, pond.c_str(), h.channel, h.message, h.file.c_str(), &size) != 0)
            size = size_from_pond(ctx, f, h.channel, h.message, h.file);
        if (size > 0)
            want.push_back({h, size, true});
    }
    for (auto &[k, h] : old) /* asked for before, and still within its week */
        if (h.until > now && !now_held.count(k))
            now_held[k] = h;
    struct Acc {
        std::map<std::string, Held> *held;
        std::vector<Want> *want;
    } acc{&now_held, &want};
    puddle_ponds_files(
        ctx->ponds, pond.c_str(),
        [](void *u, int64_t channel, int64_t message, const char *file, int64_t size) {
            auto *a = (Acc *)u;
            Held h;
            h.file = file, h.channel = channel, h.message = message;
            auto it = a->held->find(h.key());
            if (it == a->held->end())
                (*a->held)[h.key()] = h;
            a->want->push_back({h, size, false});
            return 0;
        },
        &acc);
    for (auto &[k, h] : now_held)
        if (!old.count(k))
            puddle_files_ref(ctx->files, h.file.c_str(), chat_of(pond, h.channel).c_str(), std::to_string(h.message).c_str(), now);
    for (auto &[k, h] : old)
        if (!now_held.count(k))
            let_go(ctx, pond, h);
    write_held(ctx, pond, now_held);

    std::set<std::string> tried;
    for (auto &w : want) {
        if (tried.count(w.h.file) || w.size > max || (!w.asked && w.size > autodl))
            continue;
        tried.insert(w.h.file);
        {
            std::lock_guard<std::mutex> g(f->mu);
            if (f->stopping)
                return;
        }
        Outcome o = fetch_file(ctx, f, w.h.file, w.size);
        if (o == AWAY || o == CERT)
            return; /* the Pond is away: the next look tries again */
        if (o == GONE)
            puddle_log(PUDDLE_LOG_INFO, "pond %.12s… would not give file %.12s…", pond.c_str(), w.h.file.c_str());
    }
}

static void fetch_loop(puddle_ctx *ctx, Follower *f)
{
    while (true) {
        if (ctx->files)
            fetch_round(ctx, f);
        std::unique_lock<std::mutex> lk(f->mu);
        f->fetch_cv.wait_for(lk, std::chrono::seconds(FETCH_REST_S), [&] { return f->stopping || f->fetch_poked; });
        f->fetch_poked = false;
        if (f->stopping)
            break;
    }
}

static void poke_fetcher(Follower *f)
{
    std::lock_guard<std::mutex> g(f->mu);
    f->fetch_poked = true;
    f->fetch_cv.notify_all();
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
            if (o == GOT) {
                synced = true, reset = false;
                poke_fetcher(f);
            }
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
                if (e == 0 && r.body_len > 40) /* not an empty answer: there may be files to fetch */
                    poke_fetcher(f);
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
        f->fetch_thread = std::thread(fetch_loop, fs->ctx, raw);
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
        if (f->fetch_sock != PUDDLE_SOCK_NONE)
            puddle_sock_cut(f->fetch_sock);
        f->cv.notify_all();
        f->fetch_cv.notify_all();
    }
    if (f->thread.joinable())
        f->thread.join();
    if (f->fetch_thread.joinable())
        f->fetch_thread.join();
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
                       size_t max_body, puddle_net_reply *r, const char **code, int total_s = 20)
{
    puddle_pond_conn conn;
    memset(r, 0, sizeof *r);
    if (!ctx->ponds || puddle_ponds_conn(ctx->ponds, pond, &conn) != 0) {
        *code = "no_pond";
        return false;
    }
    Call call;
    call.expect_fp = conn.tls_fp;
    pond_call(ctx, &conn.address, conn.id, method, target, body, max_body, total_s, &call, r);
    if (r->result == PUDDLE_NET_OK)
        return true;
    *code = r->result == PUDDLE_NET_OFFLINE ? "unreachable" : r->result == PUDDLE_NET_CERT_CHANGED ? "certificate_changed" : "failed";
    return false;
}

/* Make sure the Pond has file `ref` whole: begin it there with its chunk hashes, and send each chunk it is missing. */
static bool upload_file(puddle_ctx *ctx, const char *pond, const puddle_file_ref &ref, const char **code)
{
    int64_t size;
    unsigned char *hashes;
    uint32_t count;
    if (!ctx->files || puddle_files_state(ctx->files, ref.id, nullptr, nullptr) != PUDDLE_FILE_WHOLE ||
        puddle_files_chunk_list(ctx->files, ref.id, &size, &hashes, &count) != 0) {
        *code = "no_file";
        return false;
    }
    int64_t max = puddle_ponds_files_max(ctx->ponds, pond);
    if (max <= 0 || size > max) {
        free(hashes);
        *code = max <= 0 ? "files_off" : "too_large";
        return false;
    }
    std::string b64(((size_t)count * 32 + 2) / 3 * 4 + 4, '\0');
    b64.resize(puddle_base64_encode(hashes, (size_t)count * 32, b64.data(), b64.size()));
    free(hashes);
    std::string F = std::string("/pond/v1/files/") + ref.id;
    puddle_net_reply r;
    if (!owner_call(ctx, pond, "POST", F, "{\"size\":" + std::to_string(size) + ",\"chunks\":\"" + b64 + "\"}", 1u << 20, &r, code))
        return puddle_net_reply_free(&r), false;
    std::vector<int64_t> missing;
    puddle_span all = {r.body, r.body_len}, arr, item;
    size_t cur = 0;
    bool ok = r.status == 200 && r.body && puddle_json_member(all.p, all.len, "missing", &arr) == 0;
    while (ok && puddle_json_array_next(arr, &cur, &item) == 1) {
        int64_t n;
        if (puddle_json_int64(item, &n) == 0 && n >= 0 && n < count)
            missing.push_back(n);
    }
    if (!ok)
        *code = refused(&r) ? "removed" : pond_error(&r, "failed");
    puddle_net_reply_free(&r);
    for (size_t i = 0; ok && i < missing.size(); i++) {
        unsigned char *data;
        size_t len;
        if (puddle_files_read_chunk(ctx->files, ref.id, (uint32_t)missing[i], &data, &len) != 0) {
            *code = "no_file";
            return false;
        }
        std::string bytes((const char *)data, len);
        free(data);
        for (int attempt = 0; attempt < 5; attempt++) {
            if (!owner_call(ctx, pond, "POST", F + "/chunks/" + std::to_string(missing[i]), bytes, 4096, &r, code, 120))
                return puddle_net_reply_free(&r), false;
            int status = r.status;
            if (status != 204 && status != 429)
                *code = refused(&r) ? "removed" : pond_error(&r, "failed");
            puddle_net_reply_free(&r);
            if (status == 204)
                break;
            if (status != 429 || attempt == 4)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(500)); /* the Pond asks us to slow down */
        }
    }
    return ok;
}

int puddle_pond_send(puddle_ctx *ctx, const char *pond, int64_t channel, const char *text, const char *files, size_t files_len,
                     const char **code)
{
    if (!ctx->ponds || !puddle_ponds_has_channel(ctx->ponds, pond, channel)) {
        *code = ctx->ponds && puddle_ponds_conn(ctx->ponds, pond, NULL) == 0 ? "no_channel" : "no_pond";
        return -1;
    }
    puddle_file_ref *refs = nullptr;
    size_t nrefs = 0;
    if (files && files_len && puddle_file_refs_parse(files, files_len, &refs, &nrefs) != 0) {
        *code = "no_file";
        return -1;
    }
    for (size_t i = 0; i < nrefs; i++)
        if (!upload_file(ctx, pond, refs[i], code)) {
            free(refs);
            return -1;
        }
    unsigned char rnd[12];
    char nonce[25];
    RAND_bytes(rnd, sizeof rnd);
    puddle_hex_encode(rnd, sizeof rnd, nonce);
    size_t tl = strlen(text);
    std::string body(tl * 6 + nrefs * 4096 + 128, '\0');
    puddle_jw w;
    puddle_jw_init(&w, body.data(), body.size());
    puddle_jw_object(&w);
    puddle_jw_str(&w, "text", text);
    puddle_jw_str(&w, "nonce", nonce);
    if (nrefs) {
        puddle_jw_key(&w, "files");
        puddle_file_refs_write(&w, refs, nrefs);
    }
    puddle_jw_end_object(&w);
    free(refs);
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
    if (rc == 0 && nrefs)
        if (Follower *f = nullptr; ctx->pond_followers) {
            std::lock_guard<std::mutex> g(ctx->pond_followers->mu);
            auto it = ctx->pond_followers->all.find(pond);
            if (it != ctx->pond_followers->all.end() && (f = it->second.get()))
                poke_fetcher(f); /* hold what we sent for this message, as for any other */
        }
    return rc;
}

int puddle_pond_history(puddle_ctx *ctx, const char *pond, int64_t channel, int64_t before, int limit, char **page, size_t *len,
                        const char **code)
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
    if (r.status != 200 || !r.body)
        *code = refused(&r) ? "removed" : pond_error(&r, "failed");
    else if (!(*page = (char *)malloc(r.body_len + 1)))
        *code = "failed";
    else {
        memcpy(*page, r.body, r.body_len);
        (*page)[r.body_len] = '\0';
        *len = r.body_len;
        puddle_ponds_history(ctx->ponds, pond, channel, before, r.body, r.body_len, limit); /* kept, if it joins onto what is */
        rc = 0;
    }
    puddle_net_reply_free(&r);
    return rc;
}

int puddle_pond_fetch(puddle_ctx *ctx, const char *pond, int64_t channel, int64_t message, const char *file, const char **code)
{
    if (!ctx->ponds || !puddle_ponds_has_channel(ctx->ponds, pond, channel)) {
        *code = ctx->ponds && puddle_ponds_conn(ctx->ponds, pond, NULL) == 0 ? "no_channel" : "no_pond";
        return -1;
    }
    if (!puddle_file_id_valid(file) || !ctx->pond_followers) {
        *code = "failed";
        return -1;
    }
    std::lock_guard<std::mutex> g(ctx->pond_followers->mu);
    auto it = ctx->pond_followers->all.find(pond);
    if (it == ctx->pond_followers->all.end()) {
        *code = "removed"; /* not followed: it no longer has us */
        return -1;
    }
    Follower *f = it->second.get();
    {
        std::lock_guard<std::mutex> fg(f->mu);
        f->asked.push_back(std::string(file) + " " + std::to_string(channel) + " " + std::to_string(message));
    }
    poke_fetcher(f);
    return 0;
}

int puddle_pond_leave(puddle_ctx *ctx, const char *pond, const char **code)
{
    if (!ctx->ponds || puddle_ponds_conn(ctx->ponds, pond, NULL) != 0) {
        *code = "no_pond";
        return -1;
    }
    stop_following(ctx, pond);
    if (ctx->files && ctx->config)
        for (auto &[k, h] : read_held(ctx, pond))
            let_go(ctx, pond, h);
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
