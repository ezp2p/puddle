#ifndef PUDDLE_CALLS_H
#define PUDDLE_CALLS_H

/* Calls between friends (docs/calls.md): the two Puddles carry the call over WebRTC (libdatachannel), set up with signed
 * requests on the stream, and each passes Opus frames to and from its owner's device. A group call is a connection between
 * each pair of the members in it ("Group calls").
 *
 * One call at a time, with a friend or in a group. Built without libdatachannel (PUDDLE_HAVE_CALLS unset), or with `calls: off`, there are none: the
 * owner is told `no_calls`, and a friend's offer is answered `409 no_calls`. */

#include "base/json.h"
#include "core/context.h"
#include "core/outbox.h"
#include "http/router.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PUDDLE_CALL_ID_LEN 26          /* base32 of 16 random bytes */
#define PUDDLE_CALL_SDP_MAX (16 * 1024)
#define PUDDLE_CALL_RING_MS 45000      /* the caller gives up after this */
#define PUDDLE_CALL_RING_IN_MS 60000   /* the callee, if the caller's word never comes */
#define PUDDLE_CALL_CONNECT_MS 20000   /* from accepting to a connection */
#define PUDDLE_CALL_KEEP_ENDED 10      /* ended calls whose connections are kept a while */
#define PUDDLE_CALL_HISTORY 500        /* ended calls the owner's history keeps (state/calls.jsonl) */
#define PUDDLE_CALL_FRAME_MAX 1500     /* the largest Opus frame passed on */
#define PUDDLE_CALL_VIDEO_MAX (1024 * 1024) /* the largest video frame (an H.264 access unit) passed on */
#define PUDDLE_GROUP_CALL_MAX 8        /* people in a group call, us included: each sends to all the others */
#define PUDDLE_GROUP_CALL_BEAT_MS 20000 /* how often a member in a group call says so again; one not heard for three is gone */

/* Was this Puddle built with calls? */
int puddle_calls_built(void);

/* Open the UDP port (calls.port) and start the timer. `hooks` say how to reach friends (the outbox's: federation/sender.h)
 * and must outlive the result. NULL with the reason in `err` if calls are off, not built, or the port can't be used; a
 * Puddle runs on without calls then. */
puddle_calls *puddle_calls_start(puddle_ctx *ctx, const puddle_outbox_hooks *hooks, char *err, size_t errlen);
/* Ends every call (telling the friend) and stops. NULL is fine. */
void puddle_calls_stop(puddle_calls *c);

/* ---- the owner's side (the courier's call_* entries) ----
 * Each returns 0, or -1 with `code`: no_calls, no_stream (not a friend with an active stream), busy (a call is going on),
 * no_call (no such call, or not in a state for this).
 * `device` is who starts or answers: NULL for the owner's device in this process (the window), or the id of the phone that
 * asked. That one has the call: the friend's frames go to it, and only it sends. */
int puddle_call_start(puddle_ctx *ctx, const char *peer, const char *device, char id[PUDDLE_CALL_ID_LEN + 1], const char **code);
/* Start a call in group `gid`, or join the one going on there. Codes as above, and no_group (not a member), full. */
int puddle_call_start_group(puddle_ctx *ctx, const char *gid, const char *device, char id[PUDDLE_CALL_ID_LEN + 1], const char **code);
int puddle_call_accept(puddle_ctx *ctx, const char *id, const char *device, const char **code);
int puddle_call_decline(puddle_ctx *ctx, const char *id, const char **code);
int puddle_call_end(puddle_ctx *ctx, const char *id, const char **code);
/* `id` NULL: {"version": n, "items": [the current call, the group calls going on that we are not in, then up to `limit`
 * ended ones, newest first]}, only those with `user` (a friend's) or in `group` if given. `version` changes whenever a call
 * does. Otherwise that call: 0, or 1 if there is none. */
int puddle_calls_write(puddle_ctx *ctx, const char *id, const char *user, const char *group, int limit, puddle_jw *w);

/* ---- media: a phone's leg (docs/calls.md, "On a phone") ----
 * The phone `device` that has call `id` offers a WebRTC leg of its own (`sdp`, complete with candidates); the answer, also
 * complete, is put in `*answer` (free() it), with the UDP port it is on. Asking again replaces the leg. 0, or -1 with
 * `code`: no_calls, no_call (no such call, or another device has it), bad_sdp. */
int puddle_call_media(puddle_ctx *ctx, const char *id, const char *device, const char *sdp, char **answer, int *udp,
                      const char **code);

