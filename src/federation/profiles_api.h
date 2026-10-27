#ifndef PUDDLE_PROFILES_API_H
#define PUDDLE_PROFILES_API_H

/* How profiles travel between Puddles (docs/profiles.md). The owner chooses a profile for each place (a direct
 * conversation, a group); that choice is the consent for it to go there, and nothing goes where nothing was chosen.
 *
 *   ours    the direct profile rides on requests and answers ("who we are"); any place's profile is pushed with
 *           POST /stream/v1/profile ({"profile", "group"}) whenever the choice or the profile changes, and when a
 *           connection opens; a null profile takes it back.
 *   theirs  kept per place in the people store, cleaned on arrival with a note for whatever had to be left out; their
 *           pictures are fetched by hash (GET /stream/v1/media) and kept.
 *
 * A peer can fetch one of our pictures only if it is in a profile shown to them in some place. */

#include "http/router.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const puddle_route puddle_profile_routes[];
extern const size_t puddle_profile_routes_count;

/* The choice for `place` ("dm:<user>" or "group:<group>") changed, or should be said again: tell whoever is there which
 * profile they now see, or that they see none (queued, retried while they are away). For a group, everyone in it we have
 * a connection to, and only while we are in it. 0, or -1 if anything could not be queued. */
int puddle_profile_push(puddle_ctx *ctx, const char *place);

/* The profile `profile_id` was edited: push it to every place showing it. 0 or -1. */
int puddle_profile_changed(puddle_ctx *ctx, const char *profile_id);

/* Tell `peer` the profile chosen for `place`, if one was (nothing is said otherwise): for when they first become someone
 * there, such as a member joining a group. For a group, only while we are in it. 0, or -1 if it could not be queued. */
int puddle_profile_push_to(puddle_ctx *ctx, const char *peer, const char *place);

/* A connection with `peer` has opened (or they joined a group we are in): tell them the profile of each group we share
 * where we chose one. How many were queued. */
int puddle_profile_push_groups_to(puddle_ctx *ctx, const char *peer);

/* Keep `json`, a profile `peer`'s Puddle sent us for `place` ("dm" or "group:<group>", already cleaned), with the `notes`
 * on what had to be left out (NULL or "" if nothing). An empty `json` means they show us nothing there, and what we held
 * for it is forgotten. 0, or -1 if it could not be kept. Does nothing in a context with no people store. */
int puddle_profile_keep(puddle_ctx *ctx, const char *peer, const char *place, const char *json, const char *notes);

/* Queue fetches for the images in `peer`'s profiles that we do not hold. Only for an open stream. Returns how many were queued. */
int puddle_media_want(puddle_ctx *ctx, const char *peer);

/* The outbox's `done` hook for what this module sends: a fetched image is checked and kept; trouble is noted on the stream. */
void puddle_profiles_done(puddle_ctx *ctx, const puddle_ob_item *item, puddle_ob_outcome outcome, const puddle_net_reply *reply);

#ifdef __cplusplus
}
#endif
#endif
