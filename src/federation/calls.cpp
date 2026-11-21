/* Calls between friends (docs/calls.md). See federation/calls.h. */
#include "federation/calls.h"

#include "core/levee.h"

#include "base/encoding.h"
#include "base/log.h"
#include "core/devices.h"
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
int puddle_call_start(puddle_ctx *, const char *, const char *, char *, const char **code) { return *code = "no_calls", -1; }
int puddle_call_start_group(puddle_ctx *, const char *, const char *, char *, const char **code) { return *code = "no_calls", -1; }
int puddle_call_accept(puddle_ctx *, const char *, const char *, const char **code) { return *code = "no_calls", -1; }
int puddle_call_media(puddle_ctx *, const char *, const char *, const char *, char **, int *, const char **code) { return *code = "no_calls", -1; }
int puddle_call_decline(puddle_ctx *, const char *, const char **code) { return *code = "no_calls", -1; }
int puddle_call_end(puddle_ctx *, const char *, const char **code) { return *code = "no_calls", -1; }
int puddle_calls_write(puddle_ctx *, const char *id, const char *, const char *, int, puddle_jw *w)
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
void puddle_calls_set_beat(puddle_calls *, int64_t) {}
int puddle_call_send_audio(puddle_calls *, const char *, const unsigned char *, size_t) { return -1; }
void puddle_calls_on_video(puddle_calls *, puddle_call_frame_fn, void *) {}
int puddle_call_send_video(puddle_calls *, const char *, const unsigned char *, size_t) { return -1; }
int puddle_call_send_screen(puddle_calls *, const char *, const unsigned char *, size_t) { return -1; }
uint64_t puddle_calls_version(puddle_calls *) { return 0; }
int puddle_voice_join(puddle_ctx *, const char *, int64_t, const char **code) { return *code = "no_calls", -1; }
int puddle_voice_leave(puddle_ctx *, const char **) { return 0; }
int puddle_voice_state(puddle_ctx *, int, int, int, int, const char **code) { return *code = "no_voice", -1; }
void puddle_voice_write(puddle_ctx *, puddle_jw *w) { puddle_jw_null(w); }
int puddle_voice_send_audio(puddle_calls *, const unsigned char *, size_t) { return -1; }
int puddle_voice_send_video(puddle_calls *, const unsigned char *, size_t, int) { return -1; }

static int no_calls(const puddle_call *, puddle_jw *w) { return puddle_fail(w, 409, "no_calls"); }

const puddle_route puddle_call_routes[] = {
    { "POST", "/stream/v1/call/offer",  PUDDLE_AUTH_PEER, PUDDLE_CALL_SDP_MAX + 1024, no_calls, NULL },
    { "POST", "/stream/v1/call/answer", PUDDLE_AUTH_PEER, PUDDLE_CALL_SDP_MAX + 1024, no_calls, NULL },
    { "POST", "/stream/v1/call/end",    PUDDLE_AUTH_PEER, 1024, no_calls, NULL },
    { "POST", "/stream/v1/call/group/join",   PUDDLE_AUTH_PEER, 1024, no_calls, NULL },
    { "POST", "/stream/v1/call/group/leave",  PUDDLE_AUTH_PEER, 1024, no_calls, NULL },
    { "POST", "/stream/v1/call/group/offer",  PUDDLE_AUTH_PEER, PUDDLE_CALL_SDP_MAX + 1024, no_calls, NULL },
    { "POST", "/stream/v1/call/group/answer", PUDDLE_AUTH_PEER, PUDDLE_CALL_SDP_MAX + 1024, no_calls, NULL },
};
const size_t puddle_call_routes_count = sizeof puddle_call_routes / sizeof puddle_call_routes[0];

#else

#include "federation/calls_internal.h"
#include "core/identity.h"

#include <openssl/rand.h>

using namespace callsx;

int puddle_calls_built(void) { return 1; }

int64_t callsx::now_of(puddle_calls *cs) { return puddle_ctx_now_ms(cs->ctx); }

std::string callsx::new_call_id()
{
    unsigned char raw[16];
    char out[PUDDLE_CALL_ID_LEN + 8];
    if (RAND_bytes(raw, sizeof raw) != 1)
        return "";
    puddle_base32_encode(raw, sizeof raw, out, sizeof out);
    return std::string(out, PUDDLE_CALL_ID_LEN);
}

uint32_t callsx::random32()
{
    uint32_t v = 0;
    RAND_bytes((unsigned char *)&v, sizeof v);
    return v;
}

bool callsx::call_id_ok(const char *s)
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
bool callsx::tell(puddle_calls *cs, const std::string &peer, const char *path, std::string body, int *status, std::string *answer)
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

