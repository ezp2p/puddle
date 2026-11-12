#include "core/transfers.h"

#include "core/identity.h"

#include "platform/clock.h"
#include "storage/storage.h"
#include "base/json.h"
#include "base/log.h"

#include <condition_variable>
#include <mutex>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <thread>
#include <vector>

#define COLLECTION "transfers"
#define GIVE_UP_MS (7LL * 24 * 3600 * 1000) /* nobody had it for a week */
#define DAY_MS (24LL * 3600 * 1000)
#define SWEEP_EVERY_MS (3600LL * 1000)

namespace {

struct Transfer {
    std::string id, chat, message, reason;
    int64_t size = 0, created_ms = 0, next_ms = 0, first_fail_ms = 0;
    int attempts = 0;
    std::vector<std::string> sources;
    puddle_tr_state state = PUDDLE_TR_QUEUED;
    bool cancelled = false; /* asked to stop while it was being fetched */
};

} // namespace

struct puddle_transfers {
    puddle_files *files;
    puddle_storage *storage;
    puddle_transfer_hooks hooks;
    std::mutex mu;
    std::condition_variable wake;
    std::vector<Transfer> items;
    std::thread worker;
    bool stop = false;
    int64_t swept_ms = 0;
};

static int64_t now_of(const puddle_transfers *t) { return t->hooks.now_ms ? t->hooks.now_ms() : puddle_wall_ms(); }

int64_t puddle_transfer_retry_delay_ms(int attempts)
{
    int64_t d = 15000; /* 15 s, doubling, at most an hour */
    for (int i = 1; i < attempts && d < 3600000; i++)
        d *= 2;
    return d < 3600000 ? d : 3600000;
}

/* ---------------------------------------------------------------- keeping them */

static void save_locked(puddle_transfers *t, const Transfer &x)
{
    char buf[4096];
    puddle_jw w;
    puddle_jw_init(&w, buf, sizeof buf);
    puddle_jw_object(&w);
    puddle_jw_num(&w, "size", x.size);
    puddle_jw_str(&w, "chat", x.chat.c_str());
    puddle_jw_str(&w, "message", x.message.c_str());
    puddle_jw_key(&w, "sources");
    puddle_jw_array(&w);
    for (const auto &s : x.sources)
        puddle_jw_string(&w, s.c_str());
    puddle_jw_end_array(&w);
    puddle_jw_str(&w, "state", x.state == PUDDLE_TR_FAILED ? "failed" : "queued");
    puddle_jw_num(&w, "attempts", x.attempts);
    puddle_jw_num(&w, "created_ms", x.created_ms);
    puddle_jw_num(&w, "next_ms", x.next_ms);
    puddle_jw_num(&w, "first_fail_ms", x.first_fail_ms);
    puddle_jw_str(&w, "reason", x.reason.c_str());
    puddle_jw_end_object(&w);
    size_t n = puddle_jw_finish(&w);
    if (!n || puddle_record_put(t->storage, COLLECTION, x.id.c_str(), buf, n) != 0)
        puddle_log(PUDDLE_LOG_WARN, "cannot keep the download of %.12s…; it starts again after a restart", x.id.c_str());
}

