/* Calls between friends (docs/calls.md). See federation/calls.h. */
#include "federation/calls.h"

#include "base/encoding.h"
#include "base/log.h"
#include "core/link.h"
#include "core/streams.h"
#include "platform/fsutil.h"
#include "federation/sender.h"

#include <stdio.h>
#include <string.h>

#ifndef PUDDLE_HAVE_CALLS

/* Built without libdatachannel: no calls, and everyone is told so. */

int puddle_calls_built(void) { return 0; }

puddle_calls *puddle_calls_start(puddle_ctx *, const puddle_outbox_hooks *, char *err, size_t errlen)
{
    if (err && errlen)
        snprintf(err, errlen, "this Puddle was built without calls (libdatachannel)");
    return NULL;
}
void puddle_calls_stop(puddle_calls *) {}
int puddle_call_start(puddle_ctx *, const char *, char *, const char **code) { return *code = "no_calls", -1; }
int puddle_call_accept(puddle_ctx *, const char *, const char **code) { return *code = "no_calls", -1; }
int puddle_call_decline(puddle_ctx *, const char *, const char **code) { return *code = "no_calls", -1; }
int puddle_call_end(puddle_ctx *, const char *, const char **code) { return *code = "no_calls", -1; }
int puddle_calls_write(puddle_ctx *, const char *id, const char *, int, puddle_jw *w)
{
    if (id)
        return 1;
    puddle_jw_object(w);
    puddle_jw_num(w, "version", 0);
    puddle_jw_key(w, "items");
    puddle_jw_array(w);
    puddle_jw_end_array(w);
    puddle_jw_end_object(w);
    return 0;
}
void puddle_calls_on_audio(puddle_calls *, puddle_call_frame_fn, void *) {}
void puddle_calls_set_timeouts(puddle_calls *, int64_t, int64_t, int64_t) {}
int puddle_call_send_audio(puddle_calls *, const char *, const unsigned char *, size_t) { return -1; }
uint64_t puddle_calls_version(puddle_calls *) { return 0; }

static int no_calls(const puddle_call *, puddle_jw *w) { return puddle_fail(w, 409, "no_calls"); }

const puddle_route puddle_call_routes[] = {
    { "POST", "/stream/v1/call/offer",  PUDDLE_AUTH_PEER, PUDDLE_CALL_SDP_MAX + 1024, no_calls, NULL },
    { "POST", "/stream/v1/call/answer", PUDDLE_AUTH_PEER, PUDDLE_CALL_SDP_MAX + 1024, no_calls, NULL },
    { "POST", "/stream/v1/call/end",    PUDDLE_AUTH_PEER, 1024, no_calls, NULL },
};
const size_t puddle_call_routes_count = sizeof puddle_call_routes / sizeof puddle_call_routes[0];

#else

#include <rtc/rtc.hpp>

#include <openssl/rand.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#define OPUS_PT 111
#define MID "audio"
#define LOST_MS 10000 /* a connection that dropped and hasn't come back in this long is over */

namespace {

struct Call {
    std::string id, peer;
    bool outgoing = false;
    std::string state, reason; /* calling, ringing, connecting, active, ended (docs/calls.md, "A call's life") */
    int64_t created = 0, answered = 0, ended = 0, deadline = 0, lost_since = 0;
    std::string remote_sdp;
    int remote_udp = 0;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> track;
    std::shared_ptr<rtc::RtpPacketizationConfig> rtp;
    bool open = false;      /* the track: media can go */
    uint32_t next_ts = 0;   /* the RTP timestamp of our next frame */
};

using CallPtr = std::shared_ptr<Call>;

/* An ended call, as the owner's history keeps it (<data_dir>/state/calls.jsonl, one per line, oldest first). */
struct Record {
    std::string id, peer, reason;
    bool outgoing = false;
    int64_t created = 0, answered = 0, ended = 0;
};

} // namespace

struct puddle_calls {
    puddle_ctx *ctx;
    puddle_outbox_hooks hooks; /* the outbox's, with shorter timeouts: a call is now or never */
    uint16_t port;
    std::string bind;
    std::mutex mu;
    CallPtr current;                 /* the call going on, if any */
    std::deque<CallPtr> ended;       /* newest first: their connections, until the worker lets them go */
    std::deque<Record> history;      /* every ended call kept, oldest first (PUDDLE_CALL_HISTORY) */
    std::string history_path, state_dir;
    std::deque<std::function<void()>> tasks; /* for the worker: telling the friend a call ended */
    std::condition_variable wake;
    bool stopping = false;
    std::thread worker;
    puddle_call_frame_fn on_audio = nullptr;
    void *on_audio_user = nullptr;
    std::atomic<uint64_t> version{1};
    int64_t ring_ms = PUDDLE_CALL_RING_MS, ring_in_ms = PUDDLE_CALL_RING_IN_MS, connect_ms = PUDDLE_CALL_CONNECT_MS;
};

