/* Group calls (docs/calls.md, "Group calls"): a connection between each pair of the members in the call, over the
 * connections the group already has (a friendship, or a group-only stream). See federation/calls.h; calls.cpp has the
 * one-to-one calls and what both share (calls_internal.h). */
#include "federation/calls.h"

#ifdef PUDDLE_HAVE_CALLS

#include "federation/calls_internal.h"

#include "base/log.h"
#include "core/group.h"
#include "core/streams.h"
#include "federation/groupflow.h"

#include <algorithm>
#include <string.h>

using namespace callsx;

namespace {

/* ---- who is in the group ---- */

struct Members {
    bool in = false;                 /* we are a member */
    std::vector<std::string> others; /* the other members */
};

Members members_of(puddle_calls *cs, const std::string &gid)
{
    Members m;
    puddle_group g;
    if (!cs->ctx->groups || puddle_groups_get(cs->ctx->groups, gid.c_str(), &g) != 0)
        return m;
    m.in = g.my == PUDDLE_GROUP_IN;
    for (size_t i = 0; i < g.nmembers; i++)
        if (g.members[i].state == PUDDLE_GM_MEMBER && strcmp(g.members[i].id, cs->me) != 0)
            m.others.push_back(g.members[i].id);
    puddle_group_free(&g);
    return m;
}

/* Is `peer` a member of `gid` with us, over a connection that carries groups? */
bool member_with(puddle_calls *cs, const std::string &gid, const char *peer)
{
    Members m = members_of(cs, gid);
    return m.in && std::find(m.others.begin(), m.others.end(), peer) != m.others.end() && puddle_group_open_with(cs->ctx, peer);
}

std::string gbody(const std::string &id, const std::string &gid, const std::string *sdp = nullptr, int udp = 0, uint64_t seq = 0)
{
    std::string buf((sdp ? sdp->size() * 2 : 0) + 512, '\0');
    puddle_jw w;
    puddle_jw_init(&w, buf.data(), buf.size());
    puddle_jw_object(&w);
    puddle_jw_str(&w, "call", id.c_str());
    puddle_jw_str(&w, "group", gid.c_str());
    if (sdp) {
        puddle_jw_str(&w, "sdp", sdp->c_str());
        puddle_jw_num(&w, "udp", udp);
    }
    if (seq)
        puddle_jw_num(&w, "seq", (int64_t)seq);
    puddle_jw_end_object(&w);
    return std::string(buf.data(), puddle_jw_finish(&w));
}

bool smaller(const char *a, const char *b) { return strcmp(a, b) < 0; }

/* Lock held. A join or a leave goes out on any of the pool's threads, so they may arrive out of order; each carries a number
 * that only goes up (the clock in microseconds, or one more than the last), and the older of two is ignored. */
uint64_t next_seq(puddle_calls *cs)
{
    uint64_t t = (uint64_t)now_of(cs) * 1000;
    cs->seq = t > cs->seq ? t : cs->seq + 1;
    return cs->seq;
}

/* Lock held: is this join or leave newer than the last from `sender` in `gid`? (Remembered if so.) */
bool fresh(puddle_calls *cs, const std::string &gid, const char *sender, uint64_t seq)
{
    if (!seq)
        return true;
    uint64_t &last = cs->seen[gid + "/" + sender];
    if (seq <= last)
        return false;
    last = seq;
    return true;
}

/* ---- the pool ---- */

/* Lock held. */
void spawn(puddle_calls *cs, std::function<void()> fn)
{
    if (cs->stopping)
        return;
    cs->gtasks.push_back(std::move(fn));
    cs->gwake.notify_one();
}

/* ---- a call's state ---- */

/* Lock held. */
void restate(puddle_calls *cs, GroupCall &g, int64_t now)
{
    if (g.state == "ended")
        return;
    std::string st;
    if (!g.joined) {
        st = g.rang && !g.declined && now < g.ring_until ? "ringing" : "open";
    } else {
        bool connected = false;
        for (auto &[peer, p] : g.pairs)
            connected = connected || p->connected;
        st = connected ? "active" : g.in.empty() ? "calling" : "connecting";
        if (connected && !g.answered)
            g.answered = now;
    }
    if (st != g.state) {
        g.state = st;
        cs->version++;
    }
}

/* Lock held. Each other member's number in what goes to a phone: the smallest not held by another member in the call now, so
 * never more than PHONE_SLOTS (the phone has a video track for each). A member keeps theirs while they are in it. */
int slot_of(GroupCall &g, const std::string &peer)
{
    auto taken = [&](int s) {
        for (auto &[who, n] : g.slots)
            if (n == s && who != peer && (g.in.count(who) || g.pairs.count(who)))
                return true;
        return false;
    };
    auto it = g.slots.find(peer);
    if (it != g.slots.end() && !taken(it->second))
        return it->second;
    int s = 1;
    while (taken(s))
        s++;
    g.slots[peer] = s;
    return s;
}

/* Lock held: the connection with `peer` goes (closed by the worker: this may be in one of its own callbacks). */
void drop_pair(puddle_calls *cs, GroupCall &g, const std::string &peer, const PairPtr &only = nullptr)
{
    auto it = g.pairs.find(peer);
    if (it == g.pairs.end() || (only && it->second != only))
        return;
    PairPtr p = it->second;
    g.pairs.erase(it);
    p->open = false;
    if (auto pc = p->pc) {
        cs->tasks.push_back([pc] { pc->close(); });
        cs->wake.notify_all();
    }
    cs->version++;
}

/* ---- a pair's connection ---- */

std::shared_ptr<rtc::PeerConnection> make_pair_pc(puddle_calls *cs, const std::weak_ptr<GroupCall> &wg, const PairPtr &pair)
{
    auto pc = std::make_shared<rtc::PeerConnection>(pc_config(cs));
    std::weak_ptr<Pair> wp = pair;
    pc->onStateChange([cs, wg, wp](rtc::PeerConnection::State s) {
        std::lock_guard<std::mutex> g(cs->mu);
        GroupPtr call = wg.lock();
        PairPtr p = wp.lock();
        if (!call || !p || !call->joined)
            return;
        auto it = call->pairs.find(p->peer);
        if (it == call->pairs.end() || it->second != p)
            return; /* gone, or a newer one took its place */
        if (s == rtc::PeerConnection::State::Connected) {
            if (!p->connected)
                puddle_log(PUDDLE_LOG_INFO, "calls: %s: connected with %.12s…", call->id.c_str(), p->peer.c_str());
            p->connected = true;
            p->lost_since = 0;
            restate(cs, *call, now_of(cs));
            cs->version++;
        } else if (s == rtc::PeerConnection::State::Disconnected) {
            p->lost_since = now_of(cs);
        } else if (s == rtc::PeerConnection::State::Failed || s == rtc::PeerConnection::State::Closed) {
            puddle_log(PUDDLE_LOG_INFO, "calls: %s: lost %.12s…", call->id.c_str(), p->peer.c_str());
            drop_pair(cs, *call, p->peer, p); /* they may come back: the next time they say they are in, it is set up again */
            restate(cs, *call, now_of(cs));
        }
    });
    return pc;
}

/* The member's video, or the screen they share (`screen`): to the owner's device here (a screen as from "<them>/screen"), or
 * to the phone that has the call, on the leg's track for their number (a screen isn't passed to a phone yet). */
void setup_pair_vtrack(puddle_calls *cs, const std::weak_ptr<GroupCall> &wg, const PairPtr &pair, const std::shared_ptr<rtc::Track> &track,
                       bool screen = false)
{
    h264_handler(track);
    std::string id, peer = pair->peer, from = pair->peer + (screen ? "/screen" : "");
    {
        std::lock_guard<std::mutex> g(cs->mu);
        GroupPtr call = wg.lock();
        if (!call)
            return;
        (screen ? pair->strack : pair->vtrack) = track;
        (screen ? pair->sbase : pair->vbase) = random32();
        id = call->id;
    }
    std::weak_ptr<Pair> wp = pair;
    track->onFrame([cs, wg, wp, id, peer, from, screen](rtc::binary data, rtc::FrameInfo info) {
        if (data.empty() || data.size() > PUDDLE_CALL_VIDEO_MAX)
            return;
        puddle_call_frame_fn fn = nullptr;
        void *user = nullptr;
        std::shared_ptr<rtc::Track> phone;
        uint32_t ts = 0;
        {
            std::lock_guard<std::mutex> g(cs->mu);
            GroupPtr call = wg.lock();
            PairPtr p = wp.lock();
            if (!call || !p || !call->joined)
                return;
            auto it = call->pairs.find(peer);
            if (it == call->pairs.end() || it->second != p)
                return;
            if (call->device.empty()) {
                fn = cs->on_video, user = cs->on_video_user;
            } else if (call->leg && !screen) {
                int s = slot_of(*call, peer);
                if (s <= PHONE_SLOTS && track_open(call->leg->vslots[s])) {
                    phone = call->leg->vslots[s];
                    ts = call->leg->vbase[s] + info.timestamp;
                }
            }
        }
        if (fn)
            fn(user, id.c_str(), from.c_str(), (const unsigned char *)data.data(), data.size(), info.timestamp);
        else if (phone)
            send_on(phone, data, ts);
    });
}

/* The member's frames go to the owner's device here (the window), or to the phone that has the call, marked with whose. */
void setup_pair_track(puddle_calls *cs, const std::weak_ptr<GroupCall> &wg, const PairPtr &pair, const std::shared_ptr<rtc::Track> &track)
{
    if (track->mid() == VIDEO_MID || track->mid() == SCREEN_MID)
        return setup_pair_vtrack(cs, wg, pair, track, track->mid() == SCREEN_MID);
    if (track->mid() != MID)
        return; /* a track from a newer Puddle: left as it came */
    opus_handler(track);
    std::string id, peer = pair->peer;
    {
        std::lock_guard<std::mutex> g(cs->mu);
        GroupPtr call = wg.lock();
        if (!call)
            return;
        pair->track = track;
        id = call->id;
    }
    std::weak_ptr<Pair> wp = pair;
    track->onOpen([cs, wp] {
        std::lock_guard<std::mutex> g(cs->mu);
        if (PairPtr p = wp.lock()) {
            p->open = true;
            cs->version++;
        }
    });
    track->onFrame([cs, wg, wp, id, peer](rtc::binary data, rtc::FrameInfo info) {
        if (data.empty() || data.size() > PUDDLE_CALL_FRAME_MAX)
            return;
        puddle_call_frame_fn fn = nullptr;
        void *user = nullptr;
        std::shared_ptr<rtc::DataChannel> dc;
        int slot = 0;
        {
            std::lock_guard<std::mutex> g(cs->mu);
            GroupPtr call = wg.lock();
            PairPtr p = wp.lock();
            if (!call || !p || !call->joined)
                return;
            auto it = call->pairs.find(peer);
            if (it == call->pairs.end() || it->second != p)
                return;
            if (call->device.empty()) {
                fn = cs->on_audio, user = cs->on_audio_user;
            } else if (call->leg && call->leg->dc && call->leg->dc_open) {
                dc = call->leg->dc;
                slot = slot_of(*call, peer);
            }
        }
        if (fn) {
            fn(user, id.c_str(), peer.c_str(), (const unsigned char *)data.data(), data.size(), info.timestamp);
        } else if (dc) { /* [slot][RTP timestamp, big-endian][the frame] */
            rtc::binary msg(5 + data.size());
            uint32_t ts = info.timestamp;
            msg[0] = std::byte(slot);
            for (int i = 0; i < 4; i++)
                msg[1 + i] = std::byte((ts >> (24 - 8 * i)) & 0xff);
            std::copy(data.begin(), data.end(), msg.begin() + 5);
            try {
                dc->send(std::move(msg));
            } catch (const std::exception &) {
            }
        }
    });
}

/* In the pool: ours is the smaller id, so we offer. */
void offer_to(puddle_calls *cs, std::weak_ptr<GroupCall> wg, PairPtr pair, std::string id, std::string gid)
{
    std::string sdp;
    std::shared_ptr<rtc::PeerConnection> pc;
    try {
        pc = make_pair_pc(cs, wg, pair);
        {
            std::lock_guard<std::mutex> g(cs->mu);
            pair->pc = pc;
        }
        rtc::Description::Audio media(MID, rtc::Description::Direction::SendRecv);
        media.addOpusCodec(OPUS_PT);
        auto track = pc->addTrack(media);
        setup_pair_track(cs, wg, pair, track);
        setup_pair_track(cs, wg, pair, pc->addTrack(video_media(VIDEO_MID, rtc::Description::Direction::SendRecv)));
        setup_pair_track(cs, wg, pair, pc->addTrack(video_media(SCREEN_MID, rtc::Description::Direction::SendRecv)));
        pc->setLocalDescription();
        if (gathered(pc, cs, [&] { return pair->track != nullptr; }))
            sdp = std::string(*pc->localDescription());
    } catch (const std::exception &e) {
        puddle_log(PUDDLE_LOG_WARN, "calls: cannot set up a connection with %.12s…: %s", pair->peer.c_str(), e.what());
    }
    int status = 0;
    if (sdp.empty() || !tell(cs, pair->peer, "/stream/v1/call/group/offer", gbody(id, gid, &sdp, cs->port), &status) || status != 204) {
        std::lock_guard<std::mutex> g(cs->mu);
        if (GroupPtr call = wg.lock())
            drop_pair(cs, *call, pair->peer, pair);
    }
}

/* In the pool: they offered, we answer. */
void answer_to(puddle_calls *cs, std::weak_ptr<GroupCall> wg, PairPtr pair, std::string id, std::string gid, std::string offer, int udp)
{
    std::string sdp;
    try {
        auto pc = make_pair_pc(cs, wg, pair);
        {
            std::lock_guard<std::mutex> g(cs->mu);
            pair->pc = pc;
        }
        auto seen = std::make_shared<std::atomic<int>>(0);
        pc->onTrack([cs, wg, pair, seen](std::shared_ptr<rtc::Track> track) {
            setup_pair_track(cs, wg, pair, track);
            (*seen)++;
        });
        pc->setRemoteDescription(rtc::Description(offer, "offer"));
        add_their_addresses(cs, pc, pair->peer, udp);
        sdp = answer_when_ready(pc, cs, offer, seen);
    } catch (const std::exception &e) {
        puddle_log(PUDDLE_LOG_INFO, "calls: %.12s…'s offer can't be used: %s", pair->peer.c_str(), e.what());
    }
    int status = 0;
    if (sdp.empty() || !tell(cs, pair->peer, "/stream/v1/call/group/answer", gbody(id, gid, &sdp, cs->port), &status) || status != 204) {
        std::lock_guard<std::mutex> g(cs->mu);
        if (GroupPtr call = wg.lock())
            drop_pair(cs, *call, pair->peer, pair);
    }
}

/* Lock held: a connection with `peer`, who is in our call, if there is none and it is ours to offer. */
void ensure_pair(puddle_calls *cs, const GroupPtr &g, const std::string &peer)
{
    if (!g->joined || g->state == "ended" || !g->in.count(peer) || g->pairs.count(peer) || !smaller(cs->me, peer.c_str()))
        return;
    if ((int)g->pairs.size() >= PUDDLE_GROUP_CALL_MAX - 1)
        return;
    auto pair = std::make_shared<Pair>();
    pair->peer = peer;
    pair->offerer = true;
    pair->started = now_of(cs);
    pair->next_ts = random32();
    g->pairs[peer] = pair;
    slot_of(*g, peer);
    spawn(cs, [cs, wg = std::weak_ptr<GroupCall>(g), pair, id = g->id, gid = g->group] { offer_to(cs, wg, pair, id, gid); });
}

void move_to(puddle_calls *cs, const GroupPtr &g, const std::string &other, const std::string &from);

/* In the pool: tell one member we are in the call. They answer whether they are too. */
void join_one(puddle_calls *cs, std::weak_ptr<GroupCall> wg, std::string id, std::string gid, std::string peer, uint64_t seq)
{
    {
        std::lock_guard<std::mutex> g(cs->mu);
        GroupPtr call = wg.lock();
        if (!call || !call->joined || cs->group != call)
            return; /* left meanwhile */
    }
    int status = 0;
    std::string answer;
    if (!tell(cs, peer, "/stream/v1/call/group/join", gbody(id, gid, nullptr, 0, seq), &status, &answer) || status != 200)
        return;
    puddle_span v;
    bool in = puddle_json_member(answer.data(), answer.size(), "in", &v) == 0 && v.len == 4 && !memcmp(v.p, "true", 4);
    char other[PUDDLE_CALL_ID_LEN + 2] = "";
    if (puddle_json_member(answer.data(), answer.size(), "call", &v) == 0)
        puddle_json_string(v, other, sizeof other);
    std::lock_guard<std::mutex> g(cs->mu);
    GroupPtr call = wg.lock();
    if (!call || !call->joined || call->id != id || cs->group != call)
        return;
    if (in) {
        call->in[peer] = now_of(cs);
        ensure_pair(cs, call, peer);
        restate(cs, *call, now_of(cs));
    } else if (call_id_ok(other) && other < id) { /* two calls started at once in the group: everyone goes to the smaller id */
        move_to(cs, call, other, peer);
    }
}

/* Lock held: tell every member of the group we are in the call (in the pool: it reads the group, and asks each). */
void announce(puddle_calls *cs, const GroupPtr &g)
{
    std::weak_ptr<GroupCall> wg = g;
    spawn(cs, [cs, wg, id = g->id, gid = g->group, seq = next_seq(cs)] {
        Members m = members_of(cs, gid);
        std::lock_guard<std::mutex> lk(cs->mu);
        for (auto &peer : m.others)
            spawn(cs, [cs, wg, id, gid, peer, seq] {
                if (puddle_group_open_with(cs->ctx, peer.c_str()))
                    join_one(cs, wg, id, gid, peer, seq);
            });
    });
}

/* Lock held: we are in `g` now. */
void join_locked(puddle_calls *cs, const GroupPtr &g, const char *device)
{
    int64_t now = now_of(cs);
    g->joined = g->was_in = true;
    g->state.clear();
    g->device = device ? device : "";
    g->alone_since = g->in.empty() ? now : 0;
    g->beat_at = now;
    cs->group = g;
    restate(cs, *g, now);
    announce(cs, g);
    for (auto &[peer, when] : g->in)
        ensure_pair(cs, g, peer);
    cs->version++;
    puddle_log(PUDDLE_LOG_INFO, "calls: in %s's call %s", g->group.c_str(), g->id.c_str());
}

/* Lock held: a call heard of that is over for us, kept in the history if it rang and we never went in. */
void forget_heard(puddle_calls *cs, std::map<std::string, GroupPtr>::iterator it)
{
    GroupPtr h = it->second;
    if (h->rang && !h->was_in) {
        Record r;
        r.id = h->id, r.peer = h->starter, r.group = h->group, r.reason = h->declined ? "declined" : "missed";
        r.created = h->created, r.ended = now_of(cs);
        remember(cs, r);
    }
    h->state = "ended";
    cs->heard.erase(it);
    cs->version++;
}

/* Lock held: we leave the call we are in, and every member is told (as they were told we joined), so those not in it see at
 * once when it is over. It goes on without us while others are in it: then it is one heard of again, which we may join
 * again. `keep`: a note of it in the history. */
void leave_locked(puddle_calls *cs, const GroupPtr &g, const char *reason, bool keep = true)
{
    int64_t now = now_of(cs);
    spawn(cs, [cs, id = g->id, gid = g->group, seq = next_seq(cs)] {
        Members m = members_of(cs, gid);
        std::lock_guard<std::mutex> lk(cs->mu);
        for (auto &peer : m.others)
            spawn(cs, [cs, peer, body = gbody(id, gid, nullptr, 0, seq)] {
                if (puddle_group_open_with(cs->ctx, peer.c_str()))
                    tell(cs, peer, "/stream/v1/call/group/leave", body);
            });
    });
    std::vector<std::string> peers;
    for (auto &[peer, p] : g->pairs)
        peers.push_back(peer);
    for (auto &peer : peers)
        drop_pair(cs, *g, peer);
    if (g->leg)
        close_leg(cs, g->leg);
    g->joined = false;
    g->state = "ended";
    g->reason = reason;
    g->ended = now;
    if (cs->group == g)
        cs->group.reset();
    if (keep) {
        Record r;
        r.id = g->id, r.peer = g->starter, r.group = g->group, r.reason = reason, r.outgoing = g->outgoing;
        r.created = g->created, r.answered = g->answered, r.ended = now;
        remember(cs, r);
    }
    cs->ended_groups.push_front(g);
    while (cs->ended_groups.size() > PUDDLE_CALL_KEEP_ENDED)
        cs->ended_groups.pop_back();
    if (!g->in.empty()) {
        auto h = std::make_shared<GroupCall>();
        h->id = g->id, h->group = g->group, h->starter = g->starter, h->created = g->created;
        h->outgoing = g->outgoing; /* still the one we started, if we did */
        h->in = g->in;
        h->was_in = true;
        cs->heard[g->group] = h;
        restate(cs, *h, now);
    }
    cs->version++;
    puddle_log(PUDDLE_LOG_INFO, "calls: left %s's call %s: %s", g->group.c_str(), g->id.c_str(), reason);
}

/* Lock held: two calls began at once in a group; we go from ours to `other`, the smaller id, which `from` is in. */
void move_to(puddle_calls *cs, const GroupPtr &g, const std::string &other, const std::string &from)
{
    std::string device = g->device;
    auto h = std::make_shared<GroupCall>();
    h->id = other, h->group = g->group, h->starter = from, h->created = now_of(cs);
    h->in[from] = now_of(cs);
    leave_locked(cs, g, "hung_up", false);
    cs->heard.erase(g->group);
    join_locked(cs, h, device.empty() ? nullptr : device.c_str());
    puddle_log(PUDDLE_LOG_DEBUG, "calls: %s went to %s, begun at the same moment", g->id.c_str(), other.c_str());
}

GroupPtr heard_by_id(puddle_calls *cs, const std::string &id)
{
    for (auto &[gid, h] : cs->heard)
        if (h->id == id)
            return h;
    return nullptr;
}

/* ---- the routes members call ---- */

struct Req {
    char id[PUDDLE_CALL_ID_LEN + 2] = "";
    char gid[PUDDLE_USER_ID_LEN + 2] = "";
    std::string sdp;
    int udp = 0;
    uint64_t seq = 0; /* joins and leaves: 0 if not given */
};

bool read_req(const puddle_call *c, Req &r, bool sdp)
{
    puddle_span v;
    const char *b = (const char *)c->body;
    if (!b || puddle_json_member(b, c->body_len, "call", &v) != 0 || puddle_json_string(v, r.id, sizeof r.id) != 0 || !call_id_ok(r.id) ||
        puddle_json_member(b, c->body_len, "group", &v) != 0 || puddle_json_string(v, r.gid, sizeof r.gid) != 0 || !puddle_id_valid(r.gid))
        return false;
    int64_t p;
    if (puddle_json_member(b, c->body_len, "seq", &v) == 0) {
        if (puddle_json_int64(v, &p) != 0 || p < 1)
            return false;
        r.seq = (uint64_t)p;
    }
    if (!sdp)
        return true;
    std::string buf(PUDDLE_CALL_SDP_MAX + 1, '\0');
    if (puddle_json_member(b, c->body_len, "sdp", &v) != 0 || puddle_json_string(v, buf.data(), buf.size()) != 0 ||
        puddle_json_member(b, c->body_len, "udp", &v) != 0 || puddle_json_int64(v, &p) != 0 || p < 1 || p > 65535)
        return false;
    r.sdp = buf.c_str();
    r.udp = (int)p;
    return !r.sdp.empty();
}

} // namespace