/* ---- media: the owner's device, in this process (the window) ---- */

/* Each Opus frame from the friend (or a member, in a group call: `from`), as it arrives: called on a network thread, so it
 * must not block. `rtp_ts` is the RTP timestamp (48 kHz) in that one's own count, for their jitter buffer. */
typedef void (*puddle_call_frame_fn)(void *user, const char *call, const char *from, const unsigned char *frame, size_t len,
                                     uint32_t rtp_ts);
void puddle_calls_on_audio(puddle_calls *c, puddle_call_frame_fn fn, void *user);
/* Send one Opus frame of 20 ms on the active call `id` (to everyone connected, in a group call). 0 sent, -1 no such active
 * call (or too large). */
int puddle_call_send_audio(puddle_calls *c, const char *id, const unsigned char *frame, size_t len);
/* ---- video (docs/calls.md, "Video"): H.264 access units, Annex B (each NAL unit after a start code), on a track of its own
 * beside the sound. The Puddle passes them on whole and decodes nothing; the device encodes, and decodes each sender's. ---- */

/* Each frame of video from the friend (or member `from`), as it arrives; `rtp_ts` in that one's own 90 kHz count. Called on
 * a network thread: it must not block. */
void puddle_calls_on_video(puddle_calls *c, puddle_call_frame_fn fn, void *user);
/* Send one frame of the owner's camera on the active call `id` (to everyone connected, in a group call), stamped with the
 * time now. 0 sent, -1 not (no such active call, its video track isn't open yet, or too large). */
int puddle_call_send_video(puddle_calls *c, const char *id, const unsigned char *frame, size_t len);

/* ---- voice channels in Ponds (docs/ponds.md, "Voice channels"; the Pond's docs/voice.md) ----
 * Not a call: you join one and sit there; nothing rings, nothing goes in the history, and it isn't in puddle_calls_write. It
 * is carried here because it is the same kind of connection, on the same UDP port, with the same device at the end of it, and
 * because the owner talks in one place at a time: in a voice channel, a friend's ring is answered `busy` ("already in a
 * call"), and starting or answering a call is `busy`; while in a call, joining a voice channel is `busy`.
 * For now the owner's device in this process (the window) has it; a phone's leg comes later. */

/* Join voice channel `channel` of Pond `pond` (leaving any other voice channel). Returns once the Pond has answered; the
 * connection comes up a moment later (`state` connecting, then active). 0, or -1 with `code`: no_calls, no_pond, no_channel,
 * not_voice (a text channel, or a Pond without voice), busy, full, removed (the Pond no longer has us), unreachable, failed. */
int puddle_voice_join(puddle_ctx *ctx, const char *pond, int64_t channel, const char **code);
/* Leave it (the Pond is told). 0, also when in none. */
int puddle_voice_leave(puddle_ctx *ctx, const char **code);
/* Muted (nothing of ours goes) and deafened (nothing comes to us); -1 leaves one as it is. The Pond is told, and tells everyone.
 * 0, or -1 with `code` no_voice (in none), or one of join's. */
int puddle_voice_state(puddle_ctx *ctx, int muted, int deafened, const char **code);
/* The voice channel we are in: {pond, channel, state, slot, muted, deafened}, with `reason` when it ended without us leaving
 * (failed, removed); or null. */
void puddle_voice_write(puddle_ctx *ctx, puddle_jw *w);
/* Send one Opus frame of 20 ms of the owner's voice to the voice channel. 0 sent, -1 not (in none, not connected, or muted).
 * Frames from the others come to puddle_calls_on_audio's function, with `call` "voice" and `from` the member's id. */
int puddle_voice_send_audio(puddle_calls *c, const unsigned char *frame, size_t len);

/* For tests: how long ringing (the caller's and the callee's) and connecting may take, in ms; 0 leaves one as it is. */
void puddle_calls_set_timeouts(puddle_calls *c, int64_t ring_ms, int64_t ring_in_ms, int64_t connect_ms);
/* For tests: how often a member in a group call says so again (and so how soon one that went is noticed: three times it). */
void puddle_calls_set_beat(puddle_calls *c, int64_t beat_ms);

/* Changes whenever a call does: the window polls it. */
uint64_t puddle_calls_version(puddle_calls *c);

/* The routes friends call: POST /stream/v1/call/offer, /answer, /end; and members, /stream/v1/call/group/join, /leave,
 * /offer, /answer. */
extern const puddle_route puddle_call_routes[];
extern const size_t puddle_call_routes_count;

#ifdef __cplusplus
}
#endif
#endif