int puddle_calls_built(void) { return 1; }

static int64_t now_of(puddle_calls *cs) { return puddle_ctx_now_ms(cs->ctx); }

static std::string new_call_id()
{
    unsigned char raw[16];
    char out[PUDDLE_CALL_ID_LEN + 8];
    if (RAND_bytes(raw, sizeof raw) != 1)
        return "";
    puddle_base32_encode(raw, sizeof raw, out, sizeof out);
    return std::string(out, PUDDLE_CALL_ID_LEN);
}

static uint32_t random32()
{
    uint32_t v = 0;
    RAND_bytes((unsigned char *)&v, sizeof v);
    return v;
}

static bool call_id_ok(const char *s)
{
    if (strlen(s) != PUDDLE_CALL_ID_LEN)
        return false;
    for (const char *p = s; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= '2' && *p <= '7')))
            return false;
    return true;
}

static bool active_friend(puddle_ctx *ctx, const char *peer)
{
    puddle_stream st;
    if (!ctx->streams || puddle_streams_get(ctx->streams, peer, &st) != 0)
        return false;
    bool ok = st.state == PUDDLE_STREAM_ACTIVE;
    puddle_stream_free(&st);
    return ok;
}

/* ---- telling the friend ---- */

/* A signed request to the friend's Puddle, checked by its receipt (federation/sender.h). true if they answered in their name. */
static bool tell(puddle_calls *cs, const std::string &peer, const char *path, std::string body, int *status = nullptr,
                 std::string *answer = nullptr)
{
    puddle_ob_item it;
    memset(&it, 0, sizeof it);
    snprintf(it.peer, sizeof it.peer, "%s", peer.c_str());
    snprintf(it.kind, sizeof it.kind, "call");
    snprintf(it.method, sizeof it.method, "POST");
    snprintf(it.path, sizeof it.path, "%s", path);
    it.body = body.data();
    it.body_len = body.size();
    puddle_net_reply r;
    puddle_federation_send(&cs->hooks, &it, &r);
    bool ok = r.result == PUDDLE_NET_OK;
    if (status)
        *status = ok ? r.status : 0;
    if (answer && ok && r.body)
        answer->assign(r.body, r.body_len);
    if (!ok)
        puddle_log(PUDDLE_LOG_INFO, "calls: %s to %.12s… didn't get through: %s", path, peer.c_str(), r.reason);
    puddle_net_reply_free(&r);
    return ok;
}

static std::string body_of(const std::string &id, const char *key, const std::string &value, int udp = 0)
{
    std::string buf(value.size() * 2 + 256, '\0');
    puddle_jw w;
    puddle_jw_init(&w, buf.data(), buf.size());
    puddle_jw_object(&w);
    puddle_jw_str(&w, "call", id.c_str());
    puddle_jw_str(&w, key, value.c_str());
    if (udp)
        puddle_jw_num(&w, "udp", udp);
    puddle_jw_end_object(&w);
    size_t n = puddle_jw_finish(&w);
    return std::string(buf.data(), n);
}

static std::string error_of(const std::string &json)
{
    puddle_span v;
    char e[40] = "";
    if (puddle_json_member(json.data(), json.size(), "error", &v) == 0)
        puddle_json_string(v, e, sizeof e);
    return e;
}

/* ---- the history ---- */

static std::string record_line(const Record &r)
{
    char buf[512];
    puddle_jw w;
    puddle_jw_init(&w, buf, sizeof buf);
    puddle_jw_object(&w);
    puddle_jw_str(&w, "id", r.id.c_str());
    puddle_jw_str(&w, "user", r.peer.c_str());
    puddle_jw_str(&w, "direction", r.outgoing ? "outgoing" : "incoming");
    puddle_jw_str(&w, "reason", r.reason.c_str());
    puddle_jw_num(&w, "created", r.created);
    puddle_jw_num(&w, "answered", r.answered);
    puddle_jw_num(&w, "ended", r.ended);
    puddle_jw_end_object(&w);
    size_t n = puddle_jw_finish(&w);
    return n ? std::string(buf, n) + "\n" : "";
}

