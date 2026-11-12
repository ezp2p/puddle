#ifndef PUDDLE_FEDERATION_PONDS_H
#define PUDDLE_FEDERATION_PONDS_H

/* Being a member of Ponds (docs/ponds.md; the Pond's routes are in the pond repository's api/pond.yaml). Joining with an invite,
 * what the owner does there (sending, scrolling back, leaving, changing the profile it sees), and following each Pond's events:
 * a thread for each, holding one request open on the Pond so news arrives as it happens, through NAT or a Levee alike. What a
 * Pond says is kept in the context's `ponds` (core/ponds.h). */

#include "core/context.h"
#include "base/json.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Join the Pond a `pond://` link names, showing it the profile `profile_id` (NULL: none). Its id goes to `out_id`, and it is
 * followed from now on. 0, or -1 with `code`: "unreachable" (nobody answered), "not_a_pond" (something answered, but not a
 * Pond), "invite_refused" (the Pond did not accept the invite: unknown, used up, expired or revoked; it does not say which),
 * "no_profile", "failed". */
int puddle_pond_join(puddle_ctx *ctx, const puddle_link *link, const char *profile_id, char out_id[PUDDLE_USER_ID_LEN + 1],
                     const char **code);

/* Post `text` to a channel, with `files` (NULL, or the text of a "files" array as the owner's send gives it: files here whole,
 * each sent to the Pond first, docs/ponds.md "Files"). 0, or -1 with `code`: "no_pond", "no_channel", "bad_text", "slow_down",
 * "unreachable", "removed" (the Pond no longer has us), "no_file" (one is not here whole), "files_off" (the Pond takes none),
 * "too_large" (one is over what it takes), "failed". */
int puddle_pond_send(puddle_ctx *ctx, const char *pond, int64_t channel, const char *text, const char *files, size_t files_len,
                     const char **code);

/* Messages older than what is kept, asked of the Pond: its answer (malloc'd, newest first; puddle_ponds_write_page writes it for
 * the owner), kept too if it joins onto what is. 0, or -1 with `code` as for sending. */
int puddle_pond_history(puddle_ctx *ctx, const char *pond, int64_t channel, int64_t before, int limit, char **page, size_t *len,
                        const char **code);

/* The owner wants a file a message carries that is not fetched by itself (larger than files.auto_download, or the message is
 * older than what is kept): fetch it soon, and keep it a week. 0, or -1 with `code`: "no_pond", "no_channel", "too_large". */
int puddle_pond_fetch(puddle_ctx *ctx, const char *pond, int64_t channel, int64_t message, const char *file, const char **code);

/* What an operator or administrator asks of the Pond (`target` under /pond/v1/admin/), or the answer to a role it offered
 * (/pond/v1/roles/answer): the Pond's status and answer (malloc'd), as it gave them. 0, or -1 with `code` as for sending. */
int puddle_pond_admin(puddle_ctx *ctx, const char *pond, const char *method, const char *target, const char *body, size_t len,
                      int *status, char **reply, size_t *reply_len, const char **code);

/* Leave: tell the Pond (if it can be reached; we leave either way) and forget it, with what was kept of it. 0, or -1 "no_pond". */
int puddle_pond_leave(puddle_ctx *ctx, const char *pond, const char **code);

/* The profile chosen for "pond:<id>" changed: tell that Pond, soon (its follower carries it). 0, or -1 if we are not in it. */
int puddle_pond_profile_push(puddle_ctx *ctx, const char *pond);

/* Our keys rolled: give every Pond our new chain. */
void puddle_ponds_keys_rolled(puddle_ctx *ctx);

/* The followers: one for each Pond in ctx->ponds, from start until stop. Stopping ends any request that is waiting. */
typedef struct puddle_pond_followers puddle_pond_followers;
puddle_pond_followers *puddle_pond_followers_start(puddle_ctx *ctx);
void puddle_pond_followers_stop(puddle_pond_followers *f);

#ifdef __cplusplus
}
#endif
#endif
