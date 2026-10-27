#ifndef PUDDLE_FSUTIL_H
#define PUDDLE_FSUTIL_H

/* Files and directories, the same on every OS. The core opens, reads and writes plain descriptors through these
 * (on Windows they are the C runtime's), so nothing above this layer includes a system header for files.
 * Everything returns 0 / a descriptor / a count on success and -1 with errno set on failure, unless it says otherwise. */

#include <stddef.h>
#include <stdint.h>

#define PUDDLE_PATH_MAX 1024 /* the longest path the Puddle builds */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- descriptors ---- */

enum {
    PUDDLE_F_READ = 1,
    PUDDLE_F_WRITE = 2,
    PUDDLE_F_CREATE = 4,   /* made if missing, readable by the owner only */
    PUDDLE_F_EXCL = 8,     /* with CREATE: fail if it exists */
    PUDDLE_F_TRUNC = 16,
    PUDDLE_F_APPEND = 32
};

/* Open `path`. Never inherited by a child process. */
int puddle_fd_open(const char *path, int flags);
int puddle_fd_close(int fd);
/* Bytes read (0 at the end) or written; -1 on error. Interrupted calls are retried. */
int64_t puddle_fd_read(int fd, void *buf, size_t len);
int64_t puddle_fd_write(int fd, const void *buf, size_t len);
/* Write all of it, or fail. */
int puddle_fd_write_all(int fd, const void *buf, size_t len);
/* The end of the file, which is where the next append lands; -1 on error. */
int64_t puddle_fd_end(int fd);
int puddle_fd_truncate(int fd, int64_t len);
/* The size of an open regular file; -1 if it is not one. */
int64_t puddle_fd_size(int fd);
/* Take an exclusive lock on the whole file, without waiting: 0, or -1 with errno EWOULDBLOCK if another process holds
 * it. Released when the descriptor is closed. */
int puddle_fd_lock(int fd);
/* Flush a file to stable storage (F_FULLFSYNC on macOS, which plain fsync() does not guarantee). */
int puddle_fsync_fd(int fd);

/* ---- paths ---- */

int puddle_path_exists(const char *path);          /* 1 or 0 */
int puddle_path_is_dir(const char *path);          /* 1 or 0 */
int puddle_path_is_file(const char *path);         /* a regular file: 1 or 0 */
int64_t puddle_path_size(const char *path);         /* of a regular file; -1 if not one */
int puddle_remove_file(const char *path);           /* 0, also if it was not there */
int puddle_remove_dir(const char *path);            /* an empty directory */
/* Rename `from` to `to`, replacing `to` if it exists, in one step. */
int puddle_rename(const char *from, const char *to);

/* mkdir -p, readable by the owner only. */
int puddle_mkdir_p(const char *path);

/* Write `data` to `final_path` so a crash leaves either the old file or the new one, never half:
 * temp file in `dir`, flush, rename, flush the directory. Owner-only. Safe to call from several threads at once,
 * for the same directory. */
int puddle_write_file_atomic(const char *dir, const char *final_path, const void *data, size_t len);

/* Flush a directory entry, so a newly created file survives a crash. (Nothing to do on Windows.) */
int puddle_fsync_dir(const char *path);

/* The names in a directory, one at a time, without "." and "..", the same on every OS:
 *     puddle_dir *d = puddle_dir_open(path);   // NULL with errno set if it can't be read
 *     const char *name;
 *     while ((name = puddle_dir_next(d))) ...  // valid until the next call
 *     puddle_dir_close(d);
 * Order is whatever the file system gives. */
typedef struct puddle_dir puddle_dir;
puddle_dir *puddle_dir_open(const char *path);
const char *puddle_dir_next(puddle_dir *d);
void puddle_dir_close(puddle_dir *d);

#ifdef __cplusplus
}
#endif
#endif
