/* Voice channels in Ponds (docs/ponds.md, "Voice channels"; the Pond's docs/voice.md). See federation/calls.h.
 *
 * We offer a WebRTC connection to the Pond, on our calls port: one Opus track for the owner's voice, and a data channel
 * (`frames`) on which the Pond sends everyone else's, each as its sender's slot (2 bytes), its RTP timestamp (4) and the frame.
 * The Pond answers with its own, and its UDP port; we add its address there as a candidate, since it is the one with a public
 * address. Whose each slot is comes from the Pond's `voice` events, kept by core/ponds. */
#include "federation/calls.h"

#ifdef PUDDLE_HAVE_CALLS

#include "federation/calls_internal.h"
#include "base/log.h"
#include "core/ponds.h"
#include "federation/ponds.h"

#include <algorithm>
#include <map>
#include <stdlib.h>
#include <string.h>

using namespace callsx;

bool callsx::in_voice(puddle_calls *cs)
{
    return cs->voice && cs->voice->state != "ended";
}

/* Close `v`'s connection on the worker, and tell the Pond it left (if it ever got a session). Lock held. */
static void retire(puddle_calls *cs, const VoicePtr &v, bool tell)
{
    if (v->state != "ended")
        v->state = "ended";
    v->open = false;
    auto pc = v->pc;
    v->pc.reset();
    std::string pond = v->pond, session = v->session;
    puddle_ctx *ctx = cs->ctx;
    cs->tasks.push_back([ctx, pc, pond, session, tell] {
        if (pc) {
            pc->resetCallbacks();
            pc->close();
        }
        if (tell && !session.empty()) {
            int status;
            char *reply = nullptr;
            size_t len;
            const char *code;
            if (puddle_pond_voice_call(ctx, pond.c_str(), ("/pond/v1/voice/" + session + "/leave").c_str(), "", 0, &status, &reply, &len, &code) == 0)
                free(reply);
        }
    });
    cs->wake.notify_all();
    cs->version++;
}

/* It ended without the owner leaving: kept, with why, so the window can say so, until the owner leaves or joins again. Lock
 * held. */
static void end_voice(puddle_calls *cs, const VoicePtr &v, const char *reason)
{
    if (v->state == "ended")
        return;
    puddle_log(PUDDLE_LOG_INFO, "voice: out of channel %lld of pond %.12s… (%s)", (long long)v->channel, v->pond.c_str(), reason);
    v->reason = reason;
    retire(cs, v, true);
}

/* A frame the Pond forwarded: [slot][ts][opus], to the owner's device as the member's */
static void voice_frame(puddle_calls *cs, const std::weak_ptr<PondVoice> &weak, const rtc::binary &b)
{
    if (b.size() < 7 || b.size() > 6 + PUDDLE_CALL_FRAME_MAX)
        return;
    int slot = (int)b[0] << 8 | (int)b[1];
    uint32_t ts = (uint32_t)b[2] << 24 | (uint32_t)b[3] << 16 | (uint32_t)b[4] << 8 | (uint32_t)b[5];
    std::string pond;
    int64_t channel;
    puddle_call_frame_fn fn;
    void *user;
    {
        std::lock_guard<std::mutex> g(cs->mu);
        VoicePtr v = weak.lock();
        if (!v || v->state == "ended" || v != cs->voice || v->deafened || !cs->on_audio)
            return;
        pond = v->pond, channel = v->channel;
        fn = cs->on_audio, user = cs->on_audio_user;
    }
    /* whose: as the Pond said in its events; one we haven't heard of yet is its slot, until we have */
    char from[PUDDLE_USER_ID_LEN + 16];
    if (!cs->ctx->ponds || puddle_ponds_voice_member(cs->ctx->ponds, pond.c_str(), channel, slot, from) != 0)
        snprintf(from, sizeof from, "slot:%d", slot);
    fn(user, "voice", from, (const unsigned char *)b.data() + 6, b.size() - 6, ts);
}

/* A camera or screen we watch, on track `n`: to the owner's device, as the member's (or "<id>/screen") */
static void voice_video(puddle_calls *cs, const std::weak_ptr<PondVoice> &weak, int n, const rtc::binary &au, uint32_t ts)
{
    if (au.empty() || au.size() > PUDDLE_CALL_VIDEO_MAX)
        return;
    std::string from;
    puddle_call_frame_fn fn;
    void *user;
    {
        std::lock_guard<std::mutex> g(cs->mu);
        VoicePtr v = weak.lock();
        if (!v || v->state == "ended" || v != cs->voice || !cs->on_video || v->watching[n].empty())
            return;
        from = v->watching[n];
        fn = cs->on_video, user = cs->on_video_user;
    }
    fn(user, "voice", from.c_str(), (const unsigned char *)au.data(), au.size(), ts);
}

