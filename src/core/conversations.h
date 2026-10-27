#ifndef PUDDLE_CONVERSATIONS_H
#define PUDDLE_CONVERSATIONS_H

/* One summary per conversation (test_structure/model/records/conversation.yaml): its last message, sort time and
 * unread count, worked out from the end of its chat log, and when the owner last read it. Collection
 * "conversations", keyed by the conversation id (the other person's user id for a DM, the group id for a group).
 *
 * The summary is derived and can always be rebuilt from the log; only `read_at` is the owner's own. A summary is
 * current only while nothing new has reached its chat: the chat log store tells this one of every append
 * (puddle_conversations_appended, wired by the service), and a summary built before the latest append is stale. After
 * a start every summary is stale until rebuilt once, so a summary can be out of date for at most one read. */

#include "core/identity.h"
#include "storage/storage.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PUDDLE_CONV_PREVIEW_MAX 500 /* bytes */

typedef enum { PUDDLE_CONV_DM = 0, PUDDLE_CONV_GROUP = 1 } puddle_conv_kind;

typedef struct {
    char id[PUDDLE_USER_ID_LEN + 1];
    char chat[PUDDLE_USER_ID_LEN + 1]; /* the chat log it reads; a DM's is a hash of both user ids */
    puddle_conv_kind kind;
    int64_t sort_ts;
    int64_t read_at; /* when the owner last read it, in this Puddle's clock; 0 = never */
    int unread;
    int has_last;    /* the rest is set only if it has a last message */
    char last_id[65];
    char last_sender[65];
    int64_t last_ts;
    char last_preview[PUDDLE_CONV_PREVIEW_MAX + 1];
    int last_deleted;
} puddle_conv_record;

typedef struct puddle_conversations puddle_conversations;

/* The first form keeps them as files under `data_dir` (<data_dir>/state/conversations/<id>.json); the second uses
 * `storage`, which must outlive the store. NULL with a message in `err`. */
puddle_conversations *puddle_conversations_open(const char *data_dir, char *err, size_t errlen);
puddle_conversations *puddle_conversations_open_with(puddle_storage *storage, char *err, size_t errlen);
void puddle_conversations_close(puddle_conversations *c);

/* Something new reached `chat`: its summary is now stale. The chat log store's listener (puddle_store_on_append). */
void puddle_conversations_appended(void *c, const char *chat);

/* How many appends `chat` has had since the store opened. Read it before reading the log, and hand it to put. */
uint64_t puddle_conversations_generation(puddle_conversations *c, const char *chat);

/* 0 and the record, 1 none, -1 not a valid id or damaged. `*fresh` says whether it was built from the log as it is now. */
int puddle_conversations_get(puddle_conversations *c, const char *id, puddle_conv_record *out, int *fresh);

/* Keep `rec`, built from the log as it was at `generation`. 0 or -1. */
int puddle_conversations_put(puddle_conversations *c, const puddle_conv_record *rec, uint64_t generation);

/* The owner read it at `read_at` (an earlier time than the one held changes nothing). Its unread count is then
 * stale. A conversation with no record yet gets one, stale, holding just this. 0 or -1. */
int puddle_conversations_set_read(puddle_conversations *c, const char *id, const char *chat, puddle_conv_kind kind, int64_t read_at);

#ifdef __cplusplus
}
#endif
#endif