/* {call, group}: the sender is in that call. 200 {in: whether we are too; call: ours, if we are in another (smaller) one} */
int callsx::group_join_post(const puddle_call *c, puddle_jw *w)
{
    puddle_calls *cs = c->ctx->calls;
    if (!cs)
        return puddle_fail(w, 409, "no_calls");
    Req r;
    if (!read_req(c, r, false))
        return puddle_fail(w, 400, "bad_request");
    if (!member_with(cs, r.gid, c->sender))
        return puddle_fail(w, 403, "not_member");
    std::string id = r.id, gid = r.gid, sender = c->sender, ours;
    bool in = false;
    {
        std::lock_guard<std::mutex> g(cs->mu);
        int64_t now = now_of(cs);
        if (!fresh(cs, gid, c->sender, r.seq)) { /* older than what they said last: not so any more */
            puddle_jw_object(w);
            puddle_jw_flag(w, "in", 0);
            puddle_jw_end_object(w);
            return 200;
        }
        /* in this call, so not in any other call of the group */
        if (cs->group && cs->group->group == gid && cs->group->id != id && cs->group->in.erase(sender)) {
            drop_pair(cs, *cs->group, sender);
            restate(cs, *cs->group, now);
        }
        if (auto h = cs->heard.find(gid); h != cs->heard.end() && h->second->id != id && h->second->in.erase(sender) &&
                                          h->second->in.empty())
            forget_heard(cs, h);
        if (cs->group && cs->group->group == gid) {
            GroupPtr call = cs->group;
            if (call->id == id) {
                in = true;
                call->in[sender] = now;
                call->alone_since = 0;
                ensure_pair(cs, call, sender);
                restate(cs, *call, now);
            } else if (id < call->id) {
                move_to(cs, call, id, sender);
            } else {
                ours = call->id; /* they come to ours */
            }
        } else {
            auto it = cs->heard.find(gid);
            if (it != cs->heard.end() && it->second->id != id && id < it->second->id)
                cs->heard.erase(it), it = cs->heard.end();
            if (it == cs->heard.end()) {
                auto h = std::make_shared<GroupCall>();
                h->id = id, h->group = gid, h->starter = sender, h->created = now;
                h->rang = !cs->current && !cs->group && !in_voice(cs); /* only when free to answer */
                h->ring_until = now + cs->ring_ms;
                it = cs->heard.emplace(gid, h).first;
                puddle_log(PUDDLE_LOG_INFO, "calls: a call in %s, begun by %.12s…", gid.c_str(), sender.c_str());
            }
            if (it->second->id == id)
                it->second->in[sender] = now;
            else
                ours = it->second->id;
            restate(cs, *it->second, now);
            cs->version++;
        }
    }
    puddle_jw_object(w);
    puddle_jw_flag(w, "in", in);
    if (!ours.empty())
        puddle_jw_str(w, "call", ours.c_str());
    puddle_jw_end_object(w);
    return 200;
}

