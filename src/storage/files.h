#ifndef PUDDLE_FILES_H
#define PUDDLE_FILES_H

/* Files sent in messages, kept by their id (docs/files.md). A file is split into chunks of PUDDLE_FILE_CHUNK bytes (the last
 * one shorter); each chunk has a SHA-256, and the file's id is the SHA-256 of all the chunk hashes (32 bytes each, in order).
 * So a file can be fetched a chunk at a time from anyone who has it, each chunk checked as it arrives, and picked up again
 * where it stopped. Profile pictures are not kept here (storage/media.h).
 *
 *   <data_dir>/files/<id>                  a whole file
 *   <data_dir>/files/<id>.chunks           its chunk hashes, 32 bytes each
 *   <data_dir>/files/partial/<id>          a download in progress, written where each chunk goes
 *   <data_dir>/files/partial/<id>.chunks   its chunk hashes (checked against the id before anything is fetched)
 *   <data_dir>/files/partial/<id>.have     one byte per chunk: 1 once that chunk is here and checked
 *   <data_dir>/files/partial/<id>.size     its size, in decimal
 *   <data_dir>/files/uploads/<token>...    the same three for a file the owner is sending us, before it has an id
 *   <data_dir>/files/refs/<id>             which messages carry it: "made <ms>", then "<chat> <message>" a line each
 *   <data_dir>/files/notes/<id>            what passed between Puddles over it, a line each (federation/files.cpp)
 *
 * Safe to use from several threads at once. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PUDDLE_FILE_CHUNK (1u << 20)
#define PUDDLE_FILE_ID_LEN 64          /* hex */
#define PUDDLE_UPLOAD_TOKEN_LEN 32     /* hex */
#define PUDDLE_FILE_HARD_MAX (64LL << 30) /* no file is ever larger, whatever the owner allows: 65536 chunks */

typedef struct puddle_files puddle_files;

/* NULL with a message in `err` if the folders cannot be made. */
puddle_files *puddle_files_open(const char *data_dir, char *err, size_t errlen);
void puddle_files_close(puddle_files *f);

/* Is `id` 64 lowercase hex? Is `token` 32? */
int puddle_file_id_valid(const char *id);
int puddle_upload_token_valid(const char *token);

/* How many chunks a file of `size` bytes has, and how long chunk `n` of it is. */
uint32_t puddle_file_chunk_count(int64_t size);
size_t puddle_file_chunk_len(int64_t size, uint32_t n);

/* ---- what is here ---- */

typedef enum { PUDDLE_FILE_ABSENT, PUDDLE_FILE_PARTIAL, PUDDLE_FILE_WHOLE } puddle_file_state;

/* Whether the file is here, its size, and how many of its bytes are (all of them when whole). Either pointer may be NULL. */
puddle_file_state puddle_files_state(puddle_files *f, const char *id, int64_t *size, int64_t *have);

/* The path of a whole file, to read or send. 0, or 1 if it is not here whole. */
int puddle_files_path(puddle_files *f, const char *id, char *out, size_t cap);

/* Its chunk hashes (malloc'd, 32 bytes each) and size, from a whole file or one being fetched. 0, or 1 if it is not here. */
int puddle_files_chunk_list(puddle_files *f, const char *id, int64_t *size, unsigned char **hashes, uint32_t *count);

/* Chunk `n` (malloc'd), from a whole file or from a download that already has it. 0, 1 not here, -1 on error. */
int puddle_files_read_chunk(puddle_files *f, const char *id, uint32_t n, unsigned char **data, size_t *len);

/* Throw the file away, whole or partial. 0 (also if it was not here) or -1. */
int puddle_files_remove(puddle_files *f, const char *id);

/* Every file here, whole or partial: `fn` per id, until it returns nonzero. */
typedef int (*puddle_file_fn)(void *user, const char *id, puddle_file_state state);
void puddle_files_each(puddle_files *f, puddle_file_fn fn, void *user);

/* ---- which messages carry it: a file goes with the last of them (docs/files.md) ---- */

/* Message `message` in `chat` carries the file (again is fine). The file need not be here yet. 0 or -1. */
int puddle_files_ref(puddle_files *f, const char *id, const char *chat, const char *message, int64_t now_ms);

/* It no longer does (it was deleted). How many messages still carry it, or -1. At 0 the caller lets the file go. */
int puddle_files_unref(puddle_files *f, const char *id, const char *chat, const char *message);

/* How many messages carry it. */
int puddle_files_ref_count(puddle_files *f, const char *id);

