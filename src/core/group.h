#ifndef PUDDLE_GROUP_H
#define PUDDLE_GROUP_H

/* Group chats (docs/groups.md): what a Puddle remembers about the groups it is in or invited to, and where it keeps
 * it. The events that make and change a group, and the rules that fold them into who is in it, are
 * events/group_events.h. */

#include "core/identity.h"
#include "core/link.h"
#include "storage/storage.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PUDDLE_GROUP_NAME_MAX_CHARS 100
#define PUDDLE_GROUP_NAME_MAX_BYTES (PUDDLE_GROUP_NAME_MAX_CHARS * 4)
#define PUDDLE_GROUP_MAX_MEMBERS 50     /* members and the invited together */
#define PUDDLE_GROUP_MAX_PENDING_IN 50  /* invitations waiting for the owner */
#define PUDDLE_GROUP_CREATE_MAX 1024    /* bytes of a create event (a name of 100 characters is at most a few hundred) */

/* ---- what we remember about a group ---- */

typedef enum { PUDDLE_GM_INVITED, PUDDLE_GM_MEMBER, PUDDLE_GM_LEFT } puddle_gm_state;
typedef struct {
    char id[PUDDLE_USER_ID_LEN + 1];
    puddle_gm_state state;
    char by[PUDDLE_USER_ID_LEN + 1]; /* who invited them; "" for the creator */
    int64_t ts;                      /* of the event that put them in this state */
    char address[PUDDLE_ADDR_MAX];   /* where the one who invited us said to reach them, "" if not told; only kept to connect once */
    int told;                        /* we have sent them our own member.join (they cannot learn it from anyone else) */
} puddle_gmember;

/* Where we stand in a group. */
typedef enum {
    PUDDLE_GROUP_INVITED,  /* someone invited us; the owner has not answered */
    PUDDLE_GROUP_IN,       /* we are a member */
    PUDDLE_GROUP_LEFT,     /* we left */
    PUDDLE_GROUP_DECLINED  /* we said no to the invitation */
} puddle_group_my;

typedef struct {
    char id[PUDDLE_USER_ID_LEN + 1];
    char name[PUDDLE_GROUP_NAME_MAX_BYTES + 1];
    int64_t name_ts;
    char creator[PUDDLE_USER_ID_LEN + 1];
    int64_t created_ms, updated_ms;
    puddle_group_my my;
    char invited_by[PUDDLE_USER_ID_LEN + 1]; /* when my == INVITED */
    char create_event[PUDDLE_GROUP_CREATE_MAX + 1]; /* the chat.create, to hand to those we invite */
    char create_sig[180];
    char join_event[PUDDLE_GROUP_CREATE_MAX + 1];  /* our own member.join, kept to show the members we connect to (we were invited, not asked) */
    char join_sig[180];
    puddle_gmember *members;                 /* everyone we know of, us included */
    size_t nmembers;
} puddle_group;

const char *puddle_group_my_name(puddle_group_my m);
const char *puddle_gm_state_name(puddle_gm_state s);

void puddle_group_free(puddle_group *g);
puddle_gmember *puddle_group_member(const puddle_group *g, const char *id); /* NULL if not listed */
puddle_gmember *puddle_group_add_member(puddle_group *g, const char *id); /* a blank member at the end; NULL if out of memory */
size_t puddle_group_count(const puddle_group *g, puddle_gm_state state);

/* ---- the store ---- */

typedef struct puddle_groups puddle_groups;

puddle_groups *puddle_groups_open(const char *data_dir, char *err, size_t errlen); /* <data_dir>/state/groups/ */
puddle_groups *puddle_groups_open_with(puddle_storage *storage, char *err, size_t errlen); /* storage outlives it */
void puddle_groups_close(puddle_groups *s);

/* 0 found (free with puddle_group_free), 1 no such group, -1 not a valid id. */
int puddle_groups_get(puddle_groups *s, const char *id, puddle_group *out);
/* Store a record (new or replacing). 0 or -1. */
int puddle_groups_put(puddle_groups *s, const puddle_group *g, char *err, size_t errlen);
/* Change one group's record without losing anyone else's change to it, like puddle_streams_update: `fn` sees the stored record
 * (`exists` 1) or an empty one (`exists` 0) and returns 0 to store it, >0 to store nothing (returned), <0 to fail. */
typedef int (*puddle_group_update_fn)(void *user, puddle_group *g, int exists);
int puddle_groups_update(puddle_groups *s, const char *id, puddle_group_update_fn fn, void *user, char *err, size_t errlen);
/* Every group. Free with puddle_group_list_free. */
int puddle_groups_list(puddle_groups *s, puddle_group **out, size_t *count);
void puddle_group_list_free(puddle_group *list, size_t count);
/* How many groups are waiting for us to answer an invitation. */
size_t puddle_groups_pending(puddle_groups *s);

#ifdef __cplusplus
}
#endif
#endif
