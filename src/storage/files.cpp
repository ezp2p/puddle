#include "storage/files.h"

#include "platform/fsutil.h"
#include "base/hash.h"

#include <openssl/rand.h>

#include <errno.h>
#include <mutex>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unordered_map>
#include <vector>

struct puddle_files {
    std::string dir, partial, uploads; /* <data_dir>/files, and its two folders */
    std::mutex mu;
    std::unordered_map<std::string, int64_t> touched; /* upload token -> when a chunk last came (0: not since we started) */
};

static int hex_ok(const char *s, size_t n)
{
    if (!s || strlen(s) != n)
        return 0;
    for (size_t i = 0; i < n; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
            return 0;
    return 1;
}

int puddle_file_id_valid(const char *id) { return hex_ok(id, PUDDLE_FILE_ID_LEN); }
int puddle_upload_token_valid(const char *token) { return hex_ok(token, PUDDLE_UPLOAD_TOKEN_LEN); }

uint32_t puddle_file_chunk_count(int64_t size)
{
    return size <= 0 ? 0 : (uint32_t)((size + PUDDLE_FILE_CHUNK - 1) / PUDDLE_FILE_CHUNK);
}

size_t puddle_file_chunk_len(int64_t size, uint32_t n)
{
    int64_t start = (int64_t)n * PUDDLE_FILE_CHUNK;
    if (start >= size)
        return 0;
    return (size_t)(size - start < PUDDLE_FILE_CHUNK ? size - start : PUDDLE_FILE_CHUNK);
}

/* ---------------------------------------------------------------- small helpers */

/* the whole of a small file (at most `max` bytes); empty on failure */
static std::string slurp(const std::string &path, size_t max)
{
    std::string out;
    int fd = puddle_fd_open(path.c_str(), PUDDLE_F_READ);
    if (fd < 0)
        return out;
    int64_t size = puddle_fd_size(fd);
    if (size > 0 && (size_t)size <= max) {
        out.resize((size_t)size);
        if (puddle_fd_read(fd, &out[0], (size_t)size) != size)
            out.clear();
    }
    puddle_fd_close(fd);
    return out;
}

static int put_small(const std::string &dir, const std::string &path, const void *data, size_t len)
{
    return puddle_write_file_atomic(dir.c_str(), path.c_str(), data, len);
}

static int64_t read_size(const std::string &path)
{
    std::string s = slurp(path, 32);
    if (s.empty())
        return -1;
    char *end;
    long long v = strtoll(s.c_str(), &end, 10);
    return end != s.c_str() && v > 0 && v <= PUDDLE_FILE_HARD_MAX ? (int64_t)v : -1;
}

static int write_size(const std::string &dir, const std::string &path, int64_t size)
{
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%lld", (long long)size);
    return put_small(dir, path, buf, (size_t)n);
}

/* write `len` bytes at `offset` of the file at `path` (made if missing) */
static int write_at(const std::string &path, int64_t offset, const unsigned char *data, size_t len)
{
    int fd = puddle_fd_open(path.c_str(), PUDDLE_F_WRITE | PUDDLE_F_CREATE);
    if (fd < 0)
        return -1;
    int rc = puddle_fd_seek(fd, offset) == offset && puddle_fd_write_all(fd, data, len) == 0 ? 0 : -1;
    puddle_fd_close(fd);
    return rc;
}

/* read chunk `n` of the file at `path`, `len` bytes long */
static int read_at(const std::string &path, int64_t offset, size_t len, unsigned char **out)
{
    int fd = puddle_fd_open(path.c_str(), PUDDLE_F_READ);
    if (fd < 0)
        return 1;
    unsigned char *buf = (unsigned char *)malloc(len ? len : 1);
    int rc = buf && puddle_fd_seek(fd, offset) == offset && puddle_fd_read(fd, buf, len) == (int64_t)len ? 0 : -1;
    puddle_fd_close(fd);
    if (rc != 0) {
        free(buf);
        return -1;
    }
    *out = buf;
    return 0;
}

/* the id the chunk hashes make */
static void id_of(const unsigned char *hashes, uint32_t count, char id[PUDDLE_FILE_ID_LEN + 1])
{
    puddle_sha256_hex(hashes, (size_t)count * 32, id);
}

/* ---------------------------------------------------------------- open */

puddle_files *puddle_files_open(const char *data_dir, char *err, size_t errlen)
{
    puddle_files *f = new (std::nothrow) puddle_files();
    if (!f) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    f->dir = std::string(data_dir) + "/files";
    f->partial = f->dir + "/partial";
    f->uploads = f->dir + "/uploads";
    if (puddle_mkdir_p(f->partial.c_str()) != 0 || puddle_mkdir_p(f->uploads.c_str()) != 0) {
        snprintf(err, errlen, "cannot make %s: %s", f->dir.c_str(), strerror(errno));
        delete f;
        return NULL;
    }
    puddle_dir *d = puddle_dir_open(f->uploads.c_str());
    for (const char *name; d && (name = puddle_dir_next(d));) /* uploads from before a restart: counted from now */
        if (strlen(name) == PUDDLE_UPLOAD_TOKEN_LEN + 5 && !strcmp(name + PUDDLE_UPLOAD_TOKEN_LEN, ".size"))
            f->touched[std::string(name, PUDDLE_UPLOAD_TOKEN_LEN)] = 0;
    if (d)
        puddle_dir_close(d);
    return f;
}

void puddle_files_close(puddle_files *f)
{
    delete f;
}

/* ---------------------------------------------------------------- what is here */

puddle_file_state puddle_files_state(puddle_files *f, const char *id, int64_t *size, int64_t *have)
{
    if (size) *size = 0;
    if (have) *have = 0;
    if (!puddle_file_id_valid(id))
        return PUDDLE_FILE_ABSENT;
    std::lock_guard<std::mutex> hold(f->mu);
    int64_t whole = puddle_path_size((f->dir + "/" + id).c_str());
    if (whole >= 0) {
        if (size) *size = whole;
        if (have) *have = whole;
        return PUDDLE_FILE_WHOLE;
    }
    std::string p = f->partial + "/" + id;
    int64_t sz = read_size(p + ".size");
    if (sz < 0)
        return PUDDLE_FILE_ABSENT;
    std::string map = slurp(p + ".have", puddle_file_chunk_count(sz));
    int64_t got = 0;
    for (uint32_t i = 0; i < map.size(); i++)
        if (map[i] == 1)
            got += (int64_t)puddle_file_chunk_len(sz, i);
    if (size) *size = sz;
    if (have) *have = got;
    return PUDDLE_FILE_PARTIAL;
}

int puddle_files_path(puddle_files *f, const char *id, char *out, size_t cap)
{
    if (!puddle_file_id_valid(id))
        return 1;
    std::string p = f->dir + "/" + id;
    if (!puddle_path_is_file(p.c_str()))
        return 1;
    snprintf(out, cap, "%s", p.c_str());
    return 0;
}

int puddle_files_chunk_list(puddle_files *f, const char *id, int64_t *size, unsigned char **hashes, uint32_t *count)
{
    if (!puddle_file_id_valid(id))
        return 1;
    std::lock_guard<std::mutex> hold(f->mu);
    std::string base = f->dir + "/" + id;
    int64_t sz = puddle_path_size(base.c_str());
    if (sz < 0) {
        base = f->partial + "/" + id;
        sz = read_size(base + ".size");
    }
    if (sz <= 0)
        return 1;
    uint32_t n = puddle_file_chunk_count(sz);
    std::string list = slurp(base + ".chunks", (size_t)n * 32);
    if (list.size() != (size_t)n * 32)
        return 1;
    *hashes = (unsigned char *)malloc(list.size());
    if (!*hashes)
        return 1;
    memcpy(*hashes, list.data(), list.size());
    *size = sz;
    *count = n;
    return 0;
}

int puddle_files_read_chunk(puddle_files *f, const char *id, uint32_t n, unsigned char **data, size_t *len)
{
    if (!puddle_file_id_valid(id))
        return 1;
    std::lock_guard<std::mutex> hold(f->mu);
    std::string p = f->dir + "/" + id;
    int64_t sz = puddle_path_size(p.c_str());
    if (sz < 0) { /* a download that may have this chunk already */
        p = f->partial + "/" + id;
        sz = read_size(p + ".size");
        std::string map = slurp(p + ".have", puddle_file_chunk_count(sz > 0 ? sz : 0));
        if (sz <= 0 || n >= map.size() || map[n] != 1)
            return 1;
    }
    if (n >= puddle_file_chunk_count(sz))
        return 1;
    *len = puddle_file_chunk_len(sz, n);
    return read_at(p, (int64_t)n * PUDDLE_FILE_CHUNK, *len, data);
}

int puddle_files_remove(puddle_files *f, const char *id)
{
    if (!puddle_file_id_valid(id))
        return 0;
    std::lock_guard<std::mutex> hold(f->mu);
    std::string w = f->dir + "/" + id, p = f->partial + "/" + id;
    int rc = 0;
    for (const std::string &x : { w, w + ".chunks", p, p + ".chunks", p + ".have", p + ".size" })
        if (puddle_remove_file(x.c_str()) != 0)
            rc = -1;
    return rc;
}

void puddle_files_each(puddle_files *f, puddle_file_fn fn, void *user)
{
    std::vector<std::pair<std::string, puddle_file_state>> all;
    {
        std::lock_guard<std::mutex> hold(f->mu);
        for (int pass = 0; pass < 2; pass++) {
            puddle_dir *d = puddle_dir_open(pass ? f->partial.c_str() : f->dir.c_str());
            for (const char *name; d && (name = puddle_dir_next(d));)
                if (puddle_file_id_valid(name) && (pass || puddle_path_is_file((f->dir + "/" + name).c_str())))
                    all.emplace_back(name, pass ? PUDDLE_FILE_PARTIAL : PUDDLE_FILE_WHOLE);
            if (d)
                puddle_dir_close(d);
        }
    }
    for (const auto &e : all)
        if (fn(user, e.first.c_str(), e.second))
            break;
}

/* ---------------------------------------------------------------- fetching */

int puddle_files_fetch_begin(puddle_files *f, const char *id, int64_t size, const unsigned char *hashes, uint32_t count)
{
    if (!puddle_file_id_valid(id) || size <= 0 || size > PUDDLE_FILE_HARD_MAX || count != puddle_file_chunk_count(size))
        return -1;
    char made[PUDDLE_FILE_ID_LEN + 1];
    id_of(hashes, count, made);
    if (strcmp(made, id) != 0)
        return -1;
    std::lock_guard<std::mutex> hold(f->mu);
    std::string p = f->partial + "/" + id;
    if (puddle_path_is_file((f->dir + "/" + id).c_str()) || read_size(p + ".size") > 0)
        return 0; /* here, or on its way */
    std::string none((size_t)count, '\0');
    if (put_small(f->partial, p + ".chunks", hashes, (size_t)count * 32) != 0 || put_small(f->partial, p + ".have", none.data(), none.size()) != 0)
        return -1;
    int fd = puddle_fd_open(p.c_str(), PUDDLE_F_WRITE | PUDDLE_F_CREATE | PUDDLE_F_TRUNC);
    if (fd < 0)
        return -1;
    puddle_fd_close(fd);
    return write_size(f->partial, p + ".size", size); /* last: until it is there, there is no download */
}

int puddle_files_fetch_put(puddle_files *f, const char *id, uint32_t n, const unsigned char *data, size_t len)
{
    if (!puddle_file_id_valid(id))
        return 1;
    std::lock_guard<std::mutex> hold(f->mu);
    std::string p = f->partial + "/" + id;
    int64_t size = read_size(p + ".size");
    if (size <= 0)
        return 1;
    uint32_t count = puddle_file_chunk_count(size);
    if (n >= count || len != puddle_file_chunk_len(size, n))
        return -1;
    std::string list = slurp(p + ".chunks", (size_t)count * 32);
    std::string map = slurp(p + ".have", count);
    if (list.size() != (size_t)count * 32 || map.size() != count)
        return -2;
    unsigned char h[32];
    puddle_sha256(data, len, h);
    if (memcmp(h, list.data() + (size_t)n * 32, 32) != 0)
        return -1;
    if (map[n] == 1)
        return 0;
    if (write_at(p, (int64_t)n * PUDDLE_FILE_CHUNK, data, len) != 0)
        return -2;
    map[n] = 1;
    return put_small(f->partial, p + ".have", map.data(), map.size()) == 0 ? 0 : -2;
}

int64_t puddle_files_fetch_next(puddle_files *f, const char *id)
{
    if (!puddle_file_id_valid(id))
        return -1;
    std::lock_guard<std::mutex> hold(f->mu);
    std::string p = f->partial + "/" + id;
    int64_t size = read_size(p + ".size");
    if (size <= 0)
        return -1;
    std::string map = slurp(p + ".have", puddle_file_chunk_count(size));
    for (size_t i = 0; i < map.size(); i++)
        if (map[i] != 1)
            return (int64_t)i;
    return map.size() == puddle_file_chunk_count(size) ? -1 : 0;
}

int puddle_files_fetch_finish(puddle_files *f, const char *id)
{
    if (!puddle_file_id_valid(id))
        return 1;
    std::lock_guard<std::mutex> hold(f->mu);
    std::string p = f->partial + "/" + id, w = f->dir + "/" + id;
    int64_t size = read_size(p + ".size");
    if (size <= 0)
        return 1;
    std::string map = slurp(p + ".have", puddle_file_chunk_count(size));
    if (map.size() != puddle_file_chunk_count(size) || map.find('\0') != std::string::npos)
        return 1;
    if (puddle_rename((p + ".chunks").c_str(), (w + ".chunks").c_str()) != 0 || puddle_rename(p.c_str(), w.c_str()) != 0)
        return -1;
    puddle_remove_file((p + ".have").c_str());
    puddle_remove_file((p + ".size").c_str());
    return 0;
}

/* ---------------------------------------------------------------- uploads */

int puddle_files_upload_create(puddle_files *f, int64_t size, char token[PUDDLE_UPLOAD_TOKEN_LEN + 1])
{
    if (size <= 0 || size > PUDDLE_FILE_HARD_MAX)
        return -1;
    unsigned char raw[PUDDLE_UPLOAD_TOKEN_LEN / 2];
    if (RAND_bytes(raw, sizeof raw) != 1)
        return -1;
    puddle_hex_encode(raw, sizeof raw, token);
    std::lock_guard<std::mutex> hold(f->mu);
    std::string p = f->uploads + "/" + token;
    std::string none((size_t)puddle_file_chunk_count(size), '\0');
    int fd = puddle_fd_open(p.c_str(), PUDDLE_F_WRITE | PUDDLE_F_CREATE | PUDDLE_F_TRUNC);
    if (fd < 0)
        return -1;
    puddle_fd_close(fd);
    if (put_small(f->uploads, p + ".have", none.data(), none.size()) != 0 || write_size(f->uploads, p + ".size", size) != 0)
        return -1;
    f->touched[token] = 0;
    return 0;
}

int puddle_files_upload_put(puddle_files *f, const char *token, uint32_t n, const unsigned char *data, size_t len)
{
    if (!puddle_upload_token_valid(token))
        return 1;
    std::lock_guard<std::mutex> hold(f->mu);
    std::string p = f->uploads + "/" + token;
    int64_t size = read_size(p + ".size");
    if (size <= 0)
        return 1;
    uint32_t count = puddle_file_chunk_count(size);
    if (n >= count || len != puddle_file_chunk_len(size, n))
        return -1;
    std::string map = slurp(p + ".have", count);
    if (map.size() != count || write_at(p, (int64_t)n * PUDDLE_FILE_CHUNK, data, len) != 0)
        return -2;
    map[n] = 1;
    f->touched[token] = 0; /* counted again from the next expiry pass */
    return put_small(f->uploads, p + ".have", map.data(), map.size()) == 0 ? 0 : -2;
}

int puddle_files_upload_info(puddle_files *f, const char *token, int64_t *size, uint32_t *have, uint32_t *count)
{
    if (!puddle_upload_token_valid(token))
        return 1;
    std::lock_guard<std::mutex> hold(f->mu);
    std::string p = f->uploads + "/" + token;
    int64_t sz = read_size(p + ".size");
    if (sz <= 0)
        return 1;
    std::string map = slurp(p + ".have", puddle_file_chunk_count(sz));
    uint32_t got = 0;
    for (char c : map)
        got += c == 1;
    *size = sz;
    *have = got;
    *count = puddle_file_chunk_count(sz);
    return 0;
}

static void drop_upload_locked(puddle_files *f, const std::string &token)
{
    std::string p = f->uploads + "/" + token;
    for (const std::string &x : { p, p + ".have", p + ".size", p + ".chunks" })
        puddle_remove_file(x.c_str());
    f->touched.erase(token);
}

int puddle_files_upload_finish(puddle_files *f, const char *token, char id[PUDDLE_FILE_ID_LEN + 1])
{
    if (!puddle_upload_token_valid(token))
        return 1;
    std::lock_guard<std::mutex> hold(f->mu);
    std::string p = f->uploads + "/" + token;
    int64_t size = read_size(p + ".size");
    if (size <= 0)
        return 1;
    uint32_t count = puddle_file_chunk_count(size);
    std::string map = slurp(p + ".have", count);
    if (map.size() != count || map.find('\0') != std::string::npos)
        return 2;
    /* hash each chunk as it is: the file's id comes from what is on disk, not from anything the owner said */
    std::vector<unsigned char> hashes((size_t)count * 32);
    int fd = puddle_fd_open(p.c_str(), PUDDLE_F_READ);
    unsigned char *buf = (unsigned char *)malloc(PUDDLE_FILE_CHUNK);
    int bad = fd < 0 || !buf;
    for (uint32_t i = 0; !bad && i < count; i++) {
        size_t len = puddle_file_chunk_len(size, i);
        bad = puddle_fd_read(fd, buf, len) != (int64_t)len;
        if (!bad)
            puddle_sha256(buf, len, &hashes[(size_t)i * 32]);
    }
    free(buf);
    if (fd >= 0)
        puddle_fd_close(fd);
    if (bad)
        return -1;
    id_of(hashes.data(), count, id);
    std::string w = f->dir + "/" + id;
    if (!puddle_path_is_file(w.c_str())) { /* the same bytes may be here already, sent or received before */
        if (put_small(f->dir, w + ".chunks", hashes.data(), hashes.size()) != 0 || puddle_rename(p.c_str(), w.c_str()) != 0)
            return -1;
    }
    drop_upload_locked(f, token);
    return 0;
}

int puddle_files_upload_cancel(puddle_files *f, const char *token)
{
    if (!puddle_upload_token_valid(token))
        return 0;
    std::lock_guard<std::mutex> hold(f->mu);
    drop_upload_locked(f, token);
    return 0;
}

int puddle_files_upload_expire(puddle_files *f, int64_t now_ms, int64_t max_age_ms)
{
    std::lock_guard<std::mutex> hold(f->mu);
    std::vector<std::string> old;
    for (auto &t : f->touched) {
        if (t.second == 0)
            t.second = now_ms; /* first seen by this pass: its time starts now */
        else if (now_ms - t.second > max_age_ms)
            old.push_back(t.first);
    }
    for (const auto &t : old)
        drop_upload_locked(f, t);
    return (int)old.size();
}