static bool record_parse(const char *line, size_t len, Record &r)
{
    puddle_span v;
    char s[64];
    int64_t n;
    auto str = [&](const char *key, std::string &out, size_t max) {
        if (puddle_json_member(line, len, key, &v) != 0 || puddle_json_string(v, s, sizeof s) != 0 || strlen(s) > max)
            return false;
        out = s;
        return true;
    };
    auto num = [&](const char *key, int64_t &out) {
        return puddle_json_member(line, len, key, &v) == 0 && puddle_json_int64(v, &n) == 0 && (out = n, true);
    };
    std::string dir;
    if (!puddle_json_valid_object(line, len) || !str("id", r.id, PUDDLE_CALL_ID_LEN) || !str("user", r.peer, PUDDLE_USER_ID_LEN) ||
        !str("direction", dir, 8) || !str("reason", r.reason, 16) || !num("created", r.created) || !num("answered", r.answered) ||
        !num("ended", r.ended) || !puddle_id_valid(r.peer.c_str()))
        return false;
    r.outgoing = dir == "outgoing";
    return true;
}

/* Read what was kept; written again, shorter, when it has grown to twice what is kept. */
static void load_history(puddle_calls *cs)
{
    FILE *f = fopen(cs->history_path.c_str(), "rb");
    if (!f)
        return;
    char line[1024];
    size_t lines = 0;
    while (fgets(line, sizeof line, f)) {
        size_t len = strcspn(line, "\r\n");
        Record r;
        lines++;
        if (record_parse(line, len, r)) {
            cs->history.push_back(std::move(r));
            if (cs->history.size() > PUDDLE_CALL_HISTORY)
                cs->history.pop_front();
        }
    }
    fclose(f);
    if (lines > 2 * PUDDLE_CALL_HISTORY) {
        std::string all;
        for (auto &r : cs->history)
            all += record_line(r);
        if (puddle_write_file_atomic(cs->state_dir.c_str(), cs->history_path.c_str(), all.data(), all.size()) != 0)
            puddle_log(PUDDLE_LOG_WARN, "calls: cannot shorten %s", cs->history_path.c_str());
    }
}

/* Called with the lock held. */
static void remember(puddle_calls *cs, const Call &c)
{
    Record r;
    r.id = c.id, r.peer = c.peer, r.reason = c.reason, r.outgoing = c.outgoing;
    r.created = c.created, r.answered = c.answered, r.ended = c.ended;
    cs->history.push_back(r);
    if (cs->history.size() > PUDDLE_CALL_HISTORY)
        cs->history.pop_front();
    if (cs->history_path.empty())
        return;
    std::string line = record_line(r);
    FILE *f = fopen(cs->history_path.c_str(), "ab");
    if (!f || fwrite(line.data(), 1, line.size(), f) != line.size())
        puddle_log(PUDDLE_LOG_WARN, "calls: cannot keep a note of the call in %s", cs->history_path.c_str());
    if (f)
        fclose(f);
}

/* ---- a call's end ---- */

/* End `call` with `reason` (if it hasn't ended), and tell the friend `tell_reason` unless it is NULL. */
static void finish(puddle_calls *cs, const CallPtr &call, const char *reason, const char *tell_reason)
{
    std::shared_ptr<rtc::PeerConnection> pc;
    {
        std::lock_guard<std::mutex> g(cs->mu);
        if (call->state == "ended")
            return;
        call->state = "ended";
        call->reason = reason;
        call->ended = now_of(cs);
        call->open = false;
        if (cs->current == call)
            cs->current.reset();
        cs->ended.push_front(call);
        remember(cs, *call);
        pc = call->pc;
        if (pc) /* closed by the worker: this may be running in one of the connection's own callbacks */
            cs->tasks.push_back([pc] { pc->close(); });
        if (tell_reason) {
            std::string peer = call->peer, body = body_of(call->id, "reason", tell_reason);
            cs->tasks.push_back([cs, peer, body] { tell(cs, peer, "/stream/v1/call/end", body); });
        }
        cs->version++;
        cs->wake.notify_all();
    }
    puddle_log(PUDDLE_LOG_INFO, "calls: %s with %.12s… ended: %s", call->id.c_str(), call->peer.c_str(), reason);
}

/* ---- the connection ---- */

static CallPtr find(puddle_calls *cs, const std::string &id)
{
    std::lock_guard<std::mutex> g(cs->mu);
    if (cs->current && cs->current->id == id)
        return cs->current;
    for (auto &c : cs->ended)
        if (c->id == id)
            return c;
    return nullptr;
}

