/* platform/fsutil.h on Windows. NOT YET BUILT OR TESTED ON WINDOWS: written on a Mac, to be checked on a Windows machine.
 *
 * Paths are UTF-8 everywhere in Puddle, so each one is turned into UTF-16 for the wide Windows calls. Descriptors are
 * the C runtime's (_wsopen_s and friends), so the rest of Puddle uses them exactly as it does on POSIX. A new file
 * gets the access rights of its folder; Puddle's folders live under the user's own profile, which only they can read. */
#include "platform/fsutil.h"

#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <limits.h>
#include <share.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <windows.h>

#include <string>

namespace {

std::wstring wide(const char *utf8)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (n <= 0)
        return std::wstring();
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &out[0], n);
    out.resize((size_t)n - 1);
    return out;
}

void set_errno_from_last_error(void)
{
    switch (GetLastError()) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND: errno = ENOENT; break;
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS:
    case ERROR_DIR_NOT_EMPTY: errno = EEXIST; break;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION: errno = EACCES; break;
    case ERROR_LOCK_VIOLATION: errno = EWOULDBLOCK; break;
    default: errno = EIO; break;
    }
}

int stat_of(const char *path, struct _stat64 *st)
{
    std::wstring w = wide(path);
    return w.empty() ? -1 : _wstat64(w.c_str(), st);
}

} // namespace

int puddle_fd_open(const char *path, int flags)
{
    int o = _O_BINARY | _O_NOINHERIT;
    if ((flags & PUDDLE_F_READ) && (flags & PUDDLE_F_WRITE))
        o |= _O_RDWR;
    else if (flags & PUDDLE_F_WRITE)
        o |= _O_WRONLY;
    else
        o |= _O_RDONLY;
    if (flags & PUDDLE_F_CREATE) o |= _O_CREAT;
    if (flags & PUDDLE_F_EXCL) o |= _O_EXCL;
    if (flags & PUDDLE_F_TRUNC) o |= _O_TRUNC;
    if (flags & PUDDLE_F_APPEND) o |= _O_APPEND;
    std::wstring w = wide(path);
    int fd = -1;
    if (w.empty()) {
        errno = EINVAL;
        return -1;
    }
    if (_wsopen_s(&fd, w.c_str(), o, _SH_DENYNO, _S_IREAD | _S_IWRITE) != 0)
        return -1;
    return fd;
}

int puddle_fd_close(int fd)
{
    return _close(fd);
}

int64_t puddle_fd_read(int fd, void *buf, size_t len)
{
    return _read(fd, buf, (unsigned)(len > INT_MAX ? INT_MAX : len));
}

int64_t puddle_fd_write(int fd, const void *buf, size_t len)
{
    return _write(fd, buf, (unsigned)(len > INT_MAX ? INT_MAX : len));
}

int64_t puddle_fd_end(int fd)
{
    return _lseeki64(fd, 0, SEEK_END);
}

int puddle_fd_truncate(int fd, int64_t len)
{
    return _chsize_s(fd, len) == 0 ? 0 : -1;
}

int64_t puddle_fd_size(int fd)
{
    struct _stat64 st;
    if (_fstat64(fd, &st) != 0 || !(st.st_mode & _S_IFREG))
        return -1;
    return st.st_size;
}

int puddle_fd_lock(int fd)
{
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    OVERLAPPED ov;
    memset(&ov, 0, sizeof ov);
    if (h == INVALID_HANDLE_VALUE) {
        errno = EBADF;
        return -1;
    }
    if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD, MAXDWORD, &ov)) {
        errno = GetLastError() == ERROR_LOCK_VIOLATION ? EWOULDBLOCK : EIO;
        return -1;
    }
    return 0; /* released when the descriptor (and so the handle) is closed */
}

int puddle_fsync_fd(int fd)
{
    return _commit(fd);
}

int puddle_fsync_dir(const char *path)
{
    (void)path; /* NTFS makes a new name durable with the file: there is no directory to flush */
    return 0;
}

int puddle_path_exists(const char *path)
{
    struct _stat64 st;
    return stat_of(path, &st) == 0;
}

int puddle_path_is_dir(const char *path)
{
    struct _stat64 st;
    return stat_of(path, &st) == 0 && (st.st_mode & _S_IFDIR);
}

int puddle_path_is_file(const char *path)
{
    struct _stat64 st;
    return stat_of(path, &st) == 0 && (st.st_mode & _S_IFREG);
}

int64_t puddle_path_size(const char *path)
{
    struct _stat64 st;
    if (stat_of(path, &st) != 0 || !(st.st_mode & _S_IFREG))
        return -1;
    return st.st_size;
}

int puddle_remove_file(const char *path)
{
    std::wstring w = wide(path);
    if (_wunlink(w.c_str()) == 0 || errno == ENOENT)
        return 0;
    return -1;
}

int puddle_remove_dir(const char *path)
{
    std::wstring w = wide(path);
    return _wrmdir(w.c_str());
}

int puddle_rename(const char *from, const char *to)
{
    /* replaces a file that is there; a folder that is there is never replaced (which is what the identity relies on) */
    std::wstring f = wide(from), t = wide(to);
    if (MoveFileExW(f.c_str(), t.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return 0;
    set_errno_from_last_error();
    return -1;
}

int puddle_mkdir_p(const char *path)
{
    char buf[PUDDLE_PATH_MAX];
    if (strlen(path) >= sizeof buf) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(buf, path);
    char *start = buf;
    if (buf[0] && buf[1] == ':')
        start = buf + 2; /* a drive, "C:", is not something to make */
    for (char *p = start + 1; *p; p++) {
        if (*p != '/' && *p != '\\')
            continue;
        char sep = *p;
        *p = '\0';
        std::wstring w = wide(buf);
        if (_wmkdir(w.c_str()) != 0 && errno != EEXIST)
            return -1;
        *p = sep;
    }
    std::wstring w = wide(buf);
    if (_wmkdir(w.c_str()) != 0 && errno != EEXIST)
        return -1;
    return 0;
}