static int load_one(void *user, const char *key, const char *data, size_t len)
{
    puddle_transfers *t = (puddle_transfers *)user;
    if (!puddle_file_id_valid(key))
        return 0;
    Transfer x;
    x.id = key;
    puddle_span v;
    char s[300];
    if (puddle_json_member(data, len, "size", &v) != 0 || puddle_json_int64(v, &x.size) != 0)
        return 0;
    if (puddle_json_member(data, len, "chat", &v) == 0 && puddle_json_string(v, s, sizeof s) == 0)
        x.chat = s;
    if (puddle_json_member(data, len, "message", &v) == 0 && puddle_json_string(v, s, sizeof s) == 0)
        x.message = s;
    if (puddle_json_member(data, len, "reason", &v) == 0 && puddle_json_string(v, s, sizeof s) == 0)
        x.reason = s;
    int64_t n;
    if (puddle_json_member(data, len, "attempts", &v) == 0 && puddle_json_int64(v, &n) == 0)
        x.attempts = (int)n;
    if (puddle_json_member(data, len, "created_ms", &v) == 0)
        puddle_json_int64(v, &x.created_ms);
    if (puddle_json_member(data, len, "next_ms", &v) == 0)
        puddle_json_int64(v, &x.next_ms);
    if (puddle_json_member(data, len, "first_fail_ms", &v) == 0)
        puddle_json_int64(v, &x.first_fail_ms);
    if (puddle_json_member(data, len, "state", &v) == 0 && puddle_json_string(v, s, sizeof s) == 0 && !strcmp(s, "failed"))
        x.state = PUDDLE_TR_FAILED;
    if (puddle_json_member(data, len, "sources", &v) == 0) {
        size_t cur = 0;
        puddle_span item;
        while (puddle_json_array_next(v, &cur, &item) == 1 && x.sources.size() < PUDDLE_TRANSFER_SOURCES)
            if (puddle_json_string(item, s, sizeof s) == 0 && strlen(s) == PUDDLE_USER_ID_LEN)
                x.sources.push_back(s);
    }
    t->items.push_back(x);
    return 0;
}

static Transfer *find_locked(puddle_transfers *t, const std::string &id)
{
    for (auto &x : t->items)
        if (x.id == id)
            return &x;
    return nullptr;
}

/* ---------------------------------------------------------------- fetching one */

typedef enum { RUN_DONE, RUN_RETRY, RUN_STOPPED } run_result;

/* GET `path` from `peer`; the body (malloc'd) if it answered 200, else NULL and why in `why` */
static char *ask(puddle_transfers *t, const std::string &peer, const std::string &path, size_t max, size_t *len, std::string &why)
{
    puddle_net_reply r;
    memset(&r, 0, sizeof r);
    t->hooks.get(t->hooks.user, peer.c_str(), path.c_str(), max, &r);
    char *body = nullptr;
    if (r.result == PUDDLE_NET_OK && r.status == 200 && r.body) {
        body = r.body;
        *len = r.body_len;
        r.body = nullptr;
    } else {
        why = r.result == PUDDLE_NET_OK ? (r.status == 404 ? "they do not have it" : "they answered " + std::to_string(r.status)) : r.reason;
    }
    puddle_net_reply_free(&r);
    return body;
}

static bool stopping(puddle_transfers *t, const std::string &id)
{
    std::lock_guard<std::mutex> hold(t->mu);
    Transfer *x = find_locked(t, id);
    return t->stop || !x || x->cancelled;
}

/* Fetch what is missing of `x` (a copy: the list may change meanwhile). Sets `why` when it cannot go on. */
static run_result run_one(puddle_transfers *t, const Transfer &x, std::string &why)
{
    const char *id = x.id.c_str();
    if (puddle_files_state(t->files, id, NULL, NULL) == PUDDLE_FILE_WHOLE)
        return RUN_DONE;
    std::string query = "?chat=" + x.chat + "&message=" + x.message;
    std::string base = "/stream/v1/files/" + x.id + "/chunks";
    uint32_t count = puddle_file_chunk_count(x.size);
    size_t good = 0; /* the source that gave the last chunk is asked first for the next */
    why = "nobody to ask";

    if (puddle_files_state(t->files, id, NULL, NULL) == PUDDLE_FILE_ABSENT) {
        bool begun = false;
        std::string bad;
        for (size_t i = 0; i < x.sources.size() && !begun; i++) {
            size_t len = 0;
            char *list = ask(t, x.sources[i], base + query, (size_t)count * 32, &len, why);
            if (list && len == (size_t)count * 32 && puddle_files_fetch_begin(t->files, id, x.size, (const unsigned char *)list, count) == 0) {
                begun = true;
                good = i;
            } else if (list) {
                why = bad = "what they sent is not this file";
            }
            free(list);
        }
        if (!begun) {
            if (!bad.empty())
                why = bad;
            return RUN_RETRY;
        }
    }
    for (int64_t n; (n = puddle_files_fetch_next(t->files, id)) >= 0;) {
        if (stopping(t, x.id))
            return RUN_STOPPED;
        std::string path = base + "/" + std::to_string(n) + query;
        bool got = false;
        std::string bad; /* someone sent the wrong bytes: worth saying over anyone else being away */
        for (size_t k = 0; k < x.sources.size() && !got; k++) {
            size_t i = (good + k) % x.sources.size(), len = 0;
            char *chunk = ask(t, x.sources[i], path, PUDDLE_FILE_CHUNK, &len, why);
            if (chunk) {
                int rc = puddle_files_fetch_put(t->files, id, (uint32_t)n, (const unsigned char *)chunk, len);
                got = rc == 0;
                if (rc == -1)
                    why = bad = "a piece they sent was not this file's";
                else if (rc != 0)
                    why = "cannot write it here";
                if (got)
                    good = i;
            }
            free(chunk);
        }
        if (!got) {
            if (!bad.empty())
                why = bad;
            return RUN_RETRY;
        }
    }
    if (puddle_files_fetch_finish(t->files, id) != 0) {
        why = "cannot keep it here";
        return RUN_RETRY;
    }
    return RUN_DONE;
}