static std::string body_of(const std::string &id, const char *key, const std::string &value, int udp = -1)
{
    std::string buf(value.size() * 2 + 256, '\0');
    puddle_jw w;
    puddle_jw_init(&w, buf.data(), buf.size());
    puddle_jw_object(&w);
    puddle_jw_str(&w, "call", id.c_str());
    puddle_jw_str(&w, key, value.c_str());
    if (udp >= 0)
        puddle_jw_num(&w, "udp", udp);
    puddle_jw_end_object(&w);
    size_t n = puddle_jw_finish(&w);
    return std::string(buf.data(), n);
}

std::string callsx::error_of(const std::string &json)
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
    if (!r.group.empty())
        puddle_jw_str(&w, "group", r.group.c_str());
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
    if (puddle_json_member(line, len, "group", &v) == 0 &&
        (!str("group", r.group, PUDDLE_USER_ID_LEN) || !puddle_id_valid(r.group.c_str())))
        return false;
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
void callsx::remember(puddle_calls *cs, const Record &r)
{
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
        Record r;
        r.id = call->id, r.peer = call->peer, r.reason = call->reason, r.outgoing = call->outgoing;
        r.created = call->created, r.answered = call->answered, r.ended = call->ended;
        remember(cs, r);
        pc = call->pc;
        if (pc) /* closed by the worker: this may be running in one of the connection's own callbacks */
            cs->tasks.push_back([pc] { pc->close(); });
        if (call->leg)
            close_leg(cs, call->leg);
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

/* Say in the track's description which SSRC we send it from, so the other side can tell it from the connection's other
 * tracks (libdatachannel otherwise gives a packet it can't place to every track). */
static uint32_t own_ssrc(const std::shared_ptr<rtc::Track> &track)
{
    uint32_t ssrc = random32() | 1;
    auto d = track->description();
    d.addSSRC(ssrc, std::string("puddle"));
    track->setDescription(std::move(d));
    return ssrc;
}

/* Opus out, packetized with sender reports, and in, as whole frames. */
std::shared_ptr<rtc::RtpPacketizationConfig> callsx::opus_handler(const std::shared_ptr<rtc::Track> &track)
{
    auto rtp = std::make_shared<rtc::RtpPacketizationConfig>(own_ssrc(track), "puddle", OPUS_PT, rtc::OpusRtpPacketizer::DefaultClockRate);
    auto pack = std::make_shared<rtc::OpusRtpPacketizer>(rtp);
    pack->addToChain(std::make_shared<rtc::RtcpSrReporter>(rtp));
    // rtc::OpusRtpDepacketizer is the same at 48 kHz, but a template its Windows DLL doesn't export
    pack->addToChain(std::make_shared<rtc::RtpDepacketizer>(rtc::OpusRtpPacketizer::DefaultClockRate));
    track->setMediaHandler(pack);
    return rtp;
}

void callsx::h264_handler(const std::shared_ptr<rtc::Track> &track)
{
    auto rtp = std::make_shared<rtc::RtpPacketizationConfig>(own_ssrc(track), "puddle", H264_PT, rtc::H264RtpPacketizer::ClockRate);
    auto pack = std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, rtp, 1200);
    pack->addToChain(std::make_shared<rtc::RtcpSrReporter>(rtp));
    pack->addToChain(std::make_shared<rtc::H264RtpDepacketizer>(rtc::NalUnit::Separator::LongStartSequence));
    track->setMediaHandler(pack);
}

rtc::Description::Video callsx::video_media(const char *mid, rtc::Description::Direction dir)
{
    rtc::Description::Video media(mid, dir);
    media.addH264Codec(H264_PT); /* Constrained Baseline, packetization-mode 1 */
    return media;
}

uint32_t callsx::video_now()
{
    using namespace std::chrono;
    return (uint32_t)(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count() * 9 / 100);
}

bool callsx::track_open(const std::shared_ptr<rtc::Track> &t)
{
    try {
        return t && t->isOpen();
    } catch (const std::exception &) {
        return false;
    }
}

/* The RTP timestamp for a frame passed on, whose own was `in`: ours moves on by a frame, and by as much again as theirs
 * skipped, so a gap (a lost frame, a pause) stays one for the jitter buffer at the other end. Called with the lock held. */
uint32_t callsx::stamp(uint32_t &next, uint32_t &last, bool &have, uint32_t in)
{
    if (have) {
        uint32_t d = in - last;
        if (d > 960 && d <= 96000) /* up to 2 s; anything else is a new start, not a gap */
            next += d - 960;
    }
    last = in, have = true;
    uint32_t ts = next;
    next += 960;
    return ts;
}

void callsx::send_on(const std::shared_ptr<rtc::Track> &track, const rtc::binary &data, uint32_t ts)
{
    try {
        track->sendFrame(data, rtc::FrameInfo(ts));
    } catch (const std::exception &) {
    }
}

/* The friend's video, or the screen they share (`screen`): to the owner's device here (a screen as from "<them>/screen"), or
 * on to the phone that has the call, on its leg's video track (a screen isn't passed to a phone yet). */