/* Opus out (packetized, with sender reports) and in (frames handed to the owner's device). */
static void setup_track(puddle_calls *cs, const std::weak_ptr<Call> &weak, const std::shared_ptr<rtc::Track> &track)
{
    auto rtp = std::make_shared<rtc::RtpPacketizationConfig>(random32(), "puddle", OPUS_PT, rtc::OpusRtpPacketizer::DefaultClockRate);
    auto pack = std::make_shared<rtc::OpusRtpPacketizer>(rtp);
    pack->addToChain(std::make_shared<rtc::RtcpSrReporter>(rtp));
    pack->addToChain(std::make_shared<rtc::OpusRtpDepacketizer>());
    track->setMediaHandler(pack);
    std::string id;
    {
        std::lock_guard<std::mutex> g(cs->mu);
        CallPtr call = weak.lock();
        if (!call)
            return;
        call->track = track;
        call->rtp = rtp;
        call->next_ts = random32();
        id = call->id;
    }
    track->onOpen([cs, weak] {
        std::lock_guard<std::mutex> g(cs->mu);
        if (CallPtr call = weak.lock(); call && call->state != "ended") {
            call->open = true;
            cs->version++;
        }
    });
    track->onFrame([cs, id](rtc::binary data, rtc::FrameInfo info) {
        puddle_call_frame_fn fn;
        void *user;
        {
            std::lock_guard<std::mutex> g(cs->mu);
            fn = cs->on_audio, user = cs->on_audio_user;
        }
        if (fn && !data.empty() && data.size() <= PUDDLE_CALL_FRAME_MAX)
            fn(user, id.c_str(), (const unsigned char *)data.data(), data.size(), info.timestamp);
    });
}

static std::shared_ptr<rtc::PeerConnection> make_pc(puddle_calls *cs, const CallPtr &call)
{
    rtc::Configuration conf; /* no ICE servers: only the two Puddles' own addresses (docs/calls.md, "Finding a path") */
    conf.enableIceUdpMux = true;
    conf.portRangeBegin = conf.portRangeEnd = cs->port;
    if (!cs->bind.empty())
        conf.bindAddress = cs->bind;
    auto pc = std::make_shared<rtc::PeerConnection>(conf);
    std::weak_ptr<Call> weak = call;
    pc->onStateChange([cs, weak](rtc::PeerConnection::State s) {
        CallPtr call = weak.lock();
        if (!call)
            return;
        bool fail = false;
        {
            std::lock_guard<std::mutex> g(cs->mu);
            if (call->state == "ended")
                return;
            if (s == rtc::PeerConnection::State::Connected) {
                call->lost_since = 0;
                if (call->state == "connecting") {
                    call->state = "active";
                    call->answered = now_of(cs);
                    puddle_log(PUDDLE_LOG_INFO, "calls: %s with %.12s… connected", call->id.c_str(), call->peer.c_str());
                }
                cs->version++;
            } else if (s == rtc::PeerConnection::State::Disconnected) {
                call->lost_since = now_of(cs); /* may come back: the worker ends it if it doesn't */
            } else if (s == rtc::PeerConnection::State::Failed || s == rtc::PeerConnection::State::Closed) {
                fail = true;
            }
        }
        if (fail)
            finish(cs, call, "failed", "failed");
    });
    return pc;
}