/* files no message carries, and uploads nobody finished: once an hour */
static void sweep(puddle_transfers *t, int64_t now)
{
    int files = puddle_files_sweep(t->files, now, DAY_MS, NULL, NULL);
    int uploads = puddle_files_upload_expire(t->files, now, DAY_MS);
    if (files || uploads)
        puddle_log(PUDDLE_LOG_INFO, "cleared away %d file%s no message uses, and %d unfinished upload%s", files, files == 1 ? "" : "s",
                   uploads, uploads == 1 ? "" : "s");
}

static void work(puddle_transfers *t)
{
    std::unique_lock<std::mutex> lock(t->mu);
    while (!t->stop) {
        int64_t now = now_of(t), soonest = INT64_MAX;
        if (now - t->swept_ms >= SWEEP_EVERY_MS) {
            t->swept_ms = now;
            lock.unlock();
            sweep(t, now);
            lock.lock();
            continue;
        }
        Transfer *next = nullptr;
        for (auto &x : t->items) {
            if (x.state != PUDDLE_TR_QUEUED)
                continue;
            if (x.next_ms <= now && (!next || x.created_ms < next->created_ms))
                next = &x;
            else if (x.next_ms > now && x.next_ms < soonest)
                soonest = x.next_ms;
        }
        if (!next) {
            int64_t wait = soonest == INT64_MAX ? 60000 : soonest - now;
            t->wake.wait_for(lock, std::chrono::milliseconds(wait < 60000 ? wait : 60000));
            continue;
        }
        next->state = PUDDLE_TR_ACTIVE;
        Transfer copy = *next;
        lock.unlock();
        std::string why;
        run_result r = run_one(t, copy, why);
        lock.lock();
        Transfer *x = find_locked(t, copy.id);
        if (!x)
            continue;
        if (x->cancelled) {
            std::string id = x->id;
            t->items.erase(t->items.begin() + (x - t->items.data()));
            puddle_record_remove(t->storage, COLLECTION, id.c_str());
            lock.unlock();
            puddle_files_remove(t->files, id.c_str());
            lock.lock();
            continue;
        }
        if (r == RUN_DONE) {
            std::string id = x->id, chat = x->chat, message = x->message;
            puddle_record_remove(t->storage, COLLECTION, x->id.c_str());
            t->items.erase(t->items.begin() + (x - t->items.data()));
            if (t->hooks.finished) {
                lock.unlock();
                t->hooks.finished(t->hooks.finished_user, id.c_str(), chat.c_str(), message.c_str());
                lock.lock();
            }
            continue;
        }
        x->state = PUDDLE_TR_QUEUED;
        if (r == RUN_STOPPED)
            continue; /* closing: it carries on at the next open */
        now = now_of(t);
        x->attempts++;
        if (!x->first_fail_ms)
            x->first_fail_ms = now;
        x->reason = why;
        if (now - x->first_fail_ms > GIVE_UP_MS) {
            x->state = PUDDLE_TR_FAILED;
            puddle_log(PUDDLE_LOG_INFO, "gave up fetching file %.12s…: %s", x->id.c_str(), why.c_str());
        } else {
            x->next_ms = now + puddle_transfer_retry_delay_ms(x->attempts);
        }
        save_locked(t, *x);
    }
}

