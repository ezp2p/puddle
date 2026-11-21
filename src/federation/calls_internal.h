/* What calls.cpp (one to one) and groupcalls.cpp (group calls) share: the calls' records, the state they live in, and the
 * helpers both use. Only built with calls (PUDDLE_HAVE_CALLS); not for anything outside federation/calls*.cpp. */
#ifndef PUDDLE_CALLS_INTERNAL_H
#define PUDDLE_CALLS_INTERNAL_H

#include "federation/calls.h"

#include <rtc/rtc.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define OPUS_PT 111
#define H264_PT 96
#define MID "audio"
#define VIDEO_MID "video"
#define SCREEN_MID "screen" /* a shared screen: a second video track (docs/calls.md, "Sharing a screen") */
#define POND_WATCH 16 /* a Pond voice channel: cameras and screens we watch at once, on v1 .. v16 (the Pond's docs/voice.md) */
#define PHONE_SLOTS 7 /* a phone's leg has a video track for each other member a group call can have: v1 .. v7 */
#define LOST_MS 10000 /* a connection that dropped and hasn't come back in this long is over */

namespace callsx {

/* A phone's own leg to this Puddle (docs/calls.md, "On a phone"): the phone offers, we answer, and Opus goes between it and
 * the friend's connection. In a group call the members' frames reach the phone over a data channel instead, each marked
 * with who it is from ("Group calls on a phone"). */
struct Leg {
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> track;
    std::shared_ptr<rtc::DataChannel> dc;
    std::shared_ptr<rtc::Track> vtrack;                      /* the phone's camera, and the friend's video back to it */
    std::shared_ptr<rtc::Track> vslots[PHONE_SLOTS + 1];     /* in a group call: member n's video to the phone ("v<n>") */
    uint32_t vbase[PHONE_SLOTS + 1] = {};                    /* each one's RTP timestamps, moved by this much */
    bool open = false, connected = false, dc_open = false;
    int64_t deadline = 0, lost_since = 0;
    uint32_t next_ts = 0;  /* our next RTP timestamp to the phone */
    uint32_t last_in = 0;  /* the friend's last timestamp, to keep their gaps */
    bool have_in = false;
};

using LegPtr = std::shared_ptr<Leg>;

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
    uint32_t last_in = 0;   /* the phone's last timestamp, to keep its gaps */
    bool have_in = false;
    std::string device;     /* who has the call: "" the owner's device in this process (the window), else a phone's device id */
    LegPtr leg;             /* that phone's leg, once it has asked for one */
    std::shared_ptr<rtc::Track> vtrack; /* the video, beside the sound */
    uint32_t vbase = 0;     /* our video's RTP timestamps are this plus the time */
    std::shared_ptr<rtc::Track> strack; /* a shared screen, beside that */
    uint32_t sbase = 0;
};

using CallPtr = std::shared_ptr<Call>;

/* Our connection with one other member in a group call (docs/calls.md, "Group calls"). */
struct Pair {
    std::string peer;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> track;
    bool offerer = false;   /* ours is the smaller id: we offered */
    bool open = false, connected = false;
    int64_t started = 0, lost_since = 0;
    uint32_t next_ts = 0;   /* the RTP timestamp of our next frame to them */
    uint32_t last_in = 0;   /* the phone's last timestamp, to keep its gaps */
    bool have_in = false;
    std::shared_ptr<rtc::Track> vtrack;
    uint32_t vbase = 0;
    std::shared_ptr<rtc::Track> strack; /* a shared screen */
    uint32_t sbase = 0;
};

using PairPtr = std::shared_ptr<Pair>;

/* A call in a group: one we are in (puddle_calls.group), or one going on there that we are not in (puddle_calls.heard). */
struct GroupCall {
    std::string id, group, starter;
    bool outgoing = false;          /* we started it */
    bool joined = false;            /* we are in it */
    bool rang = false, was_in = false, declined = false;
    std::string state, reason;      /* ringing, open (not ours); calling, connecting, active (ours); ended */
    int64_t created = 0, answered = 0, ended = 0, ring_until = 0, alone_since = 0, beat_at = 0;
    std::map<std::string, int64_t> in;      /* the others in it, and when we last heard so */
    std::map<std::string, PairPtr> pairs;   /* while we are in it: our connection with each */
    std::map<std::string, int> slots;       /* each member's number in what goes to a phone */
    std::string device;                     /* as Call's */
    LegPtr leg;
};

using GroupPtr = std::shared_ptr<GroupCall>;