/* Wait until our side of the description is complete (candidates and all: nothing trickles). */
static bool gathered(const std::shared_ptr<rtc::PeerConnection> &pc, const CallPtr &call, puddle_calls *cs, bool want_track)
{
    for (int i = 0; i < 500; i++) {
        bool track;
        {
            std::lock_guard<std::mutex> g(cs->mu);
            track = call->track != nullptr;
        }
        if (pc->gatheringState() == rtc::PeerConnection::GatheringState::Complete && pc->localDescription() && (track || !want_track))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

/* The friend's advertised addresses, at the UDP port they said, as candidates beside the ones their description lists. */
static void add_their_addresses(puddle_calls *cs, const std::shared_ptr<rtc::PeerConnection> &pc, const std::string &peer, int udp)
{
    puddle_address addrs[4];
    int n = cs->hooks.addresses ? cs->hooks.addresses(cs->hooks.user, peer.c_str(), addrs, 4) : 0;
    for (int i = 0; i < n; i++) {
        std::string cand = "a=candidate:" + std::to_string(i + 9) + " 1 UDP " + std::to_string(2122000000 - i) + " " + addrs[i].host + " " +
                           std::to_string(udp) + " typ host";
        try {
            pc->addRemoteCandidate(rtc::Candidate(cand, MID));
        } catch (const std::exception &e) {
            puddle_log(PUDDLE_LOG_INFO, "calls: not using %s for %.12s…: %s", addrs[i].host, peer.c_str(), e.what());
        }
    }
}

/* ---- the worker: deadlines, and telling friends a call ended ---- */

static void work(puddle_calls *cs)
{
    std::unique_lock<std::mutex> lk(cs->mu);
    while (true) {
        while (!cs->tasks.empty()) {
            auto t = std::move(cs->tasks.front());
            cs->tasks.pop_front();
            lk.unlock();
            t();
            lk.lock();
        }
        if (cs->stopping && cs->tasks.empty())
            break;
        /* only here, never in a callback, does a connection go away for good */
        std::deque<CallPtr> gone;
        while (cs->ended.size() > PUDDLE_CALL_KEEP_ENDED) {
            gone.push_back(cs->ended.back());
            cs->ended.pop_back();
        }
        CallPtr call = cs->current;
        const char *reason = nullptr, *told = nullptr;
        int64_t now = now_of(cs);
        if (call && call->deadline && now >= call->deadline) {
            if (call->state == "calling")
                reason = "no_answer", told = "no_answer";
            else if (call->state == "ringing")
                reason = "missed";
            else if (call->state == "connecting")
                reason = "failed", told = "failed";
        } else if (call && call->lost_since && now - call->lost_since >= LOST_MS) {
            reason = "failed", told = "failed";
        }
        lk.unlock();
        gone.clear();
        if (reason)
            finish(cs, call, reason, told);
        lk.lock();
        if (cs->tasks.empty() && !cs->stopping)
            cs->wake.wait_for(lk, std::chrono::milliseconds(200));
    }
}

/* ---- starting and stopping ---- */

puddle_calls *puddle_calls_start(puddle_ctx *ctx, const puddle_outbox_hooks *hooks, char *err, size_t errlen)
{
    if (ctx->config && ctx->config->calls_off) {
        snprintf(err, errlen, "calls are off (calls: off in puddle.yaml)");
        return NULL;
    }
    if (!hooks || !hooks->send || !ctx->config) {
        snprintf(err, errlen, "calls need a way to reach friends");
        return NULL;
    }
    auto *cs = new puddle_calls();
    cs->ctx = ctx;
    cs->hooks = *hooks;
    cs->hooks.done = NULL;
    cs->hooks.net.connect_timeout_s = 5;
    cs->hooks.net.io_timeout_s = 10;
    cs->hooks.net.total_timeout_s = 15;
    cs->port = puddle_calls_port(ctx->config);
    std::string addr = ctx->config->address;
    if (addr != "::" && addr != "0.0.0.0" && !addr.empty())
        cs->bind = addr;
    if (ctx->config->data_dir[0]) {
        cs->state_dir = std::string(ctx->config->data_dir) + "/state";
        cs->history_path = cs->state_dir + "/calls.jsonl";
        puddle_mkdir_p(cs->state_dir.c_str());
        load_history(cs);
    }
    try {
        cs->worker = std::thread(work, cs);
    } catch (const std::system_error &) {
        delete cs;
        snprintf(err, errlen, "cannot start a thread for calls");
        return NULL;
    }
    puddle_log(PUDDLE_LOG_INFO, "calls: on UDP port %u", (unsigned)cs->port);
    return cs;
}

void puddle_calls_stop(puddle_calls *cs)
{
    if (!cs)
        return;
    CallPtr call;
    {
        std::lock_guard<std::mutex> g(cs->mu);
        call = cs->current;
    }
    if (call) /* the friend hears it went (the worker sends it before it stops) */
        finish(cs, call, "hung_up", call->state == "ringing" ? "declined" : call->state == "calling" ? "cancelled" : "hung_up");
    {
        std::lock_guard<std::mutex> g(cs->mu);
        cs->stopping = true;
        cs->wake.notify_all();
    }
    cs->worker.join();
    std::deque<CallPtr> all;
    {
        std::lock_guard<std::mutex> g(cs->mu);
        all.swap(cs->ended);
    }
    for (auto &c : all)
        if (c->pc) {
            c->pc->resetCallbacks();
            if (c->track)
                c->track->resetCallbacks();
            c->pc->close();
        }
    all.clear();
    delete cs;
}

/* ---- the owner's side ---- */

int puddle_call_start(puddle_ctx *ctx, const char *peer, char id_out[PUDDLE_CALL_ID_LEN + 1], const char **code)
{
    puddle_calls *cs = ctx->calls;
    if (!cs)
        return *code = "no_calls", -1;
    if (!active_friend(ctx, peer))
        return *code = "no_stream", -1;
    auto call = std::make_shared<Call>();
    call->id = new_call_id();
    call->peer = peer;
    call->outgoing = true;
    call->state = "calling";
    call->created = now_of(cs);
    if (call->id.empty())
        return *code = "internal", -1;
    {
        std::lock_guard<std::mutex> g(cs->mu);
        if (cs->current)
            return *code = "busy", -1;
        cs->current = call;
        cs->version++;
    }
    snprintf(id_out, PUDDLE_CALL_ID_LEN + 1, "%s", call->id.c_str());
    std::string sdp;
    try {
        auto pc = make_pc(cs, call);
        {
            std::lock_guard<std::mutex> g(cs->mu);
            call->pc = pc;
        }
        rtc::Description::Audio media(MID, rtc::Description::Direction::SendRecv);
        media.addOpusCodec(OPUS_PT);
        auto track = pc->addTrack(media);
        setup_track(cs, call, track);
        pc->setLocalDescription();
        if (gathered(pc, call, cs, true))
            sdp = std::string(*pc->localDescription());
    } catch (const std::exception &e) {
        puddle_log(PUDDLE_LOG_WARN, "calls: cannot set a call up: %s", e.what());
    }
    if (sdp.empty()) {
        finish(cs, call, "failed", nullptr);
        return 0;
    }
    int status = 0;
    std::string answer;
    if (!tell(cs, call->peer, "/stream/v1/call/offer", body_of(call->id, "sdp", sdp, cs->port), &status, &answer)) {
        finish(cs, call, "unreachable", nullptr);
        return 0;
    }
    if (status != 200) {
        std::string e = error_of(answer);
        finish(cs, call, e == "busy" ? "busy" : e == "no_calls" ? "no_calls" : "failed", nullptr);
        return 0;
    }
    std::lock_guard<std::mutex> g(cs->mu);
    if (call->state == "calling") {
        call->deadline = now_of(cs) + cs->ring_ms;
        cs->wake.notify_all();
    }
    return 0;
}

int puddle_call_accept(puddle_ctx *ctx, const char *id, const char **code)
{
    puddle_calls *cs = ctx->calls;
    if (!cs)
        return *code = "no_calls", -1;
    CallPtr call;
    std::string offer;
    int udp;
    {
        std::lock_guard<std::mutex> g(cs->mu);
        call = cs->current;
        if (!call || call->id != id || call->outgoing || call->state != "ringing")
            return *code = "no_call", -1;
        call->state = "connecting";
        call->deadline = now_of(cs) + cs->connect_ms;
        offer = call->remote_sdp, udp = call->remote_udp;
        cs->version++;
    }
    std::string sdp;
    try {
        auto pc = make_pc(cs, call);
        {
            std::lock_guard<std::mutex> g(cs->mu);
            call->pc = pc;
        }
        std::weak_ptr<Call> weak = call;
        pc->onTrack([cs, weak](std::shared_ptr<rtc::Track> track) { setup_track(cs, weak, track); });
        pc->setRemoteDescription(rtc::Description(offer, "offer")); /* the answer is made from it */
        add_their_addresses(cs, pc, call->peer, udp);
        if (gathered(pc, call, cs, true))
            sdp = std::string(*pc->localDescription());
    } catch (const std::exception &e) {
        puddle_log(PUDDLE_LOG_WARN, "calls: cannot answer %.12s…'s call: %s", call->peer.c_str(), e.what());
    }
    if (sdp.empty()) {
        finish(cs, call, "failed", "failed");
        return 0;
    }
    int status = 0;
    if (!tell(cs, call->peer, "/stream/v1/call/answer", body_of(call->id, "sdp", sdp, cs->port), &status) || status != 204)
        finish(cs, call, "failed", nullptr); /* they gave up meanwhile, or can't be reached: they find out themselves */
    return 0;
}

int puddle_call_decline(puddle_ctx *ctx, const char *id, const char **code)
{
    puddle_calls *cs = ctx->calls;
    if (!cs)
        return *code = "no_calls", -1;
    CallPtr call;
    {
        std::lock_guard<std::mutex> g(cs->mu);
        call = cs->current;
        if (!call || call->id != id || call->outgoing || call->state != "ringing")
            return *code = "no_call", -1;
    }
    finish(cs, call, "declined", "declined");
    return 0;
}

int puddle_call_end(puddle_ctx *ctx, const char *id, const char **code)
{
    puddle_calls *cs = ctx->calls;
    if (!cs)
        return *code = "no_calls", -1;
    CallPtr call;
    const char *reason;
    {
        std::lock_guard<std::mutex> g(cs->mu);
        call = cs->current;
        if (!call || call->id != id)
            return *code = "no_call", -1;
        reason = call->state == "ringing" ? "declined" : call->state == "calling" ? "cancelled" : "hung_up";
    }
    finish(cs, call, reason, reason);
    return 0;
}

static void write_call(puddle_jw *w, const Call &c)
{
    puddle_jw_object(w);
    puddle_jw_str(w, "id", c.id.c_str());
    puddle_jw_str(w, "user", c.peer.c_str());
    puddle_jw_str(w, "direction", c.outgoing ? "outgoing" : "incoming");
    puddle_jw_str(w, "state", c.state.c_str());
    puddle_jw_key(w, "reason");
    if (c.state == "ended")
        puddle_jw_string(w, c.reason.c_str());
    else
        puddle_jw_null(w);
    puddle_jw_flag(w, "media", c.open);
    puddle_jw_num(w, "created", c.created);
    puddle_jw_key(w, "answered");
    c.answered ? puddle_jw_int(w, c.answered) : puddle_jw_null(w);
    puddle_jw_key(w, "ended");
    c.ended ? puddle_jw_int(w, c.ended) : puddle_jw_null(w);
    puddle_jw_end_object(w);
}

static void write_record(puddle_jw *w, const Record &r)
{
    puddle_jw_object(w);
    puddle_jw_str(w, "id", r.id.c_str());
    puddle_jw_str(w, "user", r.peer.c_str());
    puddle_jw_str(w, "direction", r.outgoing ? "outgoing" : "incoming");
    puddle_jw_str(w, "state", "ended");
    puddle_jw_str(w, "reason", r.reason.c_str());
    puddle_jw_flag(w, "media", 0);
    puddle_jw_num(w, "created", r.created);
    puddle_jw_key(w, "answered");
    r.answered ? puddle_jw_int(w, r.answered) : puddle_jw_null(w);
    puddle_jw_num(w, "ended", r.ended);
    puddle_jw_end_object(w);
}

int puddle_calls_write(puddle_ctx *ctx, const char *id, const char *user, int limit, puddle_jw *w)
{
    puddle_calls *cs = ctx->calls;
    if (!cs) {
        if (id)
            return 1;
        puddle_jw_object(w);
        puddle_jw_num(w, "version", 0);
        puddle_jw_key(w, "items");
        puddle_jw_array(w);
        puddle_jw_end_array(w);
        puddle_jw_end_object(w);
        return 0;
    }
    std::lock_guard<std::mutex> g(cs->mu);
    if (id) {
        if (cs->current && cs->current->id == id)
            return write_call(w, *cs->current), 0;
        for (auto it = cs->history.rbegin(); it != cs->history.rend(); ++it)
            if (it->id == id)
                return write_record(w, *it), 0;
        return 1;
    }
    puddle_jw_object(w);
    puddle_jw_num(w, "version", (int64_t)cs->version.load());
    puddle_jw_key(w, "items");
    puddle_jw_array(w);
    int n = 0;
    if (cs->current && (!user || cs->current->peer == user))
        write_call(w, *cs->current), n++;
    for (auto it = cs->history.rbegin(); it != cs->history.rend() && n < limit && w->len + 600 < w->cap; ++it)
        if (!user || it->peer == user)
            write_record(w, *it), n++;
    puddle_jw_end_array(w);
    puddle_jw_end_object(w);
    return 0;
}

/* ---- media ---- */

void puddle_calls_on_audio(puddle_calls *cs, puddle_call_frame_fn fn, void *user)
{
    if (!cs)
        return;
    std::lock_guard<std::mutex> g(cs->mu);
    cs->on_audio = fn, cs->on_audio_user = user;
}

int puddle_call_send_audio(puddle_calls *cs, const char *id, const unsigned char *frame, size_t len)
{
    if (!cs || !frame || !len || len > PUDDLE_CALL_FRAME_MAX)
        return -1;
    std::shared_ptr<rtc::Track> track;
    uint32_t ts;
    {
        std::lock_guard<std::mutex> g(cs->mu);
        CallPtr call = cs->current;
        if (!call || call->id != id || call->state != "active" || !call->open || !call->track)
            return -1;
        track = call->track;
        ts = call->next_ts;
        call->next_ts += 960; /* 20 ms at 48 kHz */
    }
    try {
        track->sendFrame((const rtc::byte *)frame, len, rtc::FrameInfo(ts));
    } catch (const std::exception &) {
        return -1;
    }
    return 0;
}

void puddle_calls_set_timeouts(puddle_calls *cs, int64_t ring_ms, int64_t ring_in_ms, int64_t connect_ms)
{
    if (!cs)
        return;
    std::lock_guard<std::mutex> g(cs->mu);
    if (ring_ms)
        cs->ring_ms = ring_ms;
    if (ring_in_ms)
        cs->ring_in_ms = ring_in_ms;
    if (connect_ms)
        cs->connect_ms = connect_ms;
}

uint64_t puddle_calls_version(puddle_calls *cs)
{
    return cs ? cs->version.load() : 0;
}

/* ---- the routes friends call ---- */

static bool read_body(const puddle_call *c, char id[PUDDLE_CALL_ID_LEN + 2], const char *key, std::string *value, int *udp)
{
    puddle_span v;
    const char *b = (const char *)c->body;
    if (!b || puddle_json_member(b, c->body_len, "call", &v) != 0 || puddle_json_string(v, id, PUDDLE_CALL_ID_LEN + 2) != 0 ||
        !call_id_ok(id))
        return false;
    if (puddle_json_member(b, c->body_len, key, &v) != 0)
        return false;
    std::string buf(PUDDLE_CALL_SDP_MAX + 1, '\0');
    if (puddle_json_string(v, buf.data(), buf.size()) != 0)
        return false;
    value->assign(buf.c_str());
    if (udp) {
        int64_t p;
        if (puddle_json_member(b, c->body_len, "udp", &v) != 0 || puddle_json_int64(v, &p) != 0 || p < 1 || p > 65535)
            return false;
        *udp = (int)p;
    }
    return !value->empty();
}

static int offer_post(const puddle_call *c, puddle_jw *w)
{
    puddle_calls *cs = c->ctx->calls;
    if (!cs)
        return puddle_fail(w, 409, "no_calls");
    if (!active_friend(c->ctx, c->sender))
        return puddle_fail(w, 403, "no_stream");
    char id[PUDDLE_CALL_ID_LEN + 2];
    std::string sdp;
    int udp = 0;
    if (!read_body(c, id, "sdp", &sdp, &udp))
        return puddle_fail(w, 400, "bad_request");
    {
        std::lock_guard<std::mutex> g(cs->mu);
        if (cs->current) {
            if (cs->current->id == id && cs->current->peer == c->sender && !cs->current->outgoing)
                goto ringing; /* the same offer again: still ringing */
            return puddle_fail(w, 409, "busy");
        }
        auto call = std::make_shared<Call>();
        call->id = id;
        call->peer = c->sender;
        call->state = "ringing";
        call->created = now_of(cs);
        call->deadline = call->created + cs->ring_in_ms;
        call->remote_sdp = sdp;
        call->remote_udp = udp;
        cs->current = call;
        cs->version++;
        cs->wake.notify_all();
    }
    puddle_log(PUDDLE_LOG_INFO, "calls: %.12s… is calling", c->sender);
ringing:
    puddle_jw_object(w);
    puddle_jw_str(w, "state", "ringing");
    puddle_jw_end_object(w);
    return 200;
}

static int answer_post(const puddle_call *c, puddle_jw *w)
{
    puddle_calls *cs = c->ctx->calls;
    if (!cs)
        return puddle_fail(w, 409, "no_calls");
    char id[PUDDLE_CALL_ID_LEN + 2];
    std::string sdp;
    int udp = 0;
    if (!read_body(c, id, "sdp", &sdp, &udp))
        return puddle_fail(w, 400, "bad_request");
    CallPtr call;
    std::shared_ptr<rtc::PeerConnection> pc;
    {
        std::lock_guard<std::mutex> g(cs->mu);
        call = cs->current;
        if (!call || call->id != id || call->peer != c->sender || !call->outgoing || call->state != "calling" || !call->pc)
            return puddle_fail(w, 404, "no_call");
        call->state = "connecting";
        call->deadline = now_of(cs) + cs->connect_ms;
        pc = call->pc;
        cs->version++;
    }
    try {
        pc->setRemoteDescription(rtc::Description(sdp, "answer"));
        add_their_addresses(cs, pc, call->peer, udp);
    } catch (const std::exception &e) {
        puddle_log(PUDDLE_LOG_WARN, "calls: %.12s…'s answer can't be used: %s", c->sender, e.what());
        finish(cs, call, "failed", "failed");
        return puddle_fail(w, 400, "bad_sdp");
    }
    return 204;
}

static int end_post(const puddle_call *c, puddle_jw *w)
{
    puddle_calls *cs = c->ctx->calls;
    if (!cs)
        return puddle_fail(w, 409, "no_calls");
    char id[PUDDLE_CALL_ID_LEN + 2];
    std::string said;
    if (!read_body(c, id, "reason", &said, nullptr))
        return puddle_fail(w, 400, "bad_request");
    CallPtr call = find(cs, id);
    if (!call || call->peer != c->sender)
        return puddle_fail(w, 404, "no_call");
    static const char *const KNOWN[] = { "hung_up", "declined", "cancelled", "no_answer", "busy", "failed" };
    const char *reason = "hung_up";
    for (const char *k : KNOWN)
        if (said == k)
            reason = k;
    bool rang = false;
    {
        std::lock_guard<std::mutex> g(cs->mu);
        rang = !call->outgoing && call->state == "ringing";
    }
    if (rang && (!strcmp(reason, "cancelled") || !strcmp(reason, "no_answer")))
        reason = "missed";
    finish(cs, call, reason, nullptr);
    return 204;
}

const puddle_route puddle_call_routes[] = {
    { "POST", "/stream/v1/call/offer",  PUDDLE_AUTH_PEER, PUDDLE_CALL_SDP_MAX + 1024, offer_post,  NULL },
    { "POST", "/stream/v1/call/answer", PUDDLE_AUTH_PEER, PUDDLE_CALL_SDP_MAX + 1024, answer_post, NULL },
    { "POST", "/stream/v1/call/end",    PUDDLE_AUTH_PEER, 1024, end_post, NULL },
};
const size_t puddle_call_routes_count = sizeof puddle_call_routes / sizeof puddle_call_routes[0];

#endif