/* Throw away every file here (whole or partial) that no message has carried for `grace_ms`: one uploaded and never sent,
 * or whose messages were all deleted while it was arriving. `gone(user, id)` hears of each. Returns how many. */
int puddle_files_sweep(puddle_files *f, int64_t now_ms, int64_t grace_ms, void (*gone)(void *user, const char *id), void *user);

/* How many files are here, and the bytes they take (partial ones by what has arrived). */
void puddle_files_usage(puddle_files *f, int64_t *count, int64_t *bytes);

/* ---- notes: what passed between Puddles over a file (who it was delivered to, who refused it, who asked for it again) ----
 * Lines of text without newlines, kept with the file and thrown away with it (puddle_files_remove). What they say is the
 * caller's; federation/files.cpp writes and reads them. */

/* Add `line` (at most 400 bytes, no newline) unless it is there already. 0 or -1. */
int puddle_files_note_add(puddle_files *f, const char *id, const char *line);
/* Every line beginning with `prefix` (all for ""): `fn` per line, until it returns nonzero. */
void puddle_files_note_each(puddle_files *f, const char *id, const char *prefix, int (*fn)(void *user, const char *line), void *user);
/* Take away every line beginning with `prefix`. How many went, or -1. */
int puddle_files_note_remove(puddle_files *f, const char *id, const char *prefix);

/* ---- fetching one from another Puddle ---- */

/* Get ready to fetch the file `id` of `size` bytes whose chunk hashes are `hashes`: they must hash to the id and their number
 * fit the size. Nothing changes if it is already here or being fetched. 0, or -1 if the list is not this file's. */
int puddle_files_fetch_begin(puddle_files *f, const char *id, int64_t size, const unsigned char *hashes, uint32_t count);

/* Chunk `n` arrived: kept only if it is the right length and hashes to what the list says. 0, 1 if no such download,
 * -1 if the chunk is not this one (it is thrown away), -2 on a write error. */
int puddle_files_fetch_put(puddle_files *f, const char *id, uint32_t n, const unsigned char *data, size_t len);

/* The first chunk a download still needs, or -1 if it has them all (or there is no such download). */
int64_t puddle_files_fetch_next(puddle_files *f, const char *id);

/* Every chunk is here: the download becomes a whole file. 0, 1 if chunks are still missing or there is no such download, -1. */
int puddle_files_fetch_finish(puddle_files *f, const char *id);

/* ---- taking one from the owner ---- */

/* Take a file the owner picked on this computer, open as `src` (handed over by the app in the same process, never named by
 * a path from the network): it becomes an upload with every chunk here, as a copy-on-write clone where the file system
 * can, else a copy (platform/fsutil.h). The Puddle never reads the original again; moving, editing or deleting it changes
 * nothing that was sent. Finish it with puddle_files_upload_finish. `cloned` (may be NULL) says which it was. 0, -1 on
 * error, -2 if it is empty or over `max_size` (0 = PUDDLE_FILE_HARD_MAX). */
int puddle_files_upload_adopt(puddle_files *f, int src, int64_t max_size, char token[PUDDLE_UPLOAD_TOKEN_LEN + 1], int *cloned);

/* Start taking a file of `size` bytes (1 to PUDDLE_FILE_HARD_MAX): a new upload token. 0, or -1. */
int puddle_files_upload_create(puddle_files *f, int64_t size, char token[PUDDLE_UPLOAD_TOKEN_LEN + 1]);

/* Chunk `n` of an upload: it must be exactly as long as that chunk of a file this size. Sending one again replaces it.
 * 0, 1 if there is no such upload, -1 if the chunk does not fit (wrong number or length), -2 on a write error. */
int puddle_files_upload_put(puddle_files *f, const char *token, uint32_t n, const unsigned char *data, size_t len);

/* Its size and how many chunks have arrived. 0, or 1 if there is no such upload. */
int puddle_files_upload_info(puddle_files *f, const char *token, int64_t *size, uint32_t *have, uint32_t *count);

/* Every chunk is here: hash it into a whole file, and say its id. If that file is here already, the upload is just dropped.
 * 0, 1 if there is no such upload, 2 if chunks are missing (nothing changes), -1 on error. */
int puddle_files_upload_finish(puddle_files *f, const char *token, char id[PUDDLE_FILE_ID_LEN + 1]);

/* Throw an upload away. 0 (also if there was none). */
int puddle_files_upload_cancel(puddle_files *f, const char *token);

/* Uploads untouched for longer than `max_age_ms` (an owner who never finished): thrown away. Returns how many. */
int puddle_files_upload_expire(puddle_files *f, int64_t now_ms, int64_t max_age_ms);

#ifdef __cplusplus
}
#endif
#endif
