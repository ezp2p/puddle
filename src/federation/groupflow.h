#ifndef PUDDLE_GROUPFLOW_H
#define PUDDLE_GROUPFLOW_H

/* What the owner does with groups, and what it sends to the other members (docs/groups.md). Each function returns 0, or the HTTP
 * status the owner API should answer with and a code saying why. */

#include "http/router.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Start a group named `name`; we are its only member. */
int puddle_group_create(puddle_ctx *ctx, const char *name, char gid[PUDDLE_USER_ID_LEN + 1], const char **code);

/* Invite `invitee`, who must be a friend of ours (an active stream). The members hear of it as an event; the invitee is sent the
 * invitation, with the group's first event and who is in it. */
int puddle_group_invite(puddle_ctx *ctx, const char *gid, const char *invitee, const char **code);

/* Say yes to an invitation: we are a member, the others are told, and we connect to the members we have no stream with. */
int puddle_group_accept(puddle_ctx *ctx, const char *gid, const char **code);

/* Say no: whoever invited us is told. */
int puddle_group_decline(puddle_ctx *ctx, const char *gid, const char **code);

/* Leave a group we are in: the members are told. */
int puddle_group_leave(puddle_ctx *ctx, const char *gid, const char **code);

/* Change the group's name (any member may). */
int puddle_group_rename(puddle_ctx *ctx, const char *gid, const char *name, const char **code);

/* Send an event of ours (a message, an edit, a redaction: already built for this group as the chat) to every member. It is signed,
 * kept in our log, and queued for each member we have an open stream with. `sent` is how many it was queued for; a member we cannot
 * reach right now is not counted, and gets nothing. Returns 0 or a status; 500 `not_queued` means it was kept here but nothing was
 * queued for anyone. */
int puddle_group_send_event(puddle_ctx *ctx, const char *gid, const char *event, size_t len, char id[65], int *sent, const char **code);

/* Is there an open (friend or group-only), unblocked stream with `peer`? */
int puddle_group_open_with(puddle_ctx *ctx, const char *peer);

/* After anything that changes who is in which group: connect to members that join us in a group, and close group-only streams
 * whose last shared group has gone. Safe to call at any time. */
void puddle_group_reconcile(puddle_ctx *ctx);

#ifdef __cplusplus
}
#endif
#endif