int callsx::group_leave_post(const puddle_call *c, puddle_jw *w)
{
    puddle_calls *cs = c->ctx->calls;
    if (!cs)
        return puddle_fail(w, 409, "no_calls");
    Req r;
    if (!read_req(c, r, false))
        return puddle_fail(w, 400, "bad_request");
    std::lock_guard<std::mutex> g(cs->mu);
    int64_t now = now_of(cs);
    if (!fresh(cs, r.gid, c->sender, r.seq))
        return 204; /* they have joined again since */
    if (cs->group && cs->group->id == r.id) {
        GroupPtr call = cs->group;
        call->in.erase(c->sender);
        drop_pair(cs, *call, c->sender);
        restate(cs, *call, now);
        cs->version++;
        return 204;
    }
    auto it = cs->heard.find(r.gid);
    if (it == cs->heard.end() || it->second->id != r.id)
        return 204; /* nothing to forget */
    it->second->in.erase(c->sender);
    if (it->second->in.empty())
        forget_heard(cs, it);
    else
        cs->version++;
    return 204;
}

/* {call, group, sdp, udp}: a member in our call, whose id is the smaller, connects to us. 204; we answer in a while. */
int callsx::group_offer_post(const puddle_call *c, puddle_jw *w)
{
    puddle_calls *cs = c->ctx->calls;
    if (!cs)
        return puddle_fail(w, 409, "no_calls");
    Req r;
    if (!read_req(c, r, true))
        return puddle_fail(w, 400, "bad_request");
    if (!member_with(cs, r.gid, c->sender))
        return puddle_fail(w, 403, "not_member");
    std::lock_guard<std::mutex> g(cs->mu);
    GroupPtr call = cs->group;
    if (!call || call->id != r.id)
        return puddle_fail(w, 404, "no_call");
    if (!smaller(c->sender, cs->me))
        return puddle_fail(w, 409, "ours_to_offer");
    std::string peer = c->sender;
    drop_pair(cs, *call, peer); /* they started again */
    if ((int)call->pairs.size() >= PUDDLE_GROUP_CALL_MAX - 1)
        return puddle_fail(w, 409, "full");
    auto pair = std::make_shared<Pair>();
    pair->peer = peer;
    pair->started = now_of(cs);
    pair->next_ts = random32();
    call->pairs[peer] = pair;
    call->in[peer] = now_of(cs);
    call->alone_since = 0;
    slot_of(*call, peer);
    restate(cs, *call, now_of(cs));
    spawn(cs, [cs, wg = std::weak_ptr<GroupCall>(call), pair, id = call->id, gid = call->group, offer = r.sdp, udp = r.udp] {
        answer_to(cs, wg, pair, id, gid, offer, udp);
    });
    return 204;
}