/* Our connection to a Pond's voice channel (docs/ponds.md, "Voice channels"; the Pond's docs/voice.md). Not a call: nothing
 * rings and nothing goes in the history. We offer, the Pond answers; our voice goes up `track`, everyone else's comes back over
 * `dc` (`frames`), each marked with its sender's slot in the channel. */
struct PondVoice {
    std::string pond, session;
    int64_t channel = 0;
    int slot = 0;
    std::string state;      /* connecting, active, ended */
    std::string reason;     /* ended: why ("failed": the connection never came or dropped; "removed": the Pond let us go) */
    std::mutex state_mu;    /* one state to the Pond at a time, each built as it goes, so the last it hears is the newest */
    bool muted = false, deafened = false, camera = false, screen = false;
    std::shared_ptr<rtc::PeerConnection> pc;
    std::shared_ptr<rtc::Track> track;
    std::shared_ptr<rtc::DataChannel> dc;
    std::shared_ptr<rtc::Track> vtrack, strack;      /* our camera, and the screen we share, to the Pond */
    uint32_t vbase = 0, sbase = 0;
    std::shared_ptr<rtc::Track> watch[POND_WATCH + 1]; /* v1 .. v16: what we watch, from the Pond */
    std::string watching[POND_WATCH + 1];            /* on each, as the Pond has it: a member's id (camera), "<id>/screen", or "" */
    bool watch_busy = false;                         /* telling the Pond what to change */
    bool open = false;      /* the track: our voice can go */
    int64_t started = 0, lost_since = 0;
    uint32_t next_ts = 0;   /* the RTP timestamp of our next frame */
};
using VoicePtr = std::shared_ptr<PondVoice>;

/* An ended call, as the owner's history keeps it (<data_dir>/state/calls.jsonl, one per line, oldest first). */
struct Record {
    std::string id, peer, reason, group;
    bool outgoing = false;
    int64_t created = 0, answered = 0, ended = 0;
};

} // namespace callsx

struct puddle_calls {
    puddle_ctx *ctx;
    puddle_outbox_hooks hooks; /* the outbox's, with shorter timeouts: a call is now or never */
    uint16_t port;
    std::string bind;
    char me[PUDDLE_USER_ID_LEN + 1] = "";
    std::mutex mu;
    callsx::CallPtr current;                 /* the call with a friend going on, if any */
    callsx::GroupPtr group;                  /* or the group call we are in */
    std::map<std::string, callsx::GroupPtr> heard; /* calls going on in our groups that we are not in, by group */
    callsx::VoicePtr voice;                  /* or the Pond voice channel we are in (or were, when it ended without us leaving) */
    std::deque<callsx::CallPtr> ended;       /* newest first: their connections, until the worker lets them go */
    std::deque<callsx::GroupPtr> ended_groups;
    std::deque<callsx::Record> history;      /* every ended call kept, oldest first (PUDDLE_CALL_HISTORY) */
    std::string history_path, state_dir;
    std::deque<std::function<void()>> tasks; /* for the worker: telling the friend a call ended */
    std::deque<std::function<void()>> gtasks; /* for the group pool: a group call's requests, which may each take a while */
    std::condition_variable wake, gwake;
    bool stopping = false;
    std::thread worker;
    std::vector<std::thread> gpool;
    puddle_call_frame_fn on_audio = nullptr, on_video = nullptr;
    void *on_audio_user = nullptr, *on_video_user = nullptr;
    std::atomic<uint64_t> version{1};
    int64_t ring_ms = PUDDLE_CALL_RING_MS, ring_in_ms = PUDDLE_CALL_RING_IN_MS, connect_ms = PUDDLE_CALL_CONNECT_MS;
    int64_t beat_ms = PUDDLE_GROUP_CALL_BEAT_MS;
    uint64_t seq = 0;                        /* the last `seq` we put on a join or a leave: only ever goes up */
    std::map<std::string, uint64_t> seen;    /* "group/member": the last `seq` they sent, older ones are stale */
};