/* The JSON of a request with an SDP in it */
static std::string offer_body(const std::string &sdp, unsigned udp)
{
    std::string buf(sdp.size() * 2 + 128, '\0');
    puddle_jw w;
    puddle_jw_init(&w, buf.data(), buf.size());
    puddle_jw_object(&w);
    puddle_jw_str(&w, "sdp", sdp.c_str());
    puddle_jw_num(&w, "udp", udp);
    puddle_jw_end_object(&w);
    buf.resize(puddle_jw_finish(&w));
    return buf;
}

/* Our side of the connection, and the Pond's answer to it. 0, or -1 with `code`. */
static int connect_voice(puddle_calls *cs, const VoicePtr &v, const char **code)
{
    std::weak_ptr<PondVoice> weak = v;
    std::shared_ptr<rtc::PeerConnection> pc;
    try {
        pc = std::make_shared<rtc::PeerConnection>(pc_config(cs));
        pc->onStateChange([cs, weak](rtc::PeerConnection::State s) {
            std::lock_guard<std::mutex> g(cs->mu);
            VoicePtr v = weak.lock();
            if (!v || v->state == "ended")
                return;
            if (s == rtc::PeerConnection::State::Connected) {
                v->lost_since = 0;
                if (v->state != "active")
                    puddle_log(PUDDLE_LOG_INFO, "voice: in channel %lld of pond %.12s…", (long long)v->channel, v->pond.c_str());
                v->state = "active";
            } else if (!v->lost_since) { /* disconnected, failed or closed: it may come back; the worker lets it go if not */
                v->lost_since = now_of(cs);
            }
            cs->version++;
        });
        rtc::Description::Audio audio("audio", rtc::Description::Direction::SendOnly);
        audio.addOpusCodec(111);
        auto track = pc->addTrack(audio);
        opus_handler(track);
        track->onOpen([cs, weak] {
            std::lock_guard<std::mutex> g(cs->mu);
            if (VoicePtr v = weak.lock())
                v->open = true;
        });
        rtc::DataChannelInit init;
        init.reliability.unordered = true;
        init.reliability.maxRetransmits = 0; /* a frame late is a frame lost: the jitter buffer covers it */
        /* our camera and our screen (sent while on), and v1 .. v16, what we watch of everyone else's */
        auto vt = pc->addTrack(video_media(VIDEO_MID, rtc::Description::Direction::SendOnly));
        h264_handler(vt);
        auto st = pc->addTrack(video_media(SCREEN_MID, rtc::Description::Direction::SendOnly));
        h264_handler(st);
        std::shared_ptr<rtc::Track> watch[POND_WATCH + 1];
        for (int n = 1; n <= POND_WATCH; n++) {
            const std::string mid = "v" + std::to_string(n);
            watch[n] = pc->addTrack(video_media(mid.c_str(), rtc::Description::Direction::RecvOnly));
            h264_handler(watch[n]);
            watch[n]->onFrame([cs, weak, n](rtc::binary data, rtc::FrameInfo info) { voice_video(cs, weak, n, data, info.timestamp); });
        }
        auto dc = pc->createDataChannel("frames", init);
        dc->onMessage([cs, weak](rtc::message_variant m) {
            if (auto *b = std::get_if<rtc::binary>(&m))
                voice_frame(cs, weak, *b);
        });
        {
            std::lock_guard<std::mutex> g(cs->mu);
            v->pc = pc, v->track = track, v->dc = dc, v->vtrack = vt, v->strack = st;
            v->vbase = random32(), v->sbase = random32();
            for (int n = 1; n <= POND_WATCH; n++)
                v->watch[n] = watch[n];
        }
        pc->setLocalDescription();
        if (!gathered(pc, cs, [] { return true; }))
            throw std::runtime_error("no description");
    } catch (const std::exception &e) {
        puddle_log(PUDDLE_LOG_WARN, "voice: can't make a connection: %s", e.what());
        return *code = "failed", -1;
    }

    std::string target = "/pond/v1/channels/" + std::to_string(v->channel) + "/voice/join";
    std::string body = offer_body(std::string(*pc->localDescription()), cs->port);
    int status = 0;
    char *reply = nullptr;
    size_t len = 0;
    if (puddle_pond_voice_call(cs->ctx, v->pond.c_str(), target.c_str(), body.data(), body.size(), &status, &reply, &len, code) != 0)
        return -1;
    std::string answer(reply, len);
    free(reply);
    if (status != 200) {
        std::string e = error_of(answer);
        *code = status == 404 ? "no_channel" : e == "full" ? "full" : "failed";
        return -1;
    }
    puddle_span all = {answer.data(), answer.size()}, f;
    std::string sdp(answer.size() + 1, '\0'), session(64, '\0');
    int64_t udp = 0, slot = 0;
    if (puddle_json_member(all.p, all.len, "sdp", &f) != 0 || puddle_json_string(f, sdp.data(), sdp.size()) != 0 ||
        puddle_json_member(all.p, all.len, "session", &f) != 0 || puddle_json_string(f, session.data(), session.size()) != 0 ||
        puddle_json_member(all.p, all.len, "udp", &f) != 0 || puddle_json_int64(f, &udp) != 0 || udp <= 0 || udp > 65535 ||
        puddle_json_member(all.p, all.len, "slot", &f) != 0 || puddle_json_int64(f, &slot) != 0)
        return *code = "failed", -1;
    sdp.resize(strlen(sdp.c_str()));
    session.resize(strlen(session.c_str()));
    {
        std::lock_guard<std::mutex> g(cs->mu);
        v->session = session, v->slot = (int)slot;
    }
    try {
        pc->setRemoteDescription(rtc::Description(sdp, "answer"));
        /* the Pond's own address, at its voice port: it has a public address, so this is the path (no STUN, no TURN) */
        puddle_pond_conn conn;
        if (puddle_ponds_conn(cs->ctx->ponds, v->pond.c_str(), &conn) == 0)
            pc->addRemoteCandidate(rtc::Candidate("a=candidate:9 1 UDP 2122000000 " + std::string(conn.address.host) + " " +
                                                  std::to_string(udp) + " typ host", "audio"));
    } catch (const std::exception &e) {
        puddle_log(PUDDLE_LOG_WARN, "voice: the Pond's answer can't be used: %s", e.what());
        return *code = "failed", -1;
    }
    return 0;
}