static void setup_vtrack(puddle_calls *cs, const std::weak_ptr<Call> &weak, const std::shared_ptr<rtc::Track> &track, bool screen = false)
{
    h264_handler(track);
    std::string id, peer;
    {
        std::lock_guard<std::mutex> g(cs->mu);
        CallPtr call = weak.lock();
        if (!call)
            return;
        peer = call->peer + (screen ? "/screen" : ""), id = call->id;
        (screen ? call->strack : call->vtrack) = track;
        (screen ? call->sbase : call->vbase) = random32();
    }
    track->onFrame([cs, weak, id, peer, screen](rtc::binary data, rtc::FrameInfo info) {
        if (data.empty() || data.size() > PUDDLE_CALL_VIDEO_MAX)
            return;
        puddle_call_frame_fn fn = nullptr;
        void *user = nullptr;
        std::shared_ptr<rtc::Track> phone;
        uint32_t ts = 0;
        {
            std::lock_guard<std::mutex> g(cs->mu);
            CallPtr call = weak.lock();
            if (!call || call->state == "ended")
                return;
            if (call->device.empty()) {
                fn = cs->on_video, user = cs->on_video_user;
            } else if (!screen && call->leg && track_open(call->leg->vtrack)) {
                phone = call->leg->vtrack;
                ts = call->leg->vbase[0] + info.timestamp;
            }
        }
        if (fn)
            fn(user, id.c_str(), peer.c_str(), (const unsigned char *)data.data(), data.size(), info.timestamp);
        else if (phone)
            send_on(phone, data, ts);
    });
}