namespace callsx {

int64_t now_of(puddle_calls *cs);
std::string new_call_id();
uint32_t random32();
bool call_id_ok(const char *s);
/* A signed request to a peer's Puddle, checked by its receipt (federation/sender.h). true if they answered in their name. */
bool tell(puddle_calls *cs, const std::string &peer, const char *path, std::string body, int *status = nullptr,
          std::string *answer = nullptr);
std::string error_of(const std::string &json);
void remember(puddle_calls *cs, const Record &r); /* lock held */
std::shared_ptr<rtc::RtpPacketizationConfig> opus_handler(const std::shared_ptr<rtc::Track> &track);
/* H.264 out, packetized (FU-A) with sender reports, and in, as whole access units with long start codes */
void h264_handler(const std::shared_ptr<rtc::Track> &track);
rtc::Description::Video video_media(const char *mid, rtc::Description::Direction dir);
uint32_t video_now(); /* the time in a 90 kHz count, for our own video's timestamps */
bool track_open(const std::shared_ptr<rtc::Track> &t);
uint32_t stamp(uint32_t &next, uint32_t &last, bool &have, uint32_t in);
void send_on(const std::shared_ptr<rtc::Track> &track, const rtc::binary &data, uint32_t ts);
rtc::Configuration pc_config(puddle_calls *cs);
bool gathered(const std::shared_ptr<rtc::PeerConnection> &pc, puddle_calls *cs, const std::function<bool()> &has_track);
/* The answerer's side, once its offer is set: when every track it offered has been set up here (`seen` counts them, so each
 * says which SSRC we send it from: two tracks in one connection need that, or what arrives can't be told apart), make the
 * answer, and wait for it to be complete. The answer, or "". */
std::string answer_when_ready(const std::shared_ptr<rtc::PeerConnection> &pc, puddle_calls *cs, const std::string &offer,
                              const std::shared_ptr<std::atomic<int>> &seen);
void add_their_addresses(puddle_calls *cs, const std::shared_ptr<rtc::PeerConnection> &pc, const std::string &peer, int udp);
/* The UDP port our offers and answers name: ours, or 0 behind a Levee, whose relay our candidates are at (spec/08) */
int our_udp(puddle_calls *cs);
bool phone_gone(puddle_calls *cs, const std::string &device, int64_t now);
void close_leg(puddle_calls *cs, const LegPtr &leg); /* lock held: closed by the worker */
/* A phone's leg for a call: `mine` (lock held) says whether it is still the call's leg; `fail` ends what it is for; `frame`
 * takes what the phone sends. The answer to its offer, or "" (then `code`). */
std::string answer_leg(puddle_calls *cs, const LegPtr &leg, const std::string &offer, std::function<bool(const LegPtr &)> mine,
                       std::function<void()> fail, std::function<void(const rtc::binary &, uint32_t)> frame,
                       std::function<void(const rtc::binary &, uint32_t)> vframe);
void write_device(puddle_ctx *ctx, puddle_jw *w, const std::string &device);

/* ---- pondvoice.cpp ---- */
bool in_voice(puddle_calls *cs);                      /* lock held: in a Pond's voice channel (one that hasn't ended) */
void voice_tick(puddle_calls *cs, int64_t now);       /* from the worker, lock not held */
void voice_stop(puddle_calls *cs);                    /* leaving, before the worker stops: the Pond is told */

/* ---- groupcalls.cpp ---- */
void group_tick(puddle_calls *cs, int64_t now);       /* from the worker, lock not held */
void group_stop(puddle_calls *cs);                    /* leaving, before the worker stops */
void group_release(puddle_calls *cs);                 /* after: closes every connection left */
bool group_busy(puddle_calls *cs);                    /* lock held: in a group call */
int group_start(puddle_ctx *ctx, const char *gid, const char *device, char id_out[PUDDLE_CALL_ID_LEN + 1], const char **code);
/* For an id that is a group call's: 0 done, -1 failed (`code`), 1 not a group call's (try the friend's). */
int group_accept(puddle_ctx *ctx, const char *id, const char *device, const char **code);
int group_decline(puddle_ctx *ctx, const char *id, const char **code);
int group_end(puddle_ctx *ctx, const char *id, const char **code);
int group_media(puddle_ctx *ctx, const char *id, const char *device, const char *sdp, char **answer, int *udp, const char **code);
int group_send_audio(puddle_calls *cs, const char *id, const unsigned char *frame, size_t len); /* 1: not a group call's */
int group_send_video(puddle_calls *cs, const char *id, const unsigned char *frame, size_t len, bool screen = false); /* 1: not a group call's */
/* lock held. write_call for a group call; the joined one and those heard of (first), in a list */
void write_group(puddle_ctx *ctx, puddle_jw *w, const GroupCall &g);
int group_write_one(puddle_ctx *ctx, puddle_jw *w, const char *id); /* 0 written, 1 none */
int group_write_live(puddle_ctx *ctx, puddle_jw *w, const char *user, const char *group);
void write_record(puddle_jw *w, const Record &r);
int group_join_post(const puddle_call *c, puddle_jw *w);
int group_leave_post(const puddle_call *c, puddle_jw *w);
int group_offer_post(const puddle_call *c, puddle_jw *w);
int group_answer_post(const puddle_call *c, puddle_jw *w);

} // namespace callsx

#endif