int callsx::group_answer_post(const puddle_call *c, puddle_jw *w)
{
    puddle_calls *cs = c->ctx->calls;
    if (!cs)
        return puddle_fail(w, 409, "no_calls");
    Req r;
    if (!read_req(c, r, true))
        return puddle_fail(w, 400, "bad_request");
    PairPtr pair;
    std::shared_ptr<rtc::PeerConnection> pc;
    GroupPtr call;
    {
        std::lock_guard<std::mutex> g(cs->mu);
        call = cs->group;
        if (!call || call->id != r.id)
            return puddle_fail(w, 404, "no_call");
        auto it = call->pairs.find(c->sender);
        if (it == call->pairs.end() || !it->second->offerer || !it->second->pc)
            return puddle_fail(w, 404, "no_call");
        pair = it->second;
        pc = pair->pc;
    }
    try {
        pc->setRemoteDescription(rtc::Description(r.sdp, "answer"));
        add_their_addresses(cs, pc, pair->peer, r.udp);
    } catch (const std::exception &e) {
        puddle_log(PUDDLE_LOG_INFO, "calls: %.12s…'s answer can't be used: %s", c->sender, e.what());
        std::lock_guard<std::mutex> g(cs->mu);
        drop_pair(cs, *call, pair->peer, pair);
        return puddle_fail(w, 400, "bad_sdp");
    }
    return 204;
}

