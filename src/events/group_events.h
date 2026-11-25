#ifndef PUDDLE_GROUP_EVENTS_H
#define PUDDLE_GROUP_EVENTS_H

/* The events that make and change a group chat (docs/groups.md), and the rules that fold them into who is in it.
 *
* A group is a chat whose id is made from its creator, a time and a random nonce, and written into its first event,
 * `chat.create`, which every member can check. Messages, edits and redactions are the same events as in a direct message,
 * with the group's id as the chat. Seven more events change the group:
 *
 *   chat.create   the creator; `name`, `nonce`
 *   chat.rename   any member; `body` is the new name
 *   member.invite any member, naming the invitee in `member`
 *   member.join   the invitee, saying yes
 *   member.leave  any member or invitee, for themselves
 *   member.remove the creator, or whoever invited `member`: takes them out (or takes the invitation back)
 *   member.vote   any member, against `member`: more than half of the other members voting takes them out
 *
 * There is no one in charge. What each Puddle believes about the members is worked out from these by the same rules, so
 * the answer is the same on every Puddle that has seen the same events, in whatever order they came. */

#include "core/group.h"
#include "core/identity.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The id of a group: base32(SHA-256("group\n" creator "\n" ts "\n" nonce)). 0, or -1 for bad input. */
int puddle_group_chat_id(const char *creator, int64_t ts, const char *nonce, char out[PUDDLE_USER_ID_LEN + 1]);
/* 32 random hex characters for a nonce. */
int puddle_group_nonce(char out[33]);

typedef enum { PUDDLE_GE_CREATE, PUDDLE_GE_RENAME, PUDDLE_GE_INVITE, PUDDLE_GE_JOIN, PUDDLE_GE_LEAVE, PUDDLE_GE_REMOVE, PUDDLE_GE_VOTE } puddle_ge_type;

typedef struct {
    puddle_ge_type type;
    char chat[PUDDLE_USER_ID_LEN + 1];
    char sender[PUDDLE_USER_ID_LEN + 1];
    int64_t ts;
    char name[PUDDLE_GROUP_NAME_MAX_BYTES + 1]; /* create, rename */
    char nonce[33];                             /* create */
    char member[PUDDLE_USER_ID_LEN + 1];        /* invite: the invitee; remove, vote: the one to take out */
} puddle_gevent;

/* Is `name` a group name: 1 to 100 characters, one line of valid text? */
int puddle_group_name_ok(const char *name);

/* Build events (compact JSON, NUL-terminated) into `out`; the length, or 0 if it does not fit or the input is not acceptable. */
size_t puddle_group_create_build(const char *creator, int64_t ts, const char *nonce, const char *name, char chat_out[PUDDLE_USER_ID_LEN + 1],
                                 char *out, size_t cap);
size_t puddle_group_rename_build(const char *chat, const char *sender, int64_t ts, const char *name, char *out, size_t cap);
size_t puddle_group_invite_build(const char *chat, const char *sender, int64_t ts, const char *invitee, char *out, size_t cap);
size_t puddle_group_join_build(const char *chat, const char *sender, int64_t ts, char *out, size_t cap);
size_t puddle_group_leave_build(const char *chat, const char *sender, int64_t ts, char *out, size_t cap);
size_t puddle_group_remove_build(const char *chat, const char *sender, int64_t ts, const char *member, char *out, size_t cap);
size_t puddle_group_vote_build(const char *chat, const char *sender, int64_t ts, const char *member, char *out, size_t cap);

/* Read a group event, strictly. 0 (and *why NULL), or -1 with a reason code in *why (`unsupported_type` for anything that is not
 * one of the seven, such as a message, so a caller can go on to read it as a chat event). A create is also checked: its chat is
 * the id its creator, time and nonce make. */
int puddle_group_event_parse(const char *event, size_t len, puddle_gevent *g, const char **why);

/* A group as it begins for its creator: just the creator, as a member. */
int puddle_group_init_created(puddle_group *g, const puddle_gevent *create, int64_t now_ms);

/* Fold one membership event into `g` (never a create). The rules, so every Puddle agrees:
 *  - rename: from a member; the newer one wins
 *  - invite: from a member, naming someone not already in; a leaver may be invited again by a later event
 *  - join: only from someone invited, and only the answer to an invitation older than it
 *  - leave: from a member or an invited person, for themselves; an older one than their last change is stale
 *  - remove: from a member who made the group or invited the one removed; older than that invitation is stale
 *  - vote: from a member, against anyone else in or invited, newer than their invitation; once the votes that count are more
 *    than half of the members other than the one voted against, they are out (puddle_group_settle_votes, after every change)
 *  - never more than 50 members and invited together
 * Returns 0 if it changed `g`, 1 if it was fine but changed nothing (already so, or stale), -1 if it is not allowed, with a reason
 * code in *why (`not_a_member`, `not_invited`, `not_allowed`, `group_full`, `bad_event`). */
int puddle_group_apply(puddle_group *g, const puddle_gevent *e, const char **why);

#ifdef __cplusplus
}
#endif
#endif