/* ---------------------------------------------------------------- the queue */

puddle_transfers *puddle_transfers_open(const char *data_dir, puddle_files *files, const puddle_transfer_hooks *hooks, char *err, size_t errlen)
{
    puddle_transfers *t = new (std::nothrow) puddle_transfers();
    if (!t) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    t->files = files;
    if (hooks)
        t->hooks = *hooks;
    t->storage = puddle_storage_open_files(data_dir, err, errlen);
    if (!t->storage) {
        delete t;
        return NULL;
    }
    puddle_record_each(t->storage, COLLECTION, load_one, t);
    if (t->hooks.get)
        t->worker = std::thread(work, t);
    return t;
}

void puddle_transfers_close(puddle_transfers *t)
{
    if (!t)
        return;
    {
        std::lock_guard<std::mutex> hold(t->mu);
        t->stop = true;
    }
    t->wake.notify_all();
    if (t->worker.joinable())
        t->worker.join();
    puddle_storage_close(t->storage);
    delete t;
}

int puddle_transfers_start(puddle_transfers *t, const char *id, int64_t size, const char *chat, const char *message,
                           const char *const *sources, size_t nsources)
{
    if (!puddle_file_id_valid(id) || size <= 0 || !chat || !message)
        return -1;
    if (puddle_files_state(t->files, id, NULL, NULL) == PUDDLE_FILE_WHOLE)
        return 0;
    std::lock_guard<std::mutex> hold(t->mu);
    Transfer *x = find_locked(t, id);
    if (!x) {
        t->items.push_back(Transfer());
        x = &t->items.back();
        x->id = id;
        x->size = size;
        x->chat = chat;
        x->message = message;
        x->created_ms = now_of(t);
    }
    x->cancelled = false;
    for (size_t i = 0; i < nsources; i++) {
        bool have = false;
        for (const auto &s : x->sources)
            have = have || s == sources[i];
        if (!have && x->sources.size() < PUDDLE_TRANSFER_SOURCES && strlen(sources[i]) == PUDDLE_USER_ID_LEN)
            x->sources.push_back(sources[i]);
    }
    if (x->state == PUDDLE_TR_FAILED) { /* asked again: from the start */
        x->state = PUDDLE_TR_QUEUED;
        x->attempts = 0;
        x->first_fail_ms = 0;
        x->reason.clear();
    }
    if (x->state == PUDDLE_TR_QUEUED)
        x->next_ms = 0;
    save_locked(t, *x);
    t->wake.notify_all();
    return 0;
}

int puddle_transfers_cancel(puddle_transfers *t, const char *id)
{
    std::unique_lock<std::mutex> lock(t->mu);
    Transfer *x = find_locked(t, id);
    if (!x)
        return 0;
    if (x->state == PUDDLE_TR_ACTIVE) { /* the worker stops it between chunks, and clears up */
        x->cancelled = true;
        return 0;
    }
    t->items.erase(t->items.begin() + (x - t->items.data()));
    puddle_record_remove(t->storage, COLLECTION, id);
    lock.unlock();
    puddle_files_remove(t->files, id);
    return 0;
}

puddle_tr_state puddle_transfers_state(puddle_transfers *t, const char *id, char *reason)
{
    if (reason)
        reason[0] = '\0';
    std::lock_guard<std::mutex> hold(t->mu);
    Transfer *x = find_locked(t, id);
    if (!x)
        return PUDDLE_TR_NONE;
    if (reason)
        snprintf(reason, 200, "%s", x->reason.c_str());
    return x->cancelled ? PUDDLE_TR_NONE : x->state;
}

void puddle_transfers_retry_now(puddle_transfers *t)
{
    {
        std::lock_guard<std::mutex> hold(t->mu);
        for (auto &x : t->items)
            if (x.state == PUDDLE_TR_QUEUED)
                x.next_ms = 0;
    }
    t->wake.notify_all();
}

void puddle_transfers_forget(puddle_transfers *t, puddle_files *files, const char *id)
{
    if (t)
        puddle_transfers_cancel(t, id);
    if (files)
        puddle_files_remove(files, id);
}
