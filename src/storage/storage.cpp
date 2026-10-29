/* storage/storage.h: the interface, and its two drivers. */
#include "storage/storage.h"

#include "platform/fsutil.h"

#include <errno.h>
#include <map>
#include <mutex>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

/* What every driver does. The C functions below check names and hand over to it. */
struct puddle_storage {
    virtual ~puddle_storage() = default;
    virtual int put(const char *collection, const char *key, const void *data, size_t len, bool secret) = 0;
    virtual int get(const char *collection, const char *key, char **data, size_t *len) = 0;
    virtual int remove(const char *collection, const char *key) = 0;
    virtual int each(const char *collection, puddle_record_fn fn, void *user) = 0;
};

namespace {

constexpr const char *kSecrets = "state"; /* where secrets live, by name, in both drivers */

bool name_ok(const char *s)
{
    size_t n = s ? strlen(s) : 0;
    if (n == 0 || n > 128 || s[0] == '.')
        return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
            return false;
    }
    return true;
}

char *copy_out(const char *data, size_t len)
{
    char *out = (char *)malloc(len + 1);
    if (out) {
        memcpy(out, data, len);
        out[len] = '\0';
    }
    return out;
}

/* ---- files ---- */

class Files final : public puddle_storage {
public:
    explicit Files(std::string dataDir) : state_(std::move(dataDir) + "/state") {}

    int put(const char *collection, const char *key, const void *data, size_t len, bool) override
    {
        std::string dir = dirOf(collection);
        if (puddle_mkdir_p(dir.c_str()) != 0)
            return -1;
        /* owner-only either way: puddle_write_file_atomic creates every file so */
        return puddle_write_file_atomic(dir.c_str(), pathOf(collection, key).c_str(), data, len);
    }

    int get(const char *collection, const char *key, char **data, size_t *len) override
    {
        return readFile(pathOf(collection, key), data, len);
    }

    int remove(const char *collection, const char *key) override
    {
        return puddle_remove_file(pathOf(collection, key).c_str());
    }

    int each(const char *collection, puddle_record_fn fn, void *user) override
    {
        std::string dir = dirOf(collection);
        puddle_dir *d = puddle_dir_open(dir.c_str());
        if (!d)
            return errno == ENOENT ? 0 : -1; /* nothing kept yet */
        bool state = !strcmp(collection, "state");
        const char *name;
        int stop = 0;
        while (!stop && (name = puddle_dir_next(d))) {
            std::string file = name, key = file;
            if (!state) {
                if (file.size() <= 5 || file.compare(file.size() - 5, 5, ".json") != 0)
                    continue; /* temp files and anything that is not <key>.json */
                key = file.substr(0, file.size() - 5);
            }
            if (!name_ok(key.c_str()))
                continue;
            char *data;
            size_t len;
            if (readFile(dir + "/" + file, &data, &len) != 0)
                continue;
            stop = fn(user, key.c_str(), data, len);
            free(data);
        }
        puddle_dir_close(d);
        return 0;
    }

private:
    std::string dirOf(const char *collection) const
    {
        return !strcmp(collection, "state") ? state_ : state_ + "/" + collection;
    }

    std::string pathOf(const char *collection, const char *key) const
    {
        return !strcmp(collection, "state") ? state_ + "/" + key : state_ + "/" + collection + "/" + key + ".json";
    }

    static int readFile(const std::string &path, char **data, size_t *len)
    {
        *data = NULL;
        *len = 0;
        int fd = puddle_fd_open(path.c_str(), PUDDLE_F_READ);
        if (fd < 0)
            return errno == ENOENT ? 1 : -1;
        int64_t size = puddle_fd_size(fd);
        if (size < 0 || size > (int64_t)64 * 1024 * 1024) {
            puddle_fd_close(fd);
            errno = EFBIG;
            return -1;
        }
        char *buf = (char *)malloc((size_t)size + 1);
        size_t got = 0;
        while (buf && got < (size_t)size) {
            int64_t r = puddle_fd_read(fd, buf + got, (size_t)size - got);
            if (r <= 0)
                break;
            got += (size_t)r;
        }
        puddle_fd_close(fd);
        if (!buf)
            return -1;
        buf[got] = '\0';
        *data = buf;
        *len = got;
        return 0;
    }

