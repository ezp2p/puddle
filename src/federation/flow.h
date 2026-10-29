#ifndef PUDDLE_FLOW_H
#define PUDDLE_FLOW_H

/* Stream traffic we start: asking for a stream, telling someone the answer, telling someone it is closed
 * (docs/streams.md). Each is a signed POST queued in the outbox; what comes of it is handled in puddle_flow_done.
 * The inbound half (the other Puddle answering us) is in inbound.c. */

#include "http/router.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Write who we are, as `peer` is shown us, as a JSON object {"id","address","profile","chain"} into `w`.
 * 0, or -1 if we cannot say where we are (see puddle_my_address). */
int puddle_flow_write_self(puddle_ctx *ctx, const char *peer, puddle_jw *w);

/* Ask the Puddle at the address in `link` for a stream: records `pending_out` under a made-up id (a ticket stands in until their
 * answer says who they are; that id is returned in `out_id`), shows them `profile_id` (NULL = the default), and queues the
 * request. Returns 0 and fills `out_id`, or an HTTP status for the owner with `code`
 * and `message` saying why (400 bad link, 409 already, ...). */
int puddle_flow_request(puddle_ctx *ctx, const puddle_link *link, const char *message, const char *profile_id,
                        char out_id[PUDDLE_USER_ID_LEN + 1], const char **code, const char **message_out);

/* The answer to a request we sent to an address has said who it is (the ticket in it is the one we sent): refile the
 * provisional record under `real_id`. 0 if it is now theirs; nonzero if there was no such request, a record for them
 * already existed, or they are blocked (the request is dropped). */
int puddle_flow_adopt(puddle_ctx *ctx, const char *ticket, const char *real_id);

/* Queue what we owe `peer` for its `owed` state (an approval or refusal of its request, or the news that we closed).
 * 0, or -1 if it could not be queued (the record keeps saying we owe it, and puddle_flow_resume tries again). */
int puddle_flow_owe(puddle_ctx *ctx, const char *peer, puddle_owed owed);

/* At start: queue whatever a record says is owed that is not already waiting in the outbox (we may have stopped
 * between recording the decision and queuing it). Returns how many were queued. */
int puddle_flow_resume(puddle_ctx *ctx);

/* The outbox's `done` hook, with the context as its user: what a delivered or failed item means for the stream. */
void puddle_flow_done(void *ctx, const puddle_ob_item *item, puddle_ob_outcome outcome, const puddle_net_reply *reply);

#ifdef __cplusplus
}
#endif
#endif