int puddle_voice_join(puddle_ctx *ctx, const char *pond, int64_t channel, const char **code)
{
    puddle_calls *cs = ctx->calls;
    if (!cs)
        return *code = "no_calls", -1;
    if (!ctx->ponds || !pond || puddle_ponds_conn(ctx->ponds, pond, NULL) != 0)
        return *code = "no_pond", -1;
    int kind = puddle_ponds_channel_kind(ctx->ponds, pond, channel);
    if (kind < 0)
        return *code = "no_channel", -1;
    if (kind == 0 || !puddle_ponds_has_feature(ctx->ponds, pond, "voice"))
        return *code = "not_voice", -1;
    auto v = std::make_shared<PondVoice>();
    v->pond = pond, v->channel = channel, v->state = "connecting";
    v->started = now_of(cs);
    v->next_ts = random32();
    {
        std::lock_guard<std::mutex> g(cs->mu);
        if (cs->stopping)
            return *code = "failed", -1;
        if (cs->current || group_busy(cs))
            return *code = "busy", -1;
        if (in_voice(cs) && cs->voice->pond == pond && cs->voice->channel == channel)
            return 0; /* there already */
        if (cs->voice) {
            v->muted = cs->voice->muted, v->deafened = cs->voice->deafened; /* moving to another channel keeps them */
            retire(cs, cs->voice, cs->voice->state != "ended");
        }
        cs->voice = v;
        cs->version++;
    }
    if (connect_voice(cs, v, code) == 0) {
        if (v->muted || v->deafened) { /* the Pond starts everyone unmuted, with nothing on; moving keeps mute and deafen */
            const char *ignored;
            puddle_voice_state(ctx, v->muted, v->deafened, -1, -1, &ignored);
        }
        return 0;
    }
    std::lock_guard<std::mutex> g(cs->mu);
    if (cs->voice == v) {
        cs->voice.reset();
        retire(cs, v, true);
    }
    return -1;
}

