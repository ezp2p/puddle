/* platform/fsutil.h on macOS, Linux and the BSDs. */
#include "platform/fsutil.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

int puddle_fd_open(const char *path, int flags)
{
    int o = O_CLOEXEC;
    if ((flags & PUDDLE_F_READ) && (flags & PUDDLE_F_WRITE))
        o |= O_RDWR;
    else if (flags & PUDDLE_F_WRITE)
        o |= O_WRONLY;
    else
        o |= O_RDONLY;
    if (flags & PUDDLE_F_CREATE) o |= O_CREAT;
    if (flags & PUDDLE_F_EXCL) o |= O_EXCL;
    if (flags & PUDDLE_F_TRUNC) o |= O_TRUNC;
    if (flags & PUDDLE_F_APPEND) o |= O_APPEND;
    return open(path, o, 0600);
}

int puddle_fd_close(int fd)
{
    return close(fd);
}

int64_t puddle_fd_read(int fd, void *buf, size_t len)
{
    for (;;) {
        ssize_t r = read(fd, buf, len);
        if (r < 0 && errno == EINTR)
            continue;
        return r;
    }
}

int64_t puddle_fd_write(int fd, const void *buf, size_t len)
{
    for (;;) {
        ssize_t w = write(fd, buf, len);
        if (w < 0 && errno == EINTR)
            continue;
        return w;
    }
}

int64_t puddle_fd_end(int fd)
{
    return lseek(fd, 0, SEEK_END);
}

int puddle_fd_truncate(int fd, int64_t len)
{
    return ftruncate(fd, (off_t)len);
}

int64_t puddle_fd_size(int fd)
{
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
        return -1;
    return st.st_size;
}

int puddle_fd_lock(int fd)
{
    return flock(fd, LOCK_EX | LOCK_NB);
}

int puddle_fsync_fd(int fd)
{
#ifdef F_FULLFSYNC
    if (fcntl(fd, F_FULLFSYNC) == 0)
        return 0;
#endif
    return fsync(fd);
}

int puddle_fsync_dir(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    int rc = puddle_fsync_fd(fd);
    close(fd);
    return rc;
}

int puddle_path_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

int puddle_path_is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int puddle_path_is_file(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

int64_t puddle_path_size(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return -1;
    return st.st_size;
}

int puddle_remove_file(const char *path)
{
    return unlink(path) == 0 || errno == ENOENT ? 0 : -1;
}

int puddle_remove_dir(const char *path)
{
    return rmdir(path);
}

int puddle_rename(const char *from, const char *to)
{
    return rename(from, to);
}

int puddle_mkdir_p(const char *path)
{
    char buf[PUDDLE_PATH_MAX];
    if (strlen(path) >= sizeof buf) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(buf, path);
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(buf, 0700) != 0 && errno != EEXIST)
            return -1;
        *p = '/';
    }
    if (mkdir(buf, 0700) != 0 && errno != EEXIST)
        return -1;
    return 0;
}
