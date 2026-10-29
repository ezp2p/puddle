#ifndef PUDDLE_FEDERATION_FILES_H
#define PUDDLE_FEDERATION_FILES_H

/* Files between Puddles (docs/files.md): the routes another Puddle fetches a file's chunks from, how our downloads
 * (core/transfers.h) travel, and what starts them. */

#include "http/router.h"
#include "core/outbox.h"

#ifdef __cplusplus
extern "C" {
#endif

/* GET /stream/v1/files/{file}/chunks?chat=&message=      the chunk hashes, 32 bytes each, as bytes
 * GET /stream/v1/files/{file}/chunks/{n}?chat=&message=  chunk n; once we have sent them the file whole, only again
 *                                                        with the owner's yes (docs/files.md, "Asking again")
 * POST /stream/v1/files/{file}/refused {chat, message, max}  they take nothing that large: a file of ours they will not fetch
 * POST /stream/v1/files/{file}/request {chat, message}       they want it again
 * POST /stream/v1/files/{file}/granted {chat, message}       one we asked will send it: fetch it now
 * POST /stream/v1/files/{file}/done {chat, message}          they have it: stop asking our owner
 * Only for a peer in `chat` (the DM with them over an open stream, or a group we are both in), only when `message` there
 * carries the file, and only what we have of it; anything else is a 404. */
extern const puddle_route puddle_file_routes[];
extern const size_t puddle_file_routes_count;

/* The `get` hook of core/transfers.h: a signed GET to `peer`, over `user`, the outbox's hooks (its addresses and
 * certificate checks). */
void puddle_federation_fetch(void *user, const char *peer, const char *path, size_t max, puddle_net_reply *reply);

/* A message from `sender` was just stored in `chat` as event `id`: fetch its files that come by themselves (up to
 * files.auto_download, and never past files.max_size). */
void puddle_files_arrived(puddle_ctx *ctx, const char *chat, const char *sender, const char *event, size_t len, const char *id);

/* The owner asked for file `file` of message `message` in `chat` (the courier's file_fetch). 0, or a status and a code:
 * 404 `not_found` (no such message, or it carries no such file), 409 `too_large` (past files.max_size). */
int puddle_file_fetch(puddle_ctx *ctx, const char *chat, const char *message, const char *file, const char **code);

/* The owner asks again for a file of `message` in `chat` that nobody would send (they had sent it to us whole before):
 * everyone who may have it is asked, and the first to say yes sends it (the courier's file_request). 0, or 404
 * `not_found`, 409 `too_large`, `already_here` or `nobody_to_ask`. */
int puddle_file_request(puddle_ctx *ctx, const char *chat, const char *message, const char *file, const char **code);

/* The owner's answer to `peer` asking for `file` of `message` in `chat` again (the courier's file_answer): yes sends it
 * once, no forgets the request without telling them. 0, or 404 `not_found` (no such request). */
int puddle_file_answer(puddle_ctx *ctx, const char *chat, const char *message, const char *file, const char *peer, int yes, const char **code);

/* The `finished` hook of core/transfers.h (`user` is the context): a file we asked for again is here. */
void puddle_file_finished(void *user, const char *file, const char *chat, const char *message);

#ifdef __cplusplus
}
#endif
#endif
