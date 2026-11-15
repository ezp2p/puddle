#ifndef PUDDLE_CALLS_H
#define PUDDLE_CALLS_H

/* Calls between friends (docs/calls.md): the two Puddles carry the call over WebRTC (libdatachannel), set up with signed
 * requests on the stream, and each passes Opus frames to and from its owner's device.
 *
 * One call at a time. Built without libdatachannel (PUDDLE_HAVE_CALLS unset), or with `calls: off`, there are none: the
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
 * no_call (no such call, or not in a state for this). */
int puddle_call_start(puddle_ctx *ctx, const char *peer, char id[PUDDLE_CALL_ID_LEN + 1], const char **code);
int puddle_call_accept(puddle_ctx *ctx, const char *id, const char **code);
int puddle_call_decline(puddle_ctx *ctx, const char *id, const char **code);
int puddle_call_end(puddle_ctx *ctx, const char *id, const char **code);
/* `id` NULL: {"version": n, "items": [the current call, then up to `limit` ended ones, newest first]}, only those with `user`
 * if it is given. `version` changes whenever a call does. Otherwise that call: 0, or 1 if there is none. */
int puddle_calls_write(puddle_ctx *ctx, const char *id, const char *user, int limit, puddle_jw *w);

/* ---- media: the owner's device, in this process (the window) ---- */

/* Each Opus frame from the friend, as it arrives: called on a network thread, so it must not block. `rtp_ts` is the RTP
 * timestamp (48 kHz), for the jitter buffer. */
typedef void (*puddle_call_frame_fn)(void *user, const char *call, const unsigned char *frame, size_t len, uint32_t rtp_ts);
void puddle_calls_on_audio(puddle_calls *c, puddle_call_frame_fn fn, void *user);
/* Send one Opus frame of 20 ms on the active call `id`. 0 sent, -1 no such active call (or too large). */
int puddle_call_send_audio(puddle_calls *c, const char *id, const unsigned char *frame, size_t len);
/* For tests: how long ringing (the caller's and the callee's) and connecting may take, in ms; 0 leaves one as it is. */
void puddle_calls_set_timeouts(puddle_calls *c, int64_t ring_ms, int64_t ring_in_ms, int64_t connect_ms);

/* Changes whenever a call does: the window polls it. */
uint64_t puddle_calls_version(puddle_calls *c);

/* The routes friends call: POST /stream/v1/call/offer, /answer, /end. */
extern const puddle_route puddle_call_routes[];
extern const size_t puddle_call_routes_count;

#ifdef __cplusplus
}
#endif
#endif