int puddle_voice_leave(puddle_ctx *ctx, const char **)
{
    puddle_calls *cs = ctx->calls;
    if (!cs)
        return 0;
    std::lock_guard<std::mutex> g(cs->mu);
    if (VoicePtr v = cs->voice) {
        cs->voice.reset();
        retire(cs, v, v->state != "ended");
    }
    return 0;
}

int puddle_voice_state(puddle_ctx *ctx, int muted, int deafened, int camera, int screen, const char **code)
{
    puddle_calls *cs = ctx->calls;
    if (!cs)
        return *code = "no_voice", -1;
    std::string pond, session;
    std::string body = "{";
    {
        std::lock_guard<std::mutex> g(cs->mu);
        VoicePtr v = cs->voice;
        if (!v || v->state == "ended")
            return *code = "no_voice", -1;
        if (muted >= 0)
            v->muted = muted;
        if (deafened >= 0)
            v->deafened = deafened;
        if (camera >= 0)
            v->camera = camera;
        if (screen >= 0)
            v->screen = screen;
        pond = v->pond, session = v->session;
        body += std::string("\"muted\":") + (v->muted ? "true" : "false") + ",\"deafened\":" + (v->deafened ? "true" : "false") +
                ",\"camera\":" + (v->camera ? "true" : "false") + ",\"screen\":" + (v->screen ? "true" : "false") + "}";
        cs->version++;
    }
    if (session.empty())
        return 0; /* still joining: join tells the Pond once it has a session */
    int status;
    char *reply = nullptr;
    size_t len;
    if (puddle_pond_voice_call(ctx, pond.c_str(), ("/pond/v1/voice/" + session + "/state").c_str(), body.data(), body.size(), &status, &reply,
                               &len, code) != 0)
        return -1;
    free(reply);
    if (status == 404) { /* the Pond doesn't have us in voice any more */
        std::lock_guard<std::mutex> g(cs->mu);
        if (cs->voice && cs->voice->session == session)
            end_voice(cs, cs->voice, "removed");
        return *code = "no_voice", -1;
    }
    return 0;
}

void puddle_voice_write(puddle_ctx *ctx, puddle_jw *w)
{
    puddle_calls *cs = ctx->calls;
    if (!cs) {
        puddle_jw_null(w);
        return;
    }
    std::lock_guard<std::mutex> g(cs->mu);
    VoicePtr v = cs->voice;
    if (!v) {
        puddle_jw_null(w);
        return;
    }
    puddle_jw_object(w);
    puddle_jw_str(w, "pond", v->pond.c_str());
    puddle_jw_num(w, "channel", v->channel);
    puddle_jw_str(w, "state", v->state.c_str());
    if (!v->reason.empty())
        puddle_jw_str(w, "reason", v->reason.c_str());
    puddle_jw_num(w, "slot", v->slot);
    puddle_jw_flag(w, "muted", v->muted);
    puddle_jw_flag(w, "deafened", v->deafened);
    puddle_jw_flag(w, "camera", v->camera);
    puddle_jw_flag(w, "screen", v->screen);
    puddle_jw_end_object(w);
}

int puddle_voice_send_audio(puddle_calls *cs, const unsigned char *frame, size_t len)
{
    if (!cs || !frame || !len || len > PUDDLE_CALL_FRAME_MAX)
        return -1;
    std::shared_ptr<rtc::Track> track;
    uint32_t ts;
    {
        std::lock_guard<std::mutex> g(cs->mu);
        VoicePtr v = cs->voice;
        if (!v || v->state != "active" || v->muted || !v->open || !track_open(v->track))
            return -1;
        track = v->track;
        ts = v->next_ts;
        v->next_ts += 960;
    }
    send_on(track, rtc::binary((const std::byte *)frame, (const std::byte *)frame + len), ts);
    return 0;
}

int puddle_voice_send_video(puddle_calls *cs, const unsigned char *frame, size_t len, int screen)
{
    if (!cs || !frame || !len || len > PUDDLE_CALL_VIDEO_MAX)
        return -1;
    std::shared_ptr<rtc::Track> track;
    uint32_t ts;
    {
        std::lock_guard<std::mutex> g(cs->mu);
        VoicePtr v = cs->voice;
        if (!v || v->state != "active" || !(screen ? v->screen : v->camera) || !track_open(screen ? v->strack : v->vtrack))
            return -1;
        track = screen ? v->strack : v->vtrack;
        ts = (screen ? v->sbase : v->vbase) + video_now();
    }
    try {
        track->sendFrame((const rtc::byte *)frame, len, rtc::FrameInfo(ts));
    } catch (const std::exception &) {
        return -1;
    }
    return 0;
}

