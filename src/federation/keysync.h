#ifndef PUDDLE_KEYSYNC_H
#define PUDDLE_KEYSYNC_H

/* Keeping our friends' picture of us up to date (docs/streams.md, "Telling friends about a new address"; roadmap 4.2):
 *
 *   - when our keys change, every peer that holds our chain is sent the new one (`POST /stream/v1/identity`), ahead
 *     of anything else waiting for it; and what is already waiting, which was signed or addressed with the old
 *     keys, is signed again, since the other side will not believe the new key until it has been told;
 *   - when where we are, or our certificate, changes, every active peer is sent a signed address record
 *     (`POST /stream/v1/address`), which is also how a changed certificate is announced so that it is accepted
 *     quietly instead of holding their messages for the owner to look at.
 *
 * What each peer is known to hold is kept on its stream record (`chain_sent`, `addr_sent`), so a restart, or a peer
 * who was away, loses nothing, and a peer who has the latest is not told again. The two inbound routes are here too. */

#include "http/router.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const puddle_route puddle_sync_routes[];
extern const size_t puddle_sync_routes_count;

#define PUDDLE_ADDRESS_DOMAIN "puddle-address-v1\n"

/* Start-up: note whether our address or certificate has changed since last time (and make the next address record
 * if so), then bring everyone up to date. Returns how many things were queued or re-signed. */
int puddle_sync_resume(puddle_ctx *ctx);

/* Just after the keys have changed: the same, for the keys. */
int puddle_sync_after_roll(puddle_ctx *ctx);

/* The outbox's `done` hook, for the kinds this module sends (and for the ones that carry our chain and address, which
 * tell us what the peer now holds). */
void puddle_sync_done(puddle_ctx *ctx, const puddle_ob_item *item, puddle_ob_outcome outcome);

#ifdef __cplusplus
}
#endif
#endif