/* ---- the owner's side ---- */

bool callsx::group_busy(puddle_calls *cs) { return cs->group != nullptr; }

int callsx::group_start(puddle_ctx *ctx, const char *gid, const char *device, char id_out[PUDDLE_CALL_ID_LEN + 1], const char **code)
{
    puddle_calls *cs = ctx->calls;
    if (!puddle_id_valid(gid) || !members_of(cs, gid).in)
        return *code = "no_group", -1;
    std::lock_guard<std::mutex> g(cs->mu);
    if (cs->current || in_voice(cs))
        return *code = "busy", -1;
    if (cs->group) {
        if (cs->group->group != gid)
            return *code = "busy", -1;
        snprintf(id_out, PUDDLE_CALL_ID_LEN + 1, "%s", cs->group->id.c_str()); /* already in it */
        return 0;
    }
    GroupPtr call;
    auto it = cs->heard.find(gid);
    if (it != cs->heard.end()) { /* one going on there: join it */
        if ((int)it->second->in.size() >= PUDDLE_GROUP_CALL_MAX)
            return *code = "full", -1;
        call = it->second;
        cs->heard.erase(it);
    } else {
        call = std::make_shared<GroupCall>();
        call->id = new_call_id();
        if (call->id.empty())
            return *code = "internal", -1;
        call->group = gid;
        call->starter = cs->me;
        call->outgoing = true;
        call->created = now_of(cs);
    }
    join_locked(cs, call, device);
    snprintf(id_out, PUDDLE_CALL_ID_LEN + 1, "%s", call->id.c_str());
    return 0;
}

