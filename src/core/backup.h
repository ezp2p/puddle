#ifndef PUDDLE_BACKUP_H
#define PUDDLE_BACKUP_H

/* A Puddle in one file, to keep somewhere safe or to move it to another computer (docs/backup.md).
 *
 * What goes in: everything in the data folder (keys, chats, friends, files, phones), the profiles, and theme.yaml; puddle.yaml
 * too, for reference. What stays out: the TLS certificate (it only carries bytes, and a new one is made) and the owner token
 * (a new one is made where the backup is restored).
 *
 * It is sealed to the Puddle's own encryption key, which the recovery phrase makes: so only the phrase opens it, and the file
 * can be kept anywhere. A backup made before a key change opens with the phrase from before it.
 *
 *     PUDDLE-BACKUP 1\n
 *     {"id":"<user id>","seq":<chain seq>,"made":<ms>,"enc_pub":"<hex>","enc":"<base64 HPKE enc>"}\n
 *     records: <u32 big-endian length><sealed bytes>, each 64 KiB of the payload but the last
 *
 * HPKE (base mode, the protocol's suite) to `enc_pub` with info "puddle-backup-v1\n<id>\n<seq>\n<made>"; the records are
 * sealed under the key it exports for "puddle-backup-v1", numbered from 0, the last marked as last, so a file cut short is
 * noticed. The payload: for each file, 'F', a u16 path length, the path ("data/...", "profiles/...", "theme.yaml",
 * "puddle.yaml"), a u64 size and the bytes; then 'E'. */

#include "core/config.h"
#include "core/identity.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char id[PUDDLE_USER_ID_LEN + 1];
    uint64_t seq;     /* which key generation sealed it: the phrase from that generation opens it */
    int64_t made_ms;
} puddle_backup_info;

/* Write the Puddle `cfg` describes, whose identity is `me`, to `path` (through a temporary file beside it). Files being
 * written while this runs may be caught part way; stop the Puddle first for a backup that is exactly one moment. With `extra`
 * (may be NULL), each file it names is taken from there instead: `extra(path_in_backup, &bytes, &len, user)` returns 1 to
 * replace it (bytes malloc'd, freed here), 0 to leave it. 0, or -1 with `err`. */
typedef int (*puddle_backup_replace_fn)(const char *path, char **bytes, size_t *len, void *user);
int puddle_backup_write(const puddle_config *cfg, puddle_identity *me, const char *path, puddle_backup_replace_fn extra, void *user,
                        char *err, size_t errlen);

/* What a backup says about itself, without opening it. 0, or -1 with `err` (not a backup). */
int puddle_backup_info_read(const char *path, puddle_backup_info *out, char *err, size_t errlen);

/* Is the file at `path` a backup (rather than, say, a chain)? 1 or 0. */
int puddle_backup_is(const char *path);

/* Open the backup at `path` with `phrase` and put its Puddle where `cfg` keeps one: the data folder, the profiles beside the
 * config (one of the same name is replaced), theme.yaml if there is none, and its puddle.yaml as puddle.yaml.from-backup (this
 * computer's own settings stay). Refuses if an identity is already there. Nothing is put in place unless the whole file opened.
 * 0, or -1 with `err`. */
int puddle_backup_restore(const puddle_config *cfg, const char *path, const char *phrase, puddle_backup_info *out, char *err,
                          size_t errlen);

#ifdef __cplusplus
}
#endif
#endif