    std::string state_;
};

/* ---- memory ---- */

class Memory final : public puddle_storage {
public:
    int put(const char *collection, const char *key, const void *data, size_t len, bool) override
    {
        std::lock_guard<std::mutex> hold(mu_);
        records_[collection][key] = std::string((const char *)data, len);
        return 0;
    }

    int get(const char *collection, const char *key, char **data, size_t *len) override
    {
        std::lock_guard<std::mutex> hold(mu_);
        *data = NULL;
        *len = 0;
        auto c = records_.find(collection);
        if (c == records_.end())
            return 1;
        auto r = c->second.find(key);
        if (r == c->second.end())
            return 1;
        *data = copy_out(r->second.data(), r->second.size());
        *len = r->second.size();
        return *data ? 0 : -1;
    }

    int remove(const char *collection, const char *key) override
    {
        std::lock_guard<std::mutex> hold(mu_);
        auto c = records_.find(collection);
        if (c != records_.end())
            c->second.erase(key);
        return 0;
    }

    int each(const char *collection, puddle_record_fn fn, void *user) override
    {
        std::map<std::string, std::string> copy; /* fn may put or remove while it runs */
        {
            std::lock_guard<std::mutex> hold(mu_);
            auto c = records_.find(collection);
            if (c != records_.end())
                copy = c->second;
        }
        for (const auto &[key, data] : copy)
            if (fn(user, key.c_str(), data.c_str(), data.size()))
                break;
        return 0;
    }

private:
    std::mutex mu_;
    std::map<std::string, std::map<std::string, std::string>> records_;
};

} // namespace

puddle_storage *puddle_storage_open_files(const char *data_dir, char *err, size_t errlen)
{
    std::string state = std::string(data_dir) + "/state";
    if (puddle_mkdir_p(state.c_str()) != 0) {
        if (err)
            snprintf(err, errlen, "cannot create %s: %s", state.c_str(), strerror(errno));
        return NULL;
    }
    return new (std::nothrow) Files(data_dir);
}

puddle_storage *puddle_storage_open_memory(void)
{
    return new (std::nothrow) Memory();
}

void puddle_storage_close(puddle_storage *s)
{
    delete s;
}

int puddle_record_put(puddle_storage *s, const char *collection, const char *key, const void *data, size_t len)
{
    if (!name_ok(collection) || !name_ok(key)) {
        errno = EINVAL;
        return -1;
    }
    return s->put(collection, key, data, len, false);
}

int puddle_record_get(puddle_storage *s, const char *collection, const char *key, char **data, size_t *len)
{
    if (!name_ok(collection) || !name_ok(key)) {
        errno = EINVAL;
        return -1;
    }
    return s->get(collection, key, data, len);
}

int puddle_record_remove(puddle_storage *s, const char *collection, const char *key)
{
    if (!name_ok(collection) || !name_ok(key)) {
        errno = EINVAL;
        return -1;
    }
    return s->remove(collection, key);
}

int puddle_record_each(puddle_storage *s, const char *collection, puddle_record_fn fn, void *user)
{
    if (!name_ok(collection)) {
        errno = EINVAL;
        return -1;
    }
    return s->each(collection, fn, user);
}

int puddle_secret_put(puddle_storage *s, const char *name, const void *data, size_t len)
{
    if (!name_ok(name)) {
        errno = EINVAL;
        return -1;
    }
    return s->put(kSecrets, name, data, len, true);
}

int puddle_secret_get(puddle_storage *s, const char *name, char **data, size_t *len)
{
    if (!name_ok(name)) {
        errno = EINVAL;
        return -1;
    }
    return s->get(kSecrets, name, data, len);
}