int callsx::group_accept(puddle_ctx *ctx, const char *id, const char *device, const char **code)
{
    puddle_calls *cs = ctx->calls;
    std::lock_guard<std::mutex> g(cs->mu);
    if (cs->group && cs->group->id == id)
        return 0;
    GroupPtr h = heard_by_id(cs, id);
    if (!h)
        return 1;
    if (cs->current || cs->group || in_voice(cs))
        return *code = "busy", -1;
    if ((int)h->in.size() >= PUDDLE_GROUP_CALL_MAX)
        return *code = "full", -1;
    cs->heard.erase(h->group);
    join_locked(cs, h, device);
    return 0;
}

int callsx::group_decline(puddle_ctx *ctx, const char *id, const char **code)
{
    puddle_calls *cs = ctx->calls;
    std::lock_guard<std::mutex> g(cs->mu);
    GroupPtr h = heard_by_id(cs, id);
    if (!h)
        return cs->group && cs->group->id == id ? (*code = "no_call", -1) : 1;
    h->declined = true; /* stops ringing; it goes on, and can still be joined */
    restate(cs, *h, now_of(cs));
    cs->version++;
    return 0;
}

int callsx::group_end(puddle_ctx *ctx, const char *id, const char **code)
{
    puddle_calls *cs = ctx->calls;
    std::lock_guard<std::mutex> g(cs->mu);
    if (cs->group && cs->group->id == id) {
        GroupPtr call = cs->group;
        leave_locked(cs, call, call->answered ? "hung_up" : call->in.empty() ? "cancelled" : "hung_up");
        return 0;
    }
    if (GroupPtr h = heard_by_id(cs, id)) {
        h->declined = true;
        restate(cs, *h, now_of(cs));
        cs->version++;
        return 0;
    }
    (void)code;
    return 1;
}

