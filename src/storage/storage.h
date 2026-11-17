#ifndef PUDDLE_STORAGE_H
#define PUDDLE_STORAGE_H

/* Where the Puddle keeps what it keeps, without saying how: one interface with a driver behind it. core/ decides what
 * its data means and turns it into bytes; storage only keeps the bytes, by name.
 *
 *   records  one document per key, in named collections: stream records, groups, the outbox, people, conversations,
 *            and single documents such as the block list (collection "state")
 *   secrets  small values only this Puddle may read: the owner token
 *
 * Two drivers:
 *   files    the default: plain files under the data directory, the layout docs/storage.md describes
 *   memory   everything in memory, gone when closed: for tests, which then need no folder
 *
 * The chat logs (storage/store.h) and media (storage/media.h) are storage too, with their own interfaces; they have
 * only the files driver so far. A driver is safe to use from several threads at once. */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct puddle_storage puddle_storage;

/* Records and secrets as files under `data_dir`:
 *   collection "state"   <data_dir>/state/<key>                 (key is the file name: "blocks.tsv")
 *   any other one        <data_dir>/state/<collection>/<key>.json
 *   secrets              <data_dir>/state/<name>, readable by the owner only
 * NULL with a message in `err` if the folder cannot be made. */
puddle_storage *puddle_storage_open_files(const char *data_dir, char *err, size_t errlen);

/* Everything in memory, gone when closed. */
puddle_storage *puddle_storage_open_memory(void);

void puddle_storage_close(puddle_storage *s);

/* A collection or key is a name: letters, digits, '-', '_' and '.', not starting with '.', at most 128. Anything
 * else is refused (-1, errno EINVAL), so a key can never reach outside its collection. */

/* Keep `data` under `key`, replacing what was there. All or nothing: after a crash a record is either the old one or
 * the new one. 0 or -1. */
int puddle_record_put(puddle_storage *s, const char *collection, const char *key, const void *data, size_t len);

/* 0 and the record (malloc'd, NUL-terminated past `len`), 1 if there is none, -1 on error. */
int puddle_record_get(puddle_storage *s, const char *collection, const char *key, char **data, size_t *len);

/* 0 (also if there was none) or -1. */
int puddle_record_remove(puddle_storage *s, const char *collection, const char *key);

/* Every record in `collection`, in no particular order. `fn` returns nonzero to stop early. 0, or -1 if the collection
 * could not be read. A record that cannot be read is skipped. */
typedef int (*puddle_record_fn)(void *user, const char *key, const char *data, size_t len);
int puddle_record_each(puddle_storage *s, const char *collection, puddle_record_fn fn, void *user);

/* Secrets: as records, kept where only the owner can read them. */
int puddle_secret_put(puddle_storage *s, const char *name, const void *data, size_t len);
int puddle_secret_get(puddle_storage *s, const char *name, char **data, size_t *len); /* 0, 1 none, -1 */

#ifdef __cplusplus
}
#endif
#endif
