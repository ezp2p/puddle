#ifndef PUDDLE_FEDERATION_POND_DMS_H
#define PUDDLE_FEDERATION_POND_DMS_H

/* Direct messages through a Pond (docs/pond-dms.md; the protocol's spec/10, "Letters"): sealing letters to another member and
 * sending them through the Pond, holding them while the other Puddle is away, opening the ones that come, the receipts both
 * ways, and a letter's files, a piece at a time through the Pond. What is kept is core/pond_dms.hpp's. Two threads: one sends
 * and answers the Pond's asks for pieces, the other fetches the files of letters that came. */

#include "core/context.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start and stop the threads (ctx->pond_dms_worker). Needs ctx->pond_dms. */
void puddle_pdm_start(puddle_ctx *ctx);
void puddle_pdm_stop(puddle_ctx *ctx);

/* A Pond's /events answer (federation/ponds.cpp's follower): the parts for direct messages, `dm`, `dm_back` and `dm_want`, are
 * taken in hand. Returns at once; the work is done by the threads. */
void puddle_pdm_events(puddle_ctx *ctx, const char *pond, const char *json, size_t len);

/* A chat event of ours in a conversation through a Pond (Conversation and Message, objects/): signed, kept in its chat log, and a
 * letter for it queued. 0 with its id, or -1 with `code`: "no_conversation", "left" (they or we are no longer in the Pond),
 * "not_accepted" (they take no direct messages there), "dms_off" (the Pond relays none), "files_off", "too_large", "failed". */
int puddle_pdm_send_event(puddle_ctx *ctx, const char *chat, const char *event, size_t len, char id[65], const char **code);

/* Offer to be friends: a letter carrying a link to this Puddle, good once, for 30 days. 0, or -1 with `code` as above, or
 * "no_link" (this Puddle has no address others can reach). */
int puddle_pdm_befriend(puddle_ctx *ctx, const char *chat, const char **code);

/* Whether the owner takes direct messages in `pond`: told to the Pond. 0, or -1 with `code` as for puddle_pond_admin. */
int puddle_pdm_accept(puddle_ctx *ctx, const char *pond, int accept, const char **code);

/* Try what waits for the conversation `chat` now. */
void puddle_pdm_retry(puddle_ctx *ctx, const char *chat);

#ifdef __cplusplus
}
#endif
#endif
