#ifndef PUDDLE_SERVICE_LEVEE_H
#define PUDDLE_SERVICE_LEVEE_H

/* Keeping this Puddle attached to its Levee (docs/levee.md; the protocol's spec/06): one thread that attaches, tells the Levee
 * who may reach us (peers, sealed to it), holds a request on its inbox, and runs what comes through the router as if it had
 * come here directly, posting the answers back. Joining and leaving are the owner's, through the API (ctx->levee_join,
 * ctx->levee_leave). What is kept is core/levee.h's; everything this Puddle sends goes out through the Levee by itself
 * (http/client.cpp). */

#include "core/context.h"
#include "http/router.h"
#include "core/outbox.h"
#include "core/delivery.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start the thread, and set ctx->levee_join, levee_leave and levee_attached. `router` serves what the Levee hands over. */
void puddle_levee_worker_start(puddle_ctx *ctx, const puddle_router *router);
void puddle_levee_worker_stop(puddle_ctx *ctx);

/* The `get` hook of core/transfers.h: a file that waits at its sender's Levee, sealed under the key its message carries, is
 * fetched from there (spec/06, "Files"); anything else, or if it isn't there, from them as always (puddle_federation_fetch). */
typedef struct {
    puddle_ctx *ctx;
    const puddle_outbox_hooks *hooks;
} puddle_levee_fetcher;
void puddle_levee_fetch(void *user, const char *peer, const char *path, size_t max, puddle_net_reply *reply);

#ifdef __cplusplus
}
#endif
#endif