int callsx::group_media(puddle_ctx *ctx, const char *id, const char *device, const char *offer, char **answer_out, int *udp_out,
                        const char **code)
{
    puddle_calls *cs = ctx->calls;
    GroupPtr call;
    {
        std::lock_guard<std::mutex> g(cs->mu);
        if (!cs->group || cs->group->id != id)
            return 1;
        call = cs->group;
        if (call->device != device)
            return *code = "no_call", -1;
    }
    auto leg = std::make_shared<Leg>();
    leg->next_ts = random32();
    std::weak_ptr<GroupCall> wg = call;
    std::string sdp = answer_leg(
        cs, leg, offer,
        [wg](const LegPtr &l) {
            GroupPtr c = wg.lock();
            return c && c->joined && c->leg == l;
        },
        [cs, wg] {
            std::lock_guard<std::mutex> g(cs->mu);
            if (GroupPtr c = wg.lock(); c && c->joined)
                leave_locked(cs, c, "failed");
        },
        [cs, wg](const rtc::binary &data, uint32_t in) { /* what the phone says goes to everyone connected */
            std::vector<std::pair<std::shared_ptr<rtc::Track>, uint32_t>> to;
            {
                std::lock_guard<std::mutex> g(cs->mu);
                GroupPtr c = wg.lock();
                if (!c || !c->joined)
                    return;
                for (auto &[peer, p] : c->pairs)
                    if (p->open && p->connected && p->track)
                        to.emplace_back(p->track, stamp(p->next_ts, p->last_in, p->have_in, in));
            }
            for (auto &[track, ts] : to)
                send_on(track, data, ts);
        },
        [cs, wg](const rtc::binary &data, uint32_t in) { /* its camera, to everyone connected */
            std::vector<std::pair<std::shared_ptr<rtc::Track>, uint32_t>> to;
            {
                std::lock_guard<std::mutex> g(cs->mu);
                GroupPtr c = wg.lock();
                if (!c || !c->joined)
                    return;
                for (auto &[peer, p] : c->pairs)
                    if (p->connected && track_open(p->vtrack))
                        to.emplace_back(p->vtrack, p->vbase + in);
            }
            for (auto &[track, ts] : to)
                send_on(track, data, ts);
        });
    if (sdp.empty())
        return *code = "bad_sdp", -1;
    {
        std::lock_guard<std::mutex> g(cs->mu);
        if (!call->joined || call->device != device) {
            leg->pc->resetCallbacks();
            close_leg(cs, leg);
            return *code = "no_call", -1;
        }
        if (call->leg)
            close_leg(cs, call->leg);
        leg->deadline = now_of(cs) + cs->connect_ms;
        call->leg = leg;
        cs->version++;
    }
    *answer_out = strdup(sdp.c_str());
    *udp_out = cs->port;
    if (!*answer_out)
        return *code = "internal", -1;
    return 0;
}

int callsx::group_send_audio(puddle_calls *cs, const char *id, const unsigned char *frame, size_t len)
{
    std::vector<std::pair<std::shared_ptr<rtc::Track>, uint32_t>> to;
    {
        std::lock_guard<std::mutex> g(cs->mu);
        GroupPtr call = cs->group;
        if (!call || call->id != id)
            return 1;
        if (!call->device.empty())
            return -1;
        for (auto &[peer, p] : call->pairs)
            if (p->open && p->connected && p->track) {
                to.emplace_back(p->track, p->next_ts);
                p->next_ts += 960; /* 20 ms at 48 kHz */
            }
    }
    if (to.empty())
        return -1;
    for (auto &[track, ts] : to)
        try {
            track->sendFrame((const rtc::byte *)frame, len, rtc::FrameInfo(ts));
        } catch (const std::exception &) {
        }
    return 0;
}

int callsx::group_send_video(puddle_calls *cs, const char *id, const unsigned char *frame, size_t len, bool screen)
{
    std::vector<std::pair<std::shared_ptr<rtc::Track>, uint32_t>> to;
    {
        std::lock_guard<std::mutex> g(cs->mu);
        GroupPtr call = cs->group;
        if (!call || call->id != id)
            return 1;
        if (!call->device.empty())
            return -1;
        const uint32_t now = video_now();
        for (auto &[peer, p] : call->pairs)
            if (p->connected && track_open(screen ? p->strack : p->vtrack))
                to.emplace_back(screen ? p->strack : p->vtrack, (screen ? p->sbase : p->vbase) + now);
    }
    if (to.empty())
        return -1;
    for (auto &[track, ts] : to)
        try {
            track->sendFrame((const rtc::byte *)frame, len, rtc::FrameInfo(ts));
        } catch (const std::exception &) {
        }
    return 0;
}

/* ---- what the owner sees ---- */

