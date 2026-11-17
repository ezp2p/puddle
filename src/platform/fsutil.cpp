#include "platform/fsutil.h"

#include "platform/process.h"

#include <atomic>
#include <cerrno>
#include <filesystem>
#include <new>
#include <stdio.h>
#include <string>

/* What is the same on every OS, built on the calls in fs_posix.cpp / fs_windows.cpp. */

int puddle_fd_write_all(int fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    while (len) {
        int64_t w = puddle_fd_write(fd, p, len);
        if (w <= 0)
            return -1;
        p += w;
        len -= (size_t)w;
    }
    return 0;
}

int puddle_write_file_atomic(const char *dir, const char *final_path, const void *data, size_t len)
{
    /* a name no other write, in this process or another, is using at the same moment */
    static std::atomic<unsigned long> counter{ 0 };
    char tmp[PUDDLE_PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s/.tmp-%ld-%lu", dir, (long)puddle_process_id(), ++counter);
    int fd = puddle_fd_open(tmp, PUDDLE_F_WRITE | PUDDLE_F_CREATE | PUDDLE_F_TRUNC);
    if (fd < 0)
        return -1;
    if (puddle_fd_write_all(fd, data, len) != 0) {
        puddle_fd_close(fd);
        puddle_remove_file(tmp);
        return -1;
    }
    int rc = puddle_fsync_fd(fd);
    puddle_fd_close(fd);
    if (rc != 0 || puddle_rename(tmp, final_path) != 0) {
        puddle_remove_file(tmp);
        return -1;
    }
    puddle_fsync_dir(dir);
    return 0;
}

/* ---- reading a directory ---- */

struct puddle_dir {
    std::filesystem::directory_iterator it;
    std::string name;
};

puddle_dir *puddle_dir_open(const char *path)
{
    std::error_code ec;
    std::filesystem::directory_iterator it(path, ec);
    if (ec) {
        errno = ec.value();
        return NULL;
    }
    return new (std::nothrow) puddle_dir{ std::move(it), std::string() };
}

const char *puddle_dir_next(puddle_dir *d)
{
    std::error_code ec;
    if (d->it == std::filesystem::directory_iterator())
        return NULL;
    d->name = d->it->path().filename().string();
    d->it.increment(ec);
    if (ec)
        d->it = std::filesystem::directory_iterator(); /* a directory that changed under us ends here rather than failing midway */
    return d->name.c_str();
}

void puddle_dir_close(puddle_dir *d)
{
    delete d;
}