/* The friend's connection: frames go to the owner's device here (the window), or on to the phone that has the call. */
static void setup_track(puddle_calls *cs, const std::weak_ptr<Call> &weak, const std::shared_ptr<rtc::Track> &track)
{
    if (track->mid() == VIDEO_MID || track->mid() == SCREEN_MID)
        return setup_vtrack(cs, weak, track, track->mid() == SCREEN_MID);
    if (track->mid() != MID)
        return; /* a track from a newer Puddle: left as it came, and nothing is sent on it */
    auto rtp = opus_handler(track);
    std::string id, peer;
    {
        std::lock_guard<std::mutex> g(cs->mu);
        CallPtr call = weak.lock();
        if (!call)
            return;
        peer = call->peer;
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
    track->onFrame([cs, weak, id, peer](rtc::binary data, rtc::FrameInfo info) {
        if (data.empty() || data.size() > PUDDLE_CALL_FRAME_MAX)
            return;
        puddle_call_frame_fn fn = nullptr;
        void *user = nullptr;
        std::shared_ptr<rtc::Track> phone;
        uint32_t ts = 0;
        {
            std::lock_guard<std::mutex> g(cs->mu);
            CallPtr call = weak.lock();
            if (!call)
                return;
            if (call->device.empty()) {
                fn = cs->on_audio, user = cs->on_audio_user;
            } else if (call->leg && call->leg->open && call->leg->track) {
                phone = call->leg->track;
                ts = stamp(call->leg->next_ts, call->leg->last_in, call->leg->have_in, info.timestamp);
            }
        }
        if (fn)
            fn(user, id.c_str(), peer.c_str(), (const unsigned char *)data.data(), data.size(), info.timestamp);
        else if (phone)
            send_on(phone, data, ts);
    });
}

rtc::Configuration callsx::pc_config(puddle_calls *cs, bool pond)
{
    rtc::Configuration conf; /* no ICE servers: only the two Puddles' own addresses (docs/calls.md, "Finding a path") */
    conf.disableAutoNegotiation = true; /* the answer waits until each track says its SSRC (answer_when_ready) */
    puddle_levee_state lv;
    /* Pond voice too, unless the owner chose to go straight to the Pond (docs/levee.md, "Pond voice") */
    if (puddle_levee_current(&lv) && lv.turn_port && !(pond && lv.pond_voice_direct)) {
        /* behind a Levee nothing reaches us by UDP either: everything goes through its relay, and only the relay's addresses are
         * offered, so ours stays private (spec/06, "Calls: the relay"). The relay is our own Levee's, nobody else's. */
        conf.iceServers.emplace_back(lv.address.host, (uint16_t)lv.turn_port, lv.turn_user, lv.turn_pass, rtc::IceServer::RelayType::TurnUdp);
        conf.iceTransportPolicy = rtc::TransportPolicy::Relay;
        return conf;
    }
    conf.enableIceUdpMux = true;
    conf.portRangeBegin = conf.portRangeEnd = cs->port;
    if (!cs->bind.empty())
        conf.bindAddress = cs->bind;
    return conf;
}

int callsx::our_udp(puddle_calls *cs)
{
    puddle_levee_state lv;
    return puddle_levee_current(&lv) && lv.turn_port ? 0 : (int)cs->port;
}

static std::shared_ptr<rtc::PeerConnection> make_pc(puddle_calls *cs, const CallPtr &call)
{
    auto pc = std::make_shared<rtc::PeerConnection>(pc_config(cs));
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

/* Wait until our side of the description is complete (candidates and all: nothing trickles) and the track is there. */
bool callsx::gathered(const std::shared_ptr<rtc::PeerConnection> &pc, puddle_calls *cs, const std::function<bool()> &has_track)
{
    for (int i = 0; i < 500; i++) {
        bool track;
        {
            std::lock_guard<std::mutex> g(cs->mu);
            track = has_track();
        }
        if (pc->gatheringState() == rtc::PeerConnection::GatheringState::Complete && pc->localDescription() && track)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

static int media_lines(const std::string &sdp)
{
    int n = 0;
    for (size_t at = 0; (at = sdp.find("m=", at)) != std::string::npos; at += 2)
        if ((at == 0 || sdp[at - 1] == '\n') && (!sdp.compare(at, 7, "m=audio") || !sdp.compare(at, 7, "m=video")))
            n++;
    return n;
}

std::string callsx::answer_when_ready(const std::shared_ptr<rtc::PeerConnection> &pc, puddle_calls *cs, const std::string &offer,
                                      const std::shared_ptr<std::atomic<int>> &seen)
{
    const int want = media_lines(offer);
    for (int i = 0; i < 500 && seen->load() < want; i++)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (seen->load() < want)
        return "";
    pc->setLocalDescription();
    if (!gathered(pc, cs, [] { return true; }))
        return "";
    return std::string(*pc->localDescription());
}

/* The friend's advertised addresses, at the UDP port they said, as candidates beside the ones their description lists. */
void callsx::add_their_addresses(puddle_calls *cs, const std::shared_ptr<rtc::PeerConnection> &pc, const std::string &peer, int udp)
{
    if (udp <= 0) /* behind a Levee: their description's relayed candidates are all there is (spec/08) */
        return;
    puddle_address addrs[4];
    int n = cs->hooks.addresses ? cs->hooks.addresses(cs->hooks.user, peer.c_str(), addrs, 4) : 0;
    for (int i = 0; i < n; i++) {
        std::string cand = "a=candidate:" + std::to_string(i + 9) + " 1 UDP " + std::to_string(2122000000 - i) + " " + addrs[i].host + " " +
                           std::to_string(udp) + " typ host";
        try {
            pc->addRemoteCandidate(rtc::Candidate(cand, MID));
        } catch (const std::exception &e) {
            puddle_log(PUDDLE_LOG_DEBUG, "calls: not using %s for %.12s…: %s", addrs[i].host, peer.c_str(), e.what());
        }
    }
}

/* ---- a phone's leg ---- */

void callsx::close_leg(puddle_calls *cs, const LegPtr &leg)
{
    leg->open = leg->dc_open = false;
    if (auto pc = leg->pc) {
        cs->tasks.push_back([pc] { pc->close(); });
        cs->wake.notify_all();
    }
}

/* The same port, and only the phone's own addresses and ours (it adds our advertised one itself). What it sends goes to
 * `frame`; a leg that fails ends what it is for, unless a newer one took its place. */
std::string callsx::answer_leg(puddle_calls *cs, const LegPtr &leg, const std::string &offer, std::function<bool(const LegPtr &)> mine,
                               std::function<void()> fail, std::function<void(const rtc::binary &, uint32_t)> frame,
                               std::function<void(const rtc::binary &, uint32_t)> vframe)
{
    std::shared_ptr<rtc::PeerConnection> pc;
    std::string sdp;
    std::weak_ptr<Leg> wleg = leg;
    try {
        pc = std::make_shared<rtc::PeerConnection>(pc_config(cs));
        leg->pc = pc;
        pc->onStateChange([cs, wleg, mine, fail](rtc::PeerConnection::State s) {
            LegPtr leg = wleg.lock();
            if (!leg)
                return;
            bool failed = false;
            {
                std::lock_guard<std::mutex> g(cs->mu);
                if (!mine(leg)) /* over, or a newer leg took its place */
                    return;
                if (s == rtc::PeerConnection::State::Connected) {
                    if (!leg->connected)
                        puddle_log(PUDDLE_LOG_INFO, "calls: the phone's leg connected");
                    leg->connected = true;
                    leg->lost_since = 0;
                    cs->version++;
                } else if (s == rtc::PeerConnection::State::Disconnected) {
                    leg->lost_since = now_of(cs);
                } else if (s == rtc::PeerConnection::State::Failed || s == rtc::PeerConnection::State::Closed) {
                    failed = true;
                }
            }
            if (failed)
                fail();
        });
        auto seen = std::make_shared<std::atomic<int>>(0);
        pc->onTrack([cs, wleg, frame, vframe, seen](std::shared_ptr<rtc::Track> track) {
            struct Count {
                std::shared_ptr<std::atomic<int>> n;
                ~Count() { (*n)++; }
            } counted{ seen };
            const std::string mid = track->mid();
            if (mid == VIDEO_MID || (mid.size() == 2 && mid[0] == 'v' && mid[1] >= '1' && mid[1] <= '0' + PHONE_SLOTS)) {
                /* its camera (and the friend's video back), or a member's video in a group call */
                h264_handler(track);
                std::lock_guard<std::mutex> g(cs->mu);
                LegPtr leg = wleg.lock();
                if (!leg)
                    return;
                int n = mid == VIDEO_MID ? 0 : mid[1] - '0';
                (n ? leg->vslots[n] : leg->vtrack) = track;
                leg->vbase[n] = random32();
                if (!n)
                    track->onFrame([vframe](rtc::binary data, rtc::FrameInfo info) {
                        if (!data.empty() && data.size() <= PUDDLE_CALL_VIDEO_MAX)
                            vframe(data, info.timestamp);
                    });
                return;
            }
            opus_handler(track);
            {
                std::lock_guard<std::mutex> g(cs->mu);
                LegPtr leg = wleg.lock();
                if (!leg)
                    return;
                leg->track = track;
            }
            track->onOpen([cs, wleg] {
                std::lock_guard<std::mutex> g(cs->mu);
                if (LegPtr leg = wleg.lock()) {
                    leg->open = true;
                    cs->version++;
                }
            });
            track->onFrame([frame](rtc::binary data, rtc::FrameInfo info) {
                if (!data.empty() && data.size() <= PUDDLE_CALL_FRAME_MAX)
                    frame(data, info.timestamp);
            });
        });
        /* the members' frames in a group call, each marked with whose it is ("Group calls on a phone") */
        pc->onDataChannel([cs, wleg](std::shared_ptr<rtc::DataChannel> dc) {
            std::lock_guard<std::mutex> g(cs->mu);
            LegPtr leg = wleg.lock();
            if (!leg)
                return;
            leg->dc = dc;
            dc->onOpen([cs, wleg] {
                std::lock_guard<std::mutex> g(cs->mu);
                if (LegPtr leg = wleg.lock())
                    leg->dc_open = true;
            });
        });
        pc->setRemoteDescription(rtc::Description(offer, "offer"));
        sdp = answer_when_ready(pc, cs, offer, seen);
    } catch (const std::exception &e) {
        puddle_log(PUDDLE_LOG_INFO, "calls: a phone's offer can't be used: %s", e.what());
    }
    if (sdp.empty() && pc) {
        pc->resetCallbacks();
        pc->close();
    }
    return sdp;
}

/* ---- the worker: deadlines, and telling friends a call ended ---- */

/* The phone that has a call asks how it stands every moment it is open (the app's Calls, every 0.7 s). One that has stopped
 * asking, or was removed, is gone, and the call with it: quicker to notice than the leg's own connection dropping. */
bool callsx::phone_gone(puddle_calls *cs, const std::string &device, int64_t now)
{
    puddle_device dv;
    if (!cs->ctx->devices || puddle_devices_get(cs->ctx->devices, device.c_str(), &dv) != 0 || dv.state != PUDDLE_DEVICE_ACTIVE)
        return true;
    return dv.last_seen_ms && now - dv.last_seen_ms >= LOST_MS;
}

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
        } else if (call && call->leg && !call->leg->connected && now >= call->leg->deadline) {
            reason = "failed", told = "failed"; /* the phone never got through to us */
        } else if (call && call->leg && call->leg->lost_since && now - call->leg->lost_since >= LOST_MS) {
            reason = "failed", told = "failed";
        } else if (call && !call->device.empty() && phone_gone(cs, call->device, now)) {
            reason = "failed", told = "failed";
        }
        lk.unlock();
        gone.clear();
        if (reason)
            finish(cs, call, reason, told);
        group_tick(cs, now);
        voice_tick(cs, now);
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
    if (ctx->identity)
        puddle_identity_id(ctx->identity, cs->me);
    try {
        cs->worker = std::thread(work, cs);
        for (int i = 0; i < 4; i++) /* a group call's requests: to each member, at once */
            cs->gpool.emplace_back([cs] {
                std::unique_lock<std::mutex> lk(cs->mu);
                while (true) {
                    if (!cs->gtasks.empty()) {
                        auto t = std::move(cs->gtasks.front());
                        cs->gtasks.pop_front();
                        lk.unlock();
                        t();
                        lk.lock();
                        continue;
                    }
                    if (cs->stopping)
                        break;
                    cs->gwake.wait(lk);
                }
            });
    } catch (const std::system_error &) {
        {
            std::lock_guard<std::mutex> g(cs->mu);
            cs->stopping = true;
            cs->wake.notify_all();
            cs->gwake.notify_all();
        }
        if (cs->worker.joinable())
            cs->worker.join();
        for (auto &t : cs->gpool)
            t.join();
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
    group_stop(cs); /* the members hear we left (the pool sends it before it stops) */
    voice_stop(cs); /* and the Pond, out of its voice channel (the worker sends it before it stops) */
    {
        std::lock_guard<std::mutex> g(cs->mu);
        cs->stopping = true;
        cs->wake.notify_all();
        cs->gwake.notify_all();
    }
    cs->worker.join();
    for (auto &t : cs->gpool)
        t.join();
    group_release(cs);
    std::deque<CallPtr> all;
    {
        std::lock_guard<std::mutex> g(cs->mu);
        all.swap(cs->ended);
    }
    for (auto &c : all) {
        if (c->pc) {
            c->pc->resetCallbacks();
            if (c->track)
                c->track->resetCallbacks();
            c->pc->close();
        }
        if (c->leg && c->leg->pc) {
            c->leg->pc->resetCallbacks();
            if (c->leg->track)
                c->leg->track->resetCallbacks();
            c->leg->pc->close();
        }
    }
    all.clear();
    delete cs;
}

/* ---- the owner's side ---- */

int puddle_call_start(puddle_ctx *ctx, const char *peer, const char *device, char id_out[PUDDLE_CALL_ID_LEN + 1], const char **code)
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
    call->device = device ? device : "";
    call->state = "calling";
    call->created = now_of(cs);
    if (call->id.empty())
        return *code = "internal", -1;
    {
        std::lock_guard<std::mutex> g(cs->mu);
        if (cs->current || group_busy(cs) || in_voice(cs))
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
        setup_vtrack(cs, call, pc->addTrack(video_media(VIDEO_MID, rtc::Description::Direction::SendRecv)));
        setup_vtrack(cs, call, pc->addTrack(video_media(SCREEN_MID, rtc::Description::Direction::SendRecv)), true);
        pc->setLocalDescription();
        if (gathered(pc, cs, [&] { return call->track != nullptr; }))
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
    if (!tell(cs, call->peer, "/stream/v1/call/offer", body_of(call->id, "sdp", sdp, our_udp(cs)), &status, &answer)) {
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

int puddle_call_start_group(puddle_ctx *ctx, const char *gid, const char *device, char id[PUDDLE_CALL_ID_LEN + 1], const char **code)
{
    if (!ctx->calls)
        return *code = "no_calls", -1;
    return group_start(ctx, gid, device, id, code);
}

int puddle_call_accept(puddle_ctx *ctx, const char *id, const char *device, const char **code)
{
    puddle_calls *cs = ctx->calls;
    if (!cs)
        return *code = "no_calls", -1;
    if (int r = group_accept(ctx, id, device, code); r != 1)
        return r;
    CallPtr call;
    std::string offer;
    int udp;
    {
        std::lock_guard<std::mutex> g(cs->mu);
        call = cs->current;
        if (!call || call->id != id || call->outgoing || call->state != "ringing")
            return *code = "no_call", -1;
        call->state = "connecting";
        call->device = device ? device : "";
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
        auto seen = std::make_shared<std::atomic<int>>(0);
        pc->onTrack([cs, weak, seen](std::shared_ptr<rtc::Track> track) {
            setup_track(cs, weak, track);
            (*seen)++;
        });
        pc->setRemoteDescription(rtc::Description(offer, "offer")); /* the answer is made from it */
        add_their_addresses(cs, pc, call->peer, udp);
        sdp = answer_when_ready(pc, cs, offer, seen);
    } catch (const std::exception &e) {
        puddle_log(PUDDLE_LOG_WARN, "calls: cannot answer %.12s…'s call: %s", call->peer.c_str(), e.what());
    }
    if (sdp.empty()) {
        finish(cs, call, "failed", "failed");
        return 0;
    }
    int status = 0;
    if (!tell(cs, call->peer, "/stream/v1/call/answer", body_of(call->id, "sdp", sdp, our_udp(cs)), &status) || status != 204)
        finish(cs, call, "failed", nullptr); /* they gave up meanwhile, or can't be reached: they find out themselves */
    return 0;
}

int puddle_call_decline(puddle_ctx *ctx, const char *id, const char **code)
{
    puddle_calls *cs = ctx->calls;
    if (!cs)
        return *code = "no_calls", -1;
    if (int r = group_decline(ctx, id, code); r != 1)
        return r;
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
    if (int r = group_end(ctx, id, code); r != 1)
        return r;
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

int puddle_call_media(puddle_ctx *ctx, const char *id, const char *device, const char *offer, char **answer_out, int *udp_out,
                      const char **code)
{
    puddle_calls *cs = ctx->calls;
    if (!cs)
        return *code = "no_calls", -1;
    if (!device || !*device)
        return *code = "no_call", -1;
    if (int r = group_media(ctx, id, device, offer, answer_out, udp_out, code); r != 1)
        return r;
    CallPtr call;
    {
        std::lock_guard<std::mutex> g(cs->mu);
        call = cs->current;
        if (!call || call->id != id || call->device != device)
            return *code = "no_call", -1;
    }
    auto leg = std::make_shared<Leg>();
    leg->next_ts = random32();
    std::weak_ptr<Call> wcall = call;
    std::string sdp = answer_leg(
        cs, leg, offer,
        [wcall](const LegPtr &l) {
            CallPtr c = wcall.lock();
            return c && c->state != "ended" && c->leg == l;
        },
        [cs, wcall] {
            if (CallPtr c = wcall.lock())
                finish(cs, c, "failed", "failed");
        },
        [cs, wcall, leg = std::weak_ptr<Leg>(leg)](const rtc::binary &data, uint32_t in) { /* on to the friend once active */
            std::shared_ptr<rtc::Track> to;
            uint32_t ts;
            {
                std::lock_guard<std::mutex> g(cs->mu);
                CallPtr call = wcall.lock();
                LegPtr l = leg.lock();
                if (!call || !l || call->leg != l || call->state != "active" || !call->open || !call->track)
                    return;
                to = call->track;
                ts = stamp(call->next_ts, call->last_in, call->have_in, in);
            }
            send_on(to, data, ts);
        },
        [cs, wcall, leg = std::weak_ptr<Leg>(leg)](const rtc::binary &data, uint32_t in) { /* its camera, on to the friend */
            std::shared_ptr<rtc::Track> to;
            uint32_t ts;
            {
                std::lock_guard<std::mutex> g(cs->mu);
                CallPtr call = wcall.lock();
                LegPtr l = leg.lock();
                if (!call || !l || call->leg != l || call->state != "active" || !track_open(call->vtrack))
                    return;
                to = call->vtrack;
                ts = call->vbase + in;
            }
            send_on(to, data, ts);
        });
    if (sdp.empty())
        return *code = "bad_sdp", -1;
    {
        std::lock_guard<std::mutex> g(cs->mu);
        if (call->state == "ended" || call->device != device) {
            leg->pc->resetCallbacks();
            close_leg(cs, leg);
            return *code = "no_call", -1;
        }
        if (call->leg) /* the phone asked again (a new network, say): the new leg takes over */
            close_leg(cs, call->leg);
        leg->deadline = now_of(cs) + cs->connect_ms;
        call->leg = leg;
        cs->version++;
    }
    *answer_out = strdup(sdp.c_str());
    *udp_out = our_udp(cs);
    if (!*answer_out)
        return *code = "internal", -1;
    puddle_log(PUDDLE_LOG_INFO, "calls: %s: the phone %.12s… set up its leg", id, device);
    return 0;
}

void callsx::write_device(puddle_ctx *ctx, puddle_jw *w, const std::string &device)
{
    puddle_jw_key(w, "device");
    puddle_device dv;
    if (device.empty()) {
        puddle_jw_null(w);
    } else if (ctx->devices && puddle_devices_get(ctx->devices, device.c_str(), &dv) == 0) {
        puddle_jw_object(w);
        puddle_jw_str(w, "id", dv.id);
        puddle_jw_str(w, "name", dv.name);
        puddle_jw_end_object(w);
    } else {
        puddle_jw_object(w);
        puddle_jw_str(w, "id", device.c_str());
        puddle_jw_str(w, "name", "");
        puddle_jw_end_object(w);
    }
}

static void write_call(puddle_ctx *ctx, puddle_jw *w, const Call &c)
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
    puddle_jw_flag(w, "media", c.open && (c.device.empty() || (c.leg && c.leg->open && c.leg->connected)));
    write_device(ctx, w, c.device);
    puddle_jw_key(w, "group");
    puddle_jw_null(w);
    puddle_jw_key(w, "members");
    puddle_jw_array(w);
    puddle_jw_end_array(w);
    puddle_jw_num(w, "created", c.created);
    puddle_jw_key(w, "answered");
    c.answered ? puddle_jw_int(w, c.answered) : puddle_jw_null(w);
    puddle_jw_key(w, "ended");
    c.ended ? puddle_jw_int(w, c.ended) : puddle_jw_null(w);
    puddle_jw_end_object(w);
}

void callsx::write_record(puddle_jw *w, const Record &r)
{
    puddle_jw_object(w);
    puddle_jw_str(w, "id", r.id.c_str());
    puddle_jw_str(w, "user", r.peer.c_str());
    puddle_jw_str(w, "direction", r.outgoing ? "outgoing" : "incoming");
    puddle_jw_str(w, "state", "ended");
    puddle_jw_str(w, "reason", r.reason.c_str());
    puddle_jw_flag(w, "media", 0);
    puddle_jw_key(w, "device");
    puddle_jw_null(w);
    puddle_jw_key(w, "group");
    r.group.empty() ? puddle_jw_null(w) : puddle_jw_string(w, r.group.c_str());
    puddle_jw_key(w, "members");
    puddle_jw_array(w);
    puddle_jw_end_array(w);
    puddle_jw_num(w, "created", r.created);
    puddle_jw_key(w, "answered");
    r.answered ? puddle_jw_int(w, r.answered) : puddle_jw_null(w);
    puddle_jw_num(w, "ended", r.ended);
    puddle_jw_end_object(w);
}

int puddle_calls_write(puddle_ctx *ctx, const char *id, const char *user, const char *group, int limit, puddle_jw *w)
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
            return write_call(ctx, w, *cs->current), 0;
        if (group_write_one(ctx, w, id) == 0)
            return 0;
        for (auto it = cs->history.rbegin(); it != cs->history.rend(); ++it)
            if (it->id == id)
                return write_record(w, *it), 0;
        return 1;
    }
    puddle_jw_object(w);
    puddle_jw_num(w, "version", (int64_t)cs->version.load());
    puddle_jw_key(w, "items");
    puddle_jw_array(w);
    if (cs->current && !group && (!user || cs->current->peer == user))
        write_call(ctx, w, *cs->current);
    group_write_live(ctx, w, user, group);
    int n = 0;
    for (auto it = cs->history.rbegin(); it != cs->history.rend() && n < limit && w->len + 600 < w->cap; ++it)
        if (group ? it->group == group : (!user || (it->group.empty() && it->peer == user)))
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
    if (int r = group_send_audio(cs, id, frame, len); r != 1)
        return r;
    std::shared_ptr<rtc::Track> track;
    uint32_t ts;
    {
        std::lock_guard<std::mutex> g(cs->mu);
        CallPtr call = cs->current;
        if (!call || call->id != id || call->state != "active" || !call->open || !call->track || !call->device.empty())
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

void puddle_calls_on_video(puddle_calls *cs, puddle_call_frame_fn fn, void *user)
{
    if (!cs)
        return;
    std::lock_guard<std::mutex> g(cs->mu);
    cs->on_video = fn, cs->on_video_user = user;
}

/* A frame of the owner's camera, or of the screen they share (`screen`), on the active call */
static int send_video(puddle_calls *cs, const char *id, const unsigned char *frame, size_t len, bool screen)
{
    if (!cs || !frame || !len || len > PUDDLE_CALL_VIDEO_MAX)
        return -1;
    if (int r = group_send_video(cs, id, frame, len, screen); r != 1)
        return r;
    std::shared_ptr<rtc::Track> track;
    uint32_t ts;
    {
        std::lock_guard<std::mutex> g(cs->mu);
        CallPtr call = cs->current;
        if (!call || call->id != id || call->state != "active" || !call->device.empty() || !track_open(screen ? call->strack : call->vtrack))
            return -1;
        track = screen ? call->strack : call->vtrack;
        ts = (screen ? call->sbase : call->vbase) + video_now();
    }
    try {
        track->sendFrame((const rtc::byte *)frame, len, rtc::FrameInfo(ts));
    } catch (const std::exception &) {
        return -1;
    }
    return 0;
}

int puddle_call_send_video(puddle_calls *cs, const char *id, const unsigned char *frame, size_t len)
{
    return send_video(cs, id, frame, len, false);
}

int puddle_call_send_screen(puddle_calls *cs, const char *id, const unsigned char *frame, size_t len)
{
    return send_video(cs, id, frame, len, true);
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

void puddle_calls_set_beat(puddle_calls *cs, int64_t beat_ms)
{
    if (!cs || beat_ms <= 0)
        return;
    std::lock_guard<std::mutex> g(cs->mu);
    cs->beat_ms = beat_ms;
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
        if (puddle_json_member(b, c->body_len, "udp", &v) != 0 || puddle_json_int64(v, &p) != 0 || p < 0 || p > 65535)
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
        if (group_busy(cs) || in_voice(cs)) /* in a voice channel too: the caller hears "already in a call" */
            return puddle_fail(w, 409, "busy");
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
    { "POST", "/stream/v1/call/group/join",   PUDDLE_AUTH_PEER, 1024, callsx::group_join_post, NULL },
    { "POST", "/stream/v1/call/group/leave",  PUDDLE_AUTH_PEER, 1024, callsx::group_leave_post, NULL },
    { "POST", "/stream/v1/call/group/offer",  PUDDLE_AUTH_PEER, PUDDLE_CALL_SDP_MAX + 1024, callsx::group_offer_post, NULL },
    { "POST", "/stream/v1/call/group/answer", PUDDLE_AUTH_PEER, PUDDLE_CALL_SDP_MAX + 1024, callsx::group_answer_post, NULL },
};
const size_t puddle_call_routes_count = sizeof puddle_call_routes / sizeof puddle_call_routes[0];

#endif
