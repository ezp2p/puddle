#include "core/conversations.h"

#include "base/json.h"
#include "base/log.h"
#include "core/link.h"

#include <stdarg.h>
#include <map>
#include <mutex>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#define RECORD_VERSION 1
#define COLLECTION "conversations"

struct puddle_conversations {
    puddle_storage *storage;
    puddle_storage *owned; /* the files driver opened for a data directory; NULL when handed one */
    std::mutex mu;
    std::map<std::string, uint64_t> appends; /* chat -> appends since open */
    std::map<std::string, uint64_t> built;   /* conversation id -> the chat's count its record was built at */
};

static void seterr(char *err, size_t errlen, const char *fmt, ...)
{
    if (!err || !errlen)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

static char *encode(const puddle_conv_record *r, size_t *out_len)
{
    size_t cap = PUDDLE_CONV_PREVIEW_MAX * 6 + 1024;
    char *buf = (char *)malloc(cap);
    if (!buf)
        return NULL;
    puddle_jw w;
    puddle_jw_init(&w, buf, cap);
    puddle_jw_object(&w);
    puddle_jw_num(&w, "v", RECORD_VERSION);
    puddle_jw_str(&w, "id", r->id);
    puddle_jw_str(&w, "chat", r->chat);
    puddle_jw_str(&w, "kind", r->kind == PUDDLE_CONV_GROUP ? "group" : "dm");
    puddle_jw_num(&w, "sort_ts", r->sort_ts);
    puddle_jw_num(&w, "read_at", r->read_at);
    puddle_jw_num(&w, "unread", r->unread);
    if (r->has_last) {
        puddle_jw_key(&w, "last");
        puddle_jw_object(&w);
        puddle_jw_str(&w, "id", r->last_id);
        puddle_jw_str(&w, "sender", r->last_sender);
        puddle_jw_num(&w, "ts", r->last_ts);
        puddle_jw_str(&w, "preview", r->last_preview);
        if (r->last_deleted)
            puddle_jw_flag(&w, "deleted", 1);
        puddle_jw_end_object(&w);
    }
    puddle_jw_end_object(&w);
    size_t n = puddle_jw_finish(&w);
    if (!n) {
        free(buf);
        return NULL;
    }
    *out_len = n;
    return buf;
}

static int member_str(const char *j, size_t n, const char *key, char *out, size_t cap)
{
    puddle_span v;
    return puddle_json_member(j, n, key, &v) == 0 && puddle_json_string(v, out, cap) == 0 ? 0 : -1;
}

static int member_int(const char *j, size_t n, const char *key, int64_t *out)
{
    puddle_span v;
    return puddle_json_member(j, n, key, &v) == 0 && puddle_json_int64(v, out) == 0 ? 0 : -1;
}

static int decode(const char *j, size_t n, const char *id, puddle_conv_record *r)
{
    memset(r, 0, sizeof *r);
    char kind[8];
    int64_t v, unread;
    if (member_int(j, n, "v", &v) != 0 || v != RECORD_VERSION || member_str(j, n, "id", r->id, sizeof r->id) != 0 ||
        strcmp(r->id, id) != 0 || member_str(j, n, "chat", r->chat, sizeof r->chat) != 0 ||
        member_str(j, n, "kind", kind, sizeof kind) != 0 || member_int(j, n, "sort_ts", &r->sort_ts) != 0 ||
        member_int(j, n, "read_at", &r->read_at) != 0 || member_int(j, n, "unread", &unread) != 0 || unread < 0 || unread > 1000000)
        return -1;
    if (!strcmp(kind, "group"))
        r->kind = PUDDLE_CONV_GROUP;
    else if (strcmp(kind, "dm") != 0)
        return -1;
    r->unread = (int)unread;
    puddle_span last;
    int rc = puddle_json_member(j, n, "last", &last);
    if (rc < 0)
        return -1;
    if (rc == 0) {
        if (member_str(last.p, last.len, "id", r->last_id, sizeof r->last_id) != 0 ||
            member_str(last.p, last.len, "sender", r->last_sender, sizeof r->last_sender) != 0 ||
            member_int(last.p, last.len, "ts", &r->last_ts) != 0 ||
            member_str(last.p, last.len, "preview", r->last_preview, sizeof r->last_preview) != 0)
            return -1;
        puddle_span d;
        r->last_deleted = puddle_json_member(last.p, last.len, "deleted", &d) == 0 && d.len == 4 && !memcmp(d.p, "true", 4);
        r->has_last = 1;
    }
    return 0;
}

/* caller holds the lock: 0, 1 none, -1 */
static int read_locked(puddle_conversations *c, const char *id, puddle_conv_record *out)
{
    char *text;
    size_t len;
    int rc = puddle_record_get(c->storage, COLLECTION, id, &text, &len);
    if (rc != 0)
        return rc;
    rc = decode(text, len, id, out);
    free(text);
    if (rc != 0) {
        puddle_log(PUDDLE_LOG_WARN, "damaged conversation record %s", id);
        return -1;
    }
    return 0;
}

/* caller holds the lock */
static int write_locked(puddle_conversations *c, const puddle_conv_record *r)
{
    size_t n;
    char *text = encode(r, &n);
    if (!text)
        return -1;
    int rc = puddle_record_put(c->storage, COLLECTION, r->id, text, n);
    free(text);
    return rc == 0 ? 0 : -1;
}

puddle_conversations *puddle_conversations_open(const char *data_dir, char *err, size_t errlen)
{
    puddle_storage *st = puddle_storage_open_files(data_dir, err, errlen);
    if (!st)
        return NULL;
    puddle_conversations *c = puddle_conversations_open_with(st, err, errlen);
    if (!c) {
        puddle_storage_close(st);
        return NULL;
    }
    c->owned = st;
    return c;
}

puddle_conversations *puddle_conversations_open_with(puddle_storage *storage, char *err, size_t errlen)
{
    puddle_conversations *c = new (std::nothrow) puddle_conversations();
    if (!c) {
        seterr(err, errlen, "out of memory");
        return NULL;
    }
    c->storage = storage;
    return c;
}

void puddle_conversations_close(puddle_conversations *c)
{
    if (!c)
        return;
    puddle_storage_close(c->owned);
    delete c;
}

void puddle_conversations_appended(void *user, const char *chat)
{
    puddle_conversations *c = (puddle_conversations *)user;
    std::lock_guard<std::mutex> hold(c->mu);
    c->appends[chat]++;
}

uint64_t puddle_conversations_generation(puddle_conversations *c, const char *chat)
{
    std::lock_guard<std::mutex> hold(c->mu);
    auto it = c->appends.find(chat);
    return it == c->appends.end() ? 0 : it->second;
}

int puddle_conversations_get(puddle_conversations *c, const char *id, puddle_conv_record *out, int *fresh)
{
    *fresh = 0;
    if (!puddle_id_valid(id)) {
        memset(out, 0, sizeof *out);
        return -1;
    }
    std::lock_guard<std::mutex> hold(c->mu);
    int rc = read_locked(c, id, out);
    if (rc == 0) {
        auto b = c->built.find(id);
        auto a = c->appends.find(out->chat);
        *fresh = b != c->built.end() && b->second == (a == c->appends.end() ? 0 : a->second);
    }
    return rc;
}

int puddle_conversations_put(puddle_conversations *c, const puddle_conv_record *rec, uint64_t generation)
{
    if (!puddle_id_valid(rec->id))
        return -1;
    std::lock_guard<std::mutex> hold(c->mu);
    if (write_locked(c, rec) != 0)
        return -1;
    c->built[rec->id] = generation;
    return 0;
}

int puddle_conversations_set_read(puddle_conversations *c, const char *id, const char *chat, puddle_conv_kind kind, int64_t read_at)
{
    if (!puddle_id_valid(id))
        return -1;
    std::lock_guard<std::mutex> hold(c->mu);
    puddle_conv_record r;
    if (read_locked(c, id, &r) != 0) {
        memset(&r, 0, sizeof r);
        snprintf(r.id, sizeof r.id, "%s", id);
        snprintf(r.chat, sizeof r.chat, "%s", chat);
        r.kind = kind;
    }
    if (read_at <= r.read_at)
        return 0; /* a read marker never goes backwards */
    r.read_at = read_at;
    if (write_locked(c, &r) != 0)
        return -1;
    c->built.erase(id); /* its unread count is worked out again */
    return 0;
}
