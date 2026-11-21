#include "service/levee.h"

#include "base/encoding.h"
#include "base/json.h"
#include "base/log.h"
#include "base/seal.h"
#include "core/devices.h"
#include "core/identity.h"
#include "core/levee.h"
#include "core/streams.h"
#include "federation/keysync.h"
#include "http/client.h"
#include "http/http.h"
#include "platform/fsutil.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <thread>

namespace {

struct Worker {
    puddle_ctx *ctx = nullptr;
    const puddle_router *router = nullptr;
    std::thread thread;
    std::mutex mu;
    std::condition_variable cv;
    bool stop = false, kick = false;
    std::atomic<bool> attached{ false };
    puddle_sock sock = PUDDLE_SOCK_NONE; /* the request waiting on the Levee now, cut to stop */
    std::string peers_sent;              /* what the Levee was last told */
};

constexpr size_t INBOX_MAX = 16u << 20;
constexpr size_t RESPONSE_MAX = 4u << 20;

void on_socket(void *user, puddle_sock s)
{
    Worker *w = (Worker *)user;
    std::lock_guard<std::mutex> g(w->mu);
    w->sock = s;
}

/* One request of ours to our Levee: signed for it in X-Levee-Signature, sealed to it when `sealed`, never through itself. */
void levee_call(Worker *w, const puddle_levee_state &lv, const char *method, const std::string &target, const std::string &body, bool sealed,
                const std::string &extra, const char *content_type, int total_s, size_t max_body, puddle_net_reply *r)
{
    puddle_net_opts o;
    memset(&o, 0, sizeof o);
    o.direct = 1;
    o.sig_header = "X-Levee-Signature";
    o.extra_headers = extra.c_str();
    o.content_type = content_type;
    o.total_timeout_s = total_s;
    o.io_timeout_s = total_s;
    o.connect_timeout_s = 10;
    o.max_body = max_body;
    o.seal_pub = sealed ? lv.seal_key : nullptr;
    if (w) {
        o.on_socket = on_socket;
        o.user = w;
    }
    puddle_net_request(&lv.address, method, target.c_str(), body.empty() ? nullptr : body.data(), body.size(), w ? w->ctx->identity : nullptr,
                       lv.id, &o, r);
}

std::string json_str(const char *json, size_t len, const char *key)
{
    puddle_span v;
    static thread_local char buf[1 << 16];
    if (puddle_json_member(json, len, key, &v) != 0 || puddle_json_string(v, buf, sizeof buf) != 0)
        return "";
    return buf;
}

/* Attach: with the join link's token the first time, without it after. Fills `st` from the answer. 0, or -1 with `err`. */
int attach(puddle_ctx *ctx, Worker *w, const puddle_address &addr, const char *levee_id, const unsigned char seal_key[32], const char *token,
           puddle_levee_state *st, char *err, size_t errlen)
{
    size_t cl = 0;
    char *chain = puddle_identity_chain(ctx->identity, &cl);
    if (!chain) {
        snprintf(err, errlen, "cannot read our key chain");
        return -1;
    }
    std::string body(16 + cl * 2 + 64, '\0');
    puddle_jw jw;
    puddle_jw_init(&jw, body.data(), body.size());
    puddle_jw_object(&jw);
    puddle_jw_key(&jw, "chain");
    puddle_jw_string_n(&jw, chain, cl);
    if (token && *token)
        puddle_jw_str(&jw, "token", token);
    puddle_jw_end_object(&jw);
    size_t bl = puddle_jw_finish(&jw);
    free(chain);
    body.resize(bl);

    puddle_levee_state tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.address = addr;
    snprintf(tmp.id, sizeof tmp.id, "%s", levee_id);
    memcpy(tmp.seal_key, seal_key, 32);
    puddle_net_reply r;
    puddle_net_opts o;
    memset(&o, 0, sizeof o);
    (void)o;
    if (w) {
        levee_call(w, tmp, "POST", "/levee/v1/attach", body, true, "", nullptr, 20, 0, &r);
    } else {
        Worker none;
        none.ctx = ctx;
        levee_call(&none, tmp, "POST", "/levee/v1/attach", body, true, "", nullptr, 20, 0, &r);
    }
    int rc = -1;
    if (r.result != PUDDLE_NET_OK) {
        snprintf(err, errlen, "%s", r.reason[0] ? r.reason : "cannot reach the Levee");
    } else if (r.status == 404) {
        snprintf(err, errlen, "the Levee didn't take us: the link was used already, or another Puddle is joined there");
    } else if (r.status != 200 || !r.opened || !r.body) {
        snprintf(err, errlen, "the Levee answered oddly (HTTP %d)", r.status);
    } else {
        const char *j = r.body;
        size_t n = r.body_len;
        puddle_span lvv, tv, v;
        unsigned char key[40];
        size_t kl = 0;
        char want[PP_ID_LEN + 1];
        if (!puddle_json_valid_object(j, n) || puddle_json_member(j, n, "levee", &lvv) != 0 ||
            json_str(lvv.p, lvv.len, "id") != levee_id || puddle_base64_decode(json_str(lvv.p, lvv.len, "key").c_str(), key, sizeof key, &kl) != 0 ||
            kl != 32) {
            snprintf(err, errlen, "the Levee's answer is malformed");
        } else {
            pp_key_id_b32(PP_LABEL_LEVEE, key, want);
            if (strcmp(want, levee_id) != 0) {
                snprintf(err, errlen, "the Levee's key isn't the one its link names");
            } else {
                *st = tmp;
                st->joined = 1;
                memcpy(st->key, key, 32);
                puddle_address there;
                std::string a = json_str(j, n, "address");
                if (!a.empty() && puddle_address_parse(a.c_str(), &there) == 0)
                    st->address = there; /* where others reach it: what it says, which may differ from how the link spelled it */
                if (puddle_json_member(j, n, "turn", &tv) == 0) {
                    int64_t port = 0;
                    if (puddle_json_member(tv.p, tv.len, "port", &v) == 0 && puddle_json_int64(v, &port) == 0 && port > 0 && port < 65536)
                        st->turn_port = (unsigned)port;
                    snprintf(st->turn_user, sizeof st->turn_user, "%s", json_str(tv.p, tv.len, "username").c_str());
                    snprintf(st->turn_pass, sizeof st->turn_pass, "%s", json_str(tv.p, tv.len, "password").c_str());
                }
                rc = 0;
            }
        }
    }
    puddle_net_reply_free(&r);
    return rc;
}

/* Who may reach us, as the Levee is told: everyone with a stream we'd take requests from, and our phones. */
std::string peers_json(puddle_ctx *ctx)
{
    std::string out = "{\"peers\":[";
    puddle_stream *sl;
    size_t sn;
    bool first = true;
    if (ctx->streams && puddle_streams_list((puddle_streams *)ctx->streams, &sl, &sn) == 0) {
        for (size_t i = 0; i < sn; i++) {
            if (!sl[i].chain || sl[i].provisional || sl[i].state == PUDDLE_STREAM_CLOSED ||
                puddle_streams_is_blocked((puddle_streams *)ctx->streams, sl[i].id))
                continue;
            size_t cap = sl[i].chain_len * 2 + 200;
            std::string one(cap, '\0');
            puddle_jw w;
            puddle_jw_init(&w, one.data(), one.size());
            puddle_jw_object(&w);
            puddle_jw_str(&w, "id", sl[i].id);
            puddle_jw_key(&w, "chain");
            puddle_jw_string_n(&w, sl[i].chain, sl[i].chain_len);
            puddle_jw_end_object(&w);
            size_t n = puddle_jw_finish(&w);
            if (!n)
                continue;
            out += first ? "" : ",";
            out.append(one.data(), n);
            first = false;
        }
        puddle_stream_list_free(sl, sn);
    }
    out += "],\"devices\":[";
    puddle_device *dl;
    size_t dn;
    first = true;
    if (ctx->devices && puddle_devices_list((puddle_devices *)ctx->devices, &dl, &dn) == 0) {
        for (size_t i = 0; i < dn; i++) {
            char key[64];
            puddle_base64_encode(dl[i].pub, 32, key, sizeof key);
            out += std::string(first ? "" : ",") + "{\"id\":\"" + dl[i].id + "\",\"key\":\"" + key + "\"}";
            first = false;
        }
        free(dl);
    }
    out += "]}";
    return out;
}

/* A file answer, read (and sealed, when it is) into memory: what the server would have sent after the head. */
bool collect_big(puddle_big *big, std::string &out)
{
    if (big->data) {
        out.append((const char *)big->data, big->len);
        free(big->data);
        big->data = nullptr;
        return true;
    }
    if (!big->has_file)
        return true;
    bool ok = true;
    int64_t left = big->file_len;
    if (left > (int64_t)RESPONSE_MAX * 8)
        ok = false;
    std::string piece(PUDDLE_SEAL_RECORD, '\0'), sealed(PUDDLE_SEAL_RECORD + PUDDLE_SEAL_TAG_LEN, '\0');
    for (uint64_t i = big->first_record; ok && left > 0; i++) {
        size_t want = left < (int64_t)PUDDLE_SEAL_RECORD ? (size_t)left : PUDDLE_SEAL_RECORD, got = 0;
        while (got < want) {
            int64_t n = puddle_fd_read(big->fd, piece.data() + got, want - got);
            if (n <= 0)
                break;
            got += (size_t)n;
        }
        if (got != want) {
            ok = false;
            break;
        }
        if (big->sealed) {
            if (puddle_seal_record(big->seal_key, i, big->seal_status, i == big->final_record, piece.data(), want, (unsigned char *)sealed.data()) != 0) {
                ok = false;
                break;
            }
            out.append(sealed.data(), want + PUDDLE_SEAL_TAG_LEN);
        } else {
            out.append(piece.data(), want);
        }
        left -= (int64_t)want;
        if (big->sealed && i == big->final_record)
            break;
    }
    puddle_fd_close(big->fd);
    big->has_file = 0;
    return ok;
}

std::string header_of(const char *head, size_t hl, const char *name)
{
    size_t nl = strlen(name);
    for (const char *l = (const char *)memchr(head, '\n', hl); l && l < head + hl; l = (const char *)memchr(l + 1, '\n', head + hl - l - 1)) {
        if ((size_t)(head + hl - (l + 1)) > nl && !strncasecmp(l + 1, name, nl)) {
            const char *v = l + 1 + nl;
            while (*v == ' ')
                v++;
            const char *e = v;
            while (e < head + hl && *e != '\r' && *e != '\n')
                e++;
            return std::string(v, (size_t)(e - v));
        }
    }
    return "";
}

/* One item from the inbox: run it as if it had come here, and for a live one post the answer back. */
void handle_item(Worker *w, const puddle_levee_state &lv, const char *j, size_t n)
{
    puddle_span hv, v;
    int64_t num = 0, received = 0;
    if (puddle_json_member(j, n, "n", &v) != 0 || puddle_json_int64(v, &num) != 0)
        return;
    if (puddle_json_member(j, n, "file_done", &hv) == 0) {
        puddle_log(PUDDLE_LOG_INFO, "levee: %.12s… has fetched a file we left there", json_str(hv.p, hv.len, "by").c_str());
        return;
    }
    bool live = puddle_json_member(j, n, "live", &v) == 0 && v.len == 4 && !memcmp(v.p, "true", 4);
    if (puddle_json_member(j, n, "received", &v) == 0)
        puddle_json_int64(v, &received);
    puddle_request req;
    memset(&req, 0, sizeof req);
    std::string method = json_str(j, n, "method"), target = json_str(j, n, "target");
    if ((method != "GET" && method != "POST") || target.empty() || target[0] != '/' || target.size() >= sizeof req.target)
        return;
    snprintf(req.method, sizeof req.method, "%s", method.c_str());
    snprintf(req.target, sizeof req.target, "%s", target.c_str());
    snprintf(req.path, sizeof req.path, "%s", target.substr(0, target.find('?')).c_str());
    if (puddle_json_member(j, n, "headers", &hv) == 0) {
        snprintf(req.signature, sizeof req.signature, "%s", json_str(hv.p, hv.len, "X-Puddle-Signature").c_str());
        snprintf(req.seal, sizeof req.seal, "%s", json_str(hv.p, hv.len, "X-Puddle-Seal").c_str());
        snprintf(req.range, sizeof req.range, "%s", json_str(hv.p, hv.len, "Range").c_str());
        snprintf(req.content_type, sizeof req.content_type, "%s", json_str(hv.p, hv.len, "Content-Type").c_str());
    }
    req.received_ms = live ? 0 : received;

    std::string body;
    std::string b64 = json_str(j, n, "body");
    if (!b64.empty()) {
        body.assign(b64.size() / 4 * 3 + 3, '\0');
        size_t bl = 0;
        if (puddle_base64_decode(b64.c_str(), (unsigned char *)body.data(), body.size(), &bl) != 0)
            return;
        body.resize(bl);
    } else if (puddle_json_member(j, n, "body_size", &v) == 0) {
        puddle_net_reply r;
        levee_call(w, lv, "GET", "/levee/v1/inbox/" + std::to_string(num) + "/body", "", false, "", nullptr, 60, INBOX_MAX * 4, &r);
        if (r.result != PUDDLE_NET_OK || r.status != 200 || !r.body) {
            puddle_net_reply_free(&r);
            return;
        }
        body.assign(r.body, r.body_len);
        puddle_net_reply_free(&r);
    }
    req.content_length = body.size();

    std::string out(RESPONSE_MAX, '\0');
    int status = 0;
    puddle_big big;
    memset(&big, 0, sizeof big);
    big.fd = -1;
    size_t len = puddle_router_handle_big(w->router, &req, 0, (const unsigned char *)body.data(), body.size(), out.data(), out.size(), &status, &big);
    if (!live) { /* answered already, by the Levee's custody receipt */
        std::string drop;
        collect_big(&big, drop);
        return;
    }
    const char *e = len ? (const char *)memmem(out.data(), len, "\r\n\r\n", 4) : nullptr;
    if (!e) {
        std::string drop;
        collect_big(&big, drop);
        return;
    }
    size_t hl = (size_t)(e - out.data()) + 4;
    std::string answer(out.data() + hl, len - hl);
    if (!collect_big(&big, answer))
        return;
    std::string extra = "X-Levee-Status: " + std::to_string(status) + "\r\n";
    std::string receipt = header_of(out.data(), hl, "X-Puddle-Receipt:"), range = header_of(out.data(), hl, "Content-Range:"),
                type = header_of(out.data(), hl, "Content-Type:");
    if (!receipt.empty())
        extra += "X-Puddle-Receipt: " + receipt + "\r\n";
    if (!range.empty())
        extra += "Content-Range: " + range + "\r\n";
    puddle_net_reply r;
    levee_call(w, lv, "POST", "/levee/v1/answer/" + std::to_string(num), answer, false, extra,
               type.empty() ? "application/octet-stream" : type.c_str(), 60, 1024, &r);
    puddle_net_reply_free(&r);
}

void wait_for(Worker *w, int ms)
{
    std::unique_lock<std::mutex> lk(w->mu);
    w->cv.wait_for(lk, std::chrono::milliseconds(ms), [w] { return w->stop || w->kick; });
    w->kick = false;
}

void run(Worker *w)
{
    int backoff = 1000;
    bool told_failed = false;
    while (true) {
        {
            std::lock_guard<std::mutex> g(w->mu);
            if (w->stop)
                return;
        }
        puddle_levee_state lv;
        if (!puddle_levee_current(&lv)) {
            w->attached = false;
            w->peers_sent.clear();
            wait_for(w, 5000);
            continue;
        }
        if (!w->attached) {
            char err[300];
            puddle_levee_state fresh;
            if (attach(w->ctx, w, lv.address, lv.id, lv.seal_key, "", &fresh, err, sizeof err) == 0) {
                fresh.after = lv.after;
                fresh.joined_ms = lv.joined_ms;
                fresh.address = lv.address;
                puddle_levee_state now;
                if (puddle_levee_current(&now) && !strcmp(now.id, lv.id))
                    puddle_levee_save(w->ctx->config->data_dir, &fresh);
                w->attached = true;
                w->peers_sent.clear();
                backoff = 1000;
                told_failed = false;
                puddle_log(PUDDLE_LOG_INFO, "levee: attached to %s", lv.address.address);
            } else {
                if (!told_failed)
                    puddle_log(PUDDLE_LOG_WARN, "levee: cannot attach to %s: %s", lv.address.address, err);
                told_failed = true;
                wait_for(w, backoff);
                backoff = backoff < 60000 ? backoff * 2 : 60000;
                continue;
            }
        }
        std::string peers = peers_json(w->ctx);
        if (peers != w->peers_sent) {
            puddle_net_reply r;
            levee_call(w, lv, "POST", "/levee/v1/peers", peers, true, "", nullptr, 20, 0, &r);
            if (r.result == PUDDLE_NET_OK && r.status == 204)
                w->peers_sent = peers;
            else if (r.result == PUDDLE_NET_OK && r.status == 404)
                w->attached = false;
            puddle_net_reply_free(&r);
        }
        puddle_net_reply r;
        levee_call(w, lv, "GET", "/levee/v1/inbox?after=" + std::to_string(lv.after), "", false, "", nullptr, 40, INBOX_MAX, &r);
        {
            std::lock_guard<std::mutex> g(w->mu);
            if (w->stop) {
                puddle_net_reply_free(&r);
                return;
            }
        }
        if (r.result != PUDDLE_NET_OK || r.status != 200 || !r.body) {
            if (r.result == PUDDLE_NET_OK && r.status == 404)
                w->attached = false; /* it doesn't know us (any more): attach again */
            puddle_net_reply_free(&r);
            wait_for(w, backoff);
            backoff = backoff < 30000 ? backoff * 2 : 30000;
            continue;
        }
        backoff = 1000;
        puddle_span items, it;
        size_t cur = 0;
        uint64_t last = lv.after;
        if (puddle_json_valid_object(r.body, r.body_len) && puddle_json_member(r.body, r.body_len, "items", &items) == 0) {
            while (puddle_json_array_next(items, &cur, &it) == 1) {
                puddle_span v;
                int64_t num = 0;
                if (puddle_json_member(it.p, it.len, "n", &v) == 0 && puddle_json_int64(v, &num) == 0 && num > 0 && (uint64_t)num > last)
                    last = (uint64_t)num;
                handle_item(w, lv, it.p, it.len);
            }
        }
        puddle_net_reply_free(&r);
        if (last != lv.after) {
            puddle_levee_state now;
            if (puddle_levee_current(&now) && !strcmp(now.id, lv.id)) {
                now.after = last;
                puddle_levee_save(w->ctx->config->data_dir, &now);
            }
        }
    }
}

int join(puddle_ctx *ctx, const char *link, char *err, size_t errlen)
{
    puddle_levee_link l;
    if (puddle_levee_link_parse(link, &l) != 0) {
        snprintf(err, errlen, "that isn't a Levee's join link (levee://…)");
        return -1;
    }
    puddle_levee_state st;
    if (attach(ctx, nullptr, l.address, l.id, l.seal_key, l.token, &st, err, errlen) != 0)
        return -1;
    st.after = 0;
    st.joined_ms = puddle_ctx_now_ms(ctx);
    if (puddle_levee_save(ctx->config->data_dir, &st) != 0) {
        snprintf(err, errlen, "cannot save the Levee");
        return -1;
    }
    puddle_log(PUDDLE_LOG_INFO, "levee: joined %s; our address is its now", st.address.address);
    Worker *w = (Worker *)ctx->levee_worker;
    if (w) {
        std::lock_guard<std::mutex> g(w->mu);
        w->attached = false; /* attach again from the thread, which then holds the inbox */
        w->kick = true;
        w->cv.notify_all();
    }
    puddle_sync_resume(ctx); /* friends are told our new address, and which Levee answers for us */
    return 0;
}

int leave(puddle_ctx *ctx, char *err, size_t errlen)
{
    (void)err;
    (void)errlen;
    puddle_levee_state lv;
    if (!puddle_levee_current(&lv))
        return 0;
    Worker *w = (Worker *)ctx->levee_worker;
    puddle_net_reply r;
    Worker none;
    none.ctx = ctx;
    levee_call(w ? w : &none, lv, "POST", "/levee/v1/detach", "", false, "", nullptr, 15, 0, &r);
    puddle_net_reply_free(&r);
    puddle_levee_state off;
    memset(&off, 0, sizeof off);
    puddle_levee_save(ctx->config->data_dir, &off);
    if (w) {
        std::lock_guard<std::mutex> g(w->mu);
        w->attached = false;
        if (w->sock != PUDDLE_SOCK_NONE)
            puddle_sock_cut(w->sock);
        w->kick = true;
        w->cv.notify_all();
    }
    puddle_log(PUDDLE_LOG_INFO, "levee: left %s", lv.address.address);
    puddle_sync_resume(ctx); /* friends are told where we are without it */
    return 0;
}

int attached(puddle_ctx *ctx)
{
    Worker *w = (Worker *)ctx->levee_worker;
    return w && w->attached;
}

} // namespace

void puddle_levee_worker_start(puddle_ctx *ctx, const puddle_router *router)
{
    if (!ctx->config || !ctx->identity)
        return;
    Worker *w = new Worker;
    w->ctx = ctx;
    w->router = router;
    ctx->levee_worker = w;
    ctx->levee_join = join;
    ctx->levee_leave = leave;
    ctx->levee_attached = attached;
    w->thread = std::thread(run, w);
}

void puddle_levee_worker_stop(puddle_ctx *ctx)
{
    Worker *w = (Worker *)ctx->levee_worker;
    if (!w)
        return;
    {
        std::lock_guard<std::mutex> g(w->mu);
        w->stop = true;
        if (w->sock != PUDDLE_SOCK_NONE)
            puddle_sock_cut(w->sock);
        w->cv.notify_all();
    }
    if (w->thread.joinable())
        w->thread.join();
    ctx->levee_worker = nullptr;
    ctx->levee_join = nullptr;
    ctx->levee_leave = nullptr;
    ctx->levee_attached = nullptr;
    delete w;
}