void callsx::write_group(puddle_ctx *ctx, puddle_jw *w, const GroupCall &g)
{
    bool media = false;
    for (auto &[peer, p] : g.pairs)
        media = media || (p->open && p->connected);
    if (!g.device.empty())
        media = media && g.leg && g.leg->open && g.leg->connected;
    puddle_jw_object(w);
    puddle_jw_str(w, "id", g.id.c_str());
    puddle_jw_str(w, "user", g.starter.c_str());
    puddle_jw_str(w, "direction", g.outgoing ? "outgoing" : "incoming");
    puddle_jw_str(w, "state", g.state.c_str());
    puddle_jw_key(w, "reason");
    g.state == "ended" ? puddle_jw_string(w, g.reason.c_str()) : puddle_jw_null(w);
    puddle_jw_flag(w, "media", g.joined && media);
    write_device(ctx, w, g.joined ? g.device : std::string());
    puddle_jw_str(w, "group", g.group.c_str());
    puddle_jw_key(w, "members"); /* the others in the call */
    puddle_jw_array(w);
    for (auto &[peer, when] : g.in) {
        auto it = g.pairs.find(peer);
        auto slot = g.slots.find(peer);
        puddle_jw_object(w);
        puddle_jw_str(w, "user", peer.c_str());
        puddle_jw_flag(w, "media", g.joined && it != g.pairs.end() && it->second->open && it->second->connected);
        puddle_jw_num(w, "slot", slot != g.slots.end() ? slot->second : 0);
        puddle_jw_end_object(w);
    }
    puddle_jw_end_array(w);
    puddle_jw_num(w, "created", g.created);
    puddle_jw_key(w, "answered");
    g.answered ? puddle_jw_int(w, g.answered) : puddle_jw_null(w);
    puddle_jw_key(w, "ended");
    g.ended ? puddle_jw_int(w, g.ended) : puddle_jw_null(w);
    puddle_jw_end_object(w);
}

int callsx::group_write_one(puddle_ctx *ctx, puddle_jw *w, const char *id)
{
    puddle_calls *cs = ctx->calls;
    if (cs->group && cs->group->id == id)
        return write_group(ctx, w, *cs->group), 0;
    if (GroupPtr h = heard_by_id(cs, id))
        return write_group(ctx, w, *h), 0;
    for (auto &g : cs->ended_groups)
        if (g->id == id)
            return write_group(ctx, w, *g), 0;
    return 1;
}

int callsx::group_write_live(puddle_ctx *ctx, puddle_jw *w, const char *user, const char *group)
{
    puddle_calls *cs = ctx->calls;
    if (user && !group)
        return 0; /* a friend's calls: group calls aren't theirs */
    int n = 0;
    if (cs->group && (!group || cs->group->group == group))
        write_group(ctx, w, *cs->group), n++;
    for (auto &[gid, h] : cs->heard)
        if (!group || gid == group)
            write_group(ctx, w, *h), n++;
    return n;
}

/* ---- the worker's round ---- */

void callsx::group_tick(puddle_calls *cs, int64_t now)
{
    std::lock_guard<std::mutex> g(cs->mu);
    const int64_t gone = 3 * cs->beat_ms;
    if (GroupPtr call = cs->group) {
        for (auto it = call->in.begin(); it != call->in.end();) { /* not heard from in three beats, and no connection: gone */
            auto p = call->pairs.find(it->first);
            if (now - it->second >= gone && (p == call->pairs.end() || !p->second->connected)) {
                drop_pair(cs, *call, it->first);
                it = call->in.erase(it);
                cs->version++;
            } else {
                ++it;
            }
        }
        std::vector<std::string> stale;
        for (auto &[peer, p] : call->pairs)
            if ((!p->connected && now - p->started >= cs->connect_ms) || (p->lost_since && now - p->lost_since >= LOST_MS))
                stale.push_back(peer);
        for (auto &peer : stale)
            drop_pair(cs, *call, peer);
        for (auto &[peer, when] : call->in)
            ensure_pair(cs, call, peer);
        if (now - call->beat_at >= cs->beat_ms) {
            call->beat_at = now;
            announce(cs, call);
        }
        const char *reason = nullptr;
        if (call->in.empty() && call->pairs.empty()) {
            if (!call->alone_since)
                call->alone_since = now;
            else if (now - call->alone_since >= cs->ring_ms)
                reason = call->answered ? "hung_up" : "no_answer"; /* nobody came, or everyone left */
        } else {
            call->alone_since = 0;
        }
        if (!reason && !call->device.empty()) {
            LegPtr leg = call->leg;
            if ((leg && !leg->connected && now >= leg->deadline) || (leg && leg->lost_since && now - leg->lost_since >= LOST_MS) ||
                phone_gone(cs, call->device, now))
                reason = "failed";
        }
        if (reason)
            leave_locked(cs, call, reason);
        else
            restate(cs, *call, now);
    }
    for (auto it = cs->heard.begin(); it != cs->heard.end();) {
        GroupPtr h = it->second;
        for (auto m = h->in.begin(); m != h->in.end();)
            m = now - m->second >= gone ? h->in.erase(m) : std::next(m);
        if (h->in.empty()) {
            auto next = std::next(it);
            forget_heard(cs, it);
            it = next;
            continue;
        }
        restate(cs, *h, now);
        ++it;
    }
}

void callsx::group_stop(puddle_calls *cs)
{
    std::lock_guard<std::mutex> g(cs->mu);
    if (GroupPtr call = cs->group)
        leave_locked(cs, call, "hung_up");
}

void callsx::group_release(puddle_calls *cs)
{
    std::deque<GroupPtr> all;
    {
        std::lock_guard<std::mutex> g(cs->mu);
        all.swap(cs->ended_groups);
        cs->heard.clear();
    }
    for (auto &c : all) {
        for (auto &[peer, p] : c->pairs)
            if (p->pc) {
                p->pc->resetCallbacks();
                p->pc->close();
            }
        if (c->leg && c->leg->pc) {
            c->leg->pc->resetCallbacks();
            c->leg->pc->close();
        }
    }
}

#endif