/* Everyone's camera and screen in the channel, watched: each keeps the track it was given until it goes (nothing moves), a
 * new one takes the first free track, up to POND_WATCH. The Pond is told what changed; while it is being told, nothing else. */
static void watch_everyone(puddle_calls *cs, const VoicePtr &v)
{
    std::string pond;
    int64_t channel;
    {
        std::lock_guard<std::mutex> g(cs->mu);
        if (v->state != "active" || v->watch_busy || v->session.empty())
            return;
        pond = v->pond, channel = v->channel;
    }
    puddle_pond_in_voice *list = nullptr;
    size_t n = 0;
    if (!cs->ctx->ponds || puddle_ponds_voice_list(cs->ctx->ponds, pond.c_str(), channel, &list, &n) != 0)
        return;
    std::vector<std::string> want;
    for (size_t i = 0; i < n; i++) {
        if (!strcmp(list[i].member, cs->me))
            continue;
        if (list[i].camera)
            want.push_back(list[i].member);
        if (list[i].screen)
            want.push_back(std::string(list[i].member) + "/screen");
    }
    free(list);
    std::map<int, std::string> change;
    std::string session;
    {
        std::lock_guard<std::mutex> g(cs->mu);
        if (cs->voice != v || v->state != "active" || v->watch_busy)
            return;
        std::string next[POND_WATCH + 1];
        for (int t = 1; t <= POND_WATCH; t++)
            if (std::find(want.begin(), want.end(), v->watching[t]) != want.end())
                next[t] = v->watching[t]; /* still on: stays where it is */
        for (auto &key : want) {
            if (std::find(std::begin(next), std::end(next), key) != std::end(next))
                continue;
            for (int t = 1; t <= POND_WATCH; t++)
                if (next[t].empty()) {
                    next[t] = key;
                    break;
                }
        }
        for (int t = 1; t <= POND_WATCH; t++)
            if (next[t] != v->watching[t])
                change[t] = next[t];
        if (change.empty())
            return;
        v->watch_busy = true;
        session = v->session;
    }
    std::string body = "{\"tracks\":{";
    for (auto &[t, key] : change)
        body += (body.size() > 11 ? "," : "") + std::string("\"v") + std::to_string(t) + "\":" + (key.empty() ? "null" : "\"" + key + "\"");
    body += "}}";
    std::weak_ptr<PondVoice> weak = v;
    puddle_ctx *ctx = cs->ctx;
    std::lock_guard<std::mutex> g(cs->mu);
    cs->tasks.push_back([cs, ctx, weak, pond, session, body, change] {
        int status = 0;
        char *reply = nullptr;
        size_t len;
        const char *code;
        bool ok = puddle_pond_voice_call(ctx, pond.c_str(), ("/pond/v1/voice/" + session + "/watch").c_str(), body.data(), body.size(), &status,
                                         &reply, &len, &code) == 0 &&
                  status == 204;
        free(reply);
        std::lock_guard<std::mutex> g(cs->mu);
        if (VoicePtr v = weak.lock()) {
            if (ok)
                for (auto &[t, key] : change)
                    v->watching[t] = key;
            v->watch_busy = false;
        }
    });
    cs->wake.notify_all();
}

void callsx::voice_tick(puddle_calls *cs, int64_t now)
{
    VoicePtr v;
    {
        std::lock_guard<std::mutex> g(cs->mu);
        v = cs->voice;
        if (!v || v->state == "ended")
            return;
        if (v->state == "connecting" && now - v->started >= cs->connect_ms) {
            end_voice(cs, v, "failed"); /* it never got through */
            return;
        }
        if (v->lost_since && now - v->lost_since >= LOST_MS) {
            end_voice(cs, v, "failed"); /* it dropped, and didn't come back */
            return;
        }
    }
    watch_everyone(cs, v);
}

void callsx::voice_stop(puddle_calls *cs)
{
    std::lock_guard<std::mutex> g(cs->mu);
    if (VoicePtr v = cs->voice) {
        cs->voice.reset();
        retire(cs, v, v->state != "ended");
    }
}

#endif
