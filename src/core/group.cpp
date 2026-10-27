#include "core/group.h"

#include "base/text.h"
#include "base/encoding.h"
#include "storage/storage.h"
#include "base/json.h"
#include "base/log.h"

#include <errno.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <stdarg.h>
#include <mutex>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RECORD_VERSION 1
#define FILE_MAX (1u << 20)

static int member_str(const char *e, size_t n, const char *key, char *out, size_t cap)
{
    puddle_span v;
    return puddle_json_member(e, n, key, &v) == 0 && puddle_json_string(v, out, cap) == 0 ? 0 : -1;
}

/* ---- the group ---- */

const char *puddle_group_my_name(puddle_group_my m)
{
    return m == PUDDLE_GROUP_INVITED ? "invited" : m == PUDDLE_GROUP_IN ? "in" : m == PUDDLE_GROUP_LEFT ? "left" : "declined";
}

const char *puddle_gm_state_name(puddle_gm_state s)
{
    return s == PUDDLE_GM_INVITED ? "invited" : s == PUDDLE_GM_MEMBER ? "member" : "left";
}

void puddle_group_free(puddle_group *g)
{
    free(g->members);
    g->members = NULL;
    g->nmembers = 0;
}

puddle_gmember *puddle_group_member(const puddle_group *g, const char *id)
{
    for (size_t i = 0; i < g->nmembers; i++)
        if (!strcmp(g->members[i].id, id))
            return &g->members[i];
    return NULL;
}

size_t puddle_group_count(const puddle_group *g, puddle_gm_state state)
{
    size_t n = 0;
    for (size_t i = 0; i < g->nmembers; i++)
        n += g->members[i].state == state;
    return n;
}

puddle_gmember *puddle_group_add_member(puddle_group *g, const char *id)
{
    puddle_gmember *n = (puddle_gmember *)realloc(g->members, (g->nmembers + 1) * sizeof *n);
    if (!n)
        return NULL;
    g->members = n;
    puddle_gmember *m = &g->members[g->nmembers++];
    memset(m, 0, sizeof *m);
    snprintf(m->id, sizeof m->id, "%s", id);
    return m;
}

/* ---- the store ---- */

#define COLLECTION "groups"

struct puddle_groups {
    std::mutex mu;
    puddle_storage *storage;
    puddle_storage *owned; /* the files driver opened for a data directory; NULL when handed one */
};

static void seterr(char *err, size_t n, const char *fmt, ...)
{
    if (!err || !n)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}


/* the record of `id`, NUL-terminated, or NULL if there is none (or it is too large to be one) */
static char *read_record(puddle_groups *s, const char *id, size_t *len)
{
    char *text;
    if (puddle_record_get(s->storage, COLLECTION, id, &text, len) != 0)
        return NULL;
    if (*len > FILE_MAX) {
        free(text);
        return NULL;
    }
    return text;
}

static char *encode(const puddle_group *g, size_t *out_len)
{
    size_t cap = 2048 + g->nmembers * 600 + strlen(g->name) * 6 + strlen(g->create_event) * 6 + strlen(g->join_event) * 6;
    char *buf = (char *)malloc(cap);
    if (!buf)
        return NULL;
    puddle_jw w;
    puddle_jw_init(&w, buf, cap);
    puddle_jw_object(&w);
    puddle_jw_num(&w, "v", RECORD_VERSION);
    puddle_jw_str(&w, "id", g->id);
    puddle_jw_str(&w, "name", g->name);
    puddle_jw_num(&w, "name_ts", g->name_ts);
    puddle_jw_str(&w, "creator", g->creator);
    puddle_jw_num(&w, "created", g->created_ms);
    puddle_jw_num(&w, "updated", g->updated_ms);
    puddle_jw_str(&w, "my", puddle_group_my_name(g->my));
    if (g->join_event[0]) {
        puddle_jw_str(&w, "join", g->join_event);
        puddle_jw_str(&w, "join_sig", g->join_sig);
    }
    if (g->create_event[0]) {
        puddle_jw_str(&w, "create", g->create_event);
        puddle_jw_str(&w, "create_sig", g->create_sig);
    }
    if (g->invited_by[0])
        puddle_jw_str(&w, "invited_by", g->invited_by);
    puddle_jw_key(&w, "members");
    puddle_jw_array(&w);
    for (size_t i = 0; i < g->nmembers; i++) {
        puddle_jw_object(&w);
        puddle_jw_str(&w, "id", g->members[i].id);
        puddle_jw_str(&w, "state", puddle_gm_state_name(g->members[i].state));
        if (g->members[i].by[0])
            puddle_jw_str(&w, "by", g->members[i].by);
        if (g->members[i].address[0])
            puddle_jw_str(&w, "address", g->members[i].address);
        if (g->members[i].told)
            puddle_jw_flag(&w, "told", 1);
        puddle_jw_num(&w, "ts", g->members[i].ts);
        puddle_jw_end_object(&w);
    }
    puddle_jw_end_array(&w);
    puddle_jw_end_object(&w);
    size_t n = puddle_jw_finish(&w);
    if (!n) {
        free(buf);
        return NULL;
    }
    *out_len = n;
    return buf;
}

static int member_int(const char *json, size_t len, const char *key, int64_t *out)
{
    puddle_span v;
    return puddle_json_member(json, len, key, &v) == 0 && puddle_json_int64(v, out) == 0 ? 0 : -1;
}

static int decode(const char *json, size_t len, const char *expect_id, puddle_group *g)
{
    memset(g, 0, sizeof *g);
    char tmp[PUDDLE_GROUP_NAME_MAX_BYTES + 1];
    int64_t v;
    if (!puddle_json_valid_object(json, len) || member_int(json, len, "v", &v) != 0 || v != RECORD_VERSION)
        return -1;
    if (member_str(json, len, "id", g->id, sizeof g->id) != 0 || strcmp(g->id, expect_id) != 0)
        return -1;
    if (member_str(json, len, "name", g->name, sizeof g->name) != 0 || member_str(json, len, "creator", g->creator, sizeof g->creator) != 0 ||
        !puddle_id_valid(g->creator) || member_int(json, len, "name_ts", &g->name_ts) != 0 || member_int(json, len, "created", &g->created_ms) != 0 ||
        member_int(json, len, "updated", &g->updated_ms) != 0)
        return -1;
    if (member_str(json, len, "my", tmp, sizeof tmp) != 0)
        return -1;
    if (!strcmp(tmp, "invited")) g->my = PUDDLE_GROUP_INVITED;
    else if (!strcmp(tmp, "in")) g->my = PUDDLE_GROUP_IN;
    else if (!strcmp(tmp, "left")) g->my = PUDDLE_GROUP_LEFT;
    else if (!strcmp(tmp, "declined")) g->my = PUDDLE_GROUP_DECLINED;
    else return -1;
    puddle_span jv;
    int jrc = puddle_json_member(json, len, "join", &jv);
    if (jrc < 0 || (jrc == 0 && (puddle_json_string(jv, g->join_event, sizeof g->join_event) != 0 ||
                                 member_str(json, len, "join_sig", g->join_sig, sizeof g->join_sig) != 0)))
        return -1;
    puddle_span cv;
    int crc = puddle_json_member(json, len, "create", &cv);
    if (crc < 0 || (crc == 0 && (puddle_json_string(cv, g->create_event, sizeof g->create_event) != 0 ||
                                 member_str(json, len, "create_sig", g->create_sig, sizeof g->create_sig) != 0)))
        return -1;
    puddle_span iv;
    int irc = puddle_json_member(json, len, "invited_by", &iv);
    if (irc < 0 || (irc == 0 && (puddle_json_string(iv, g->invited_by, sizeof g->invited_by) != 0 || !puddle_id_valid(g->invited_by))))
        return -1;
    puddle_span arr, item;
    if (puddle_json_member(json, len, "members", &arr) != 0)
        return -1;
    size_t cursor = 0;
    int r;
    while ((r = puddle_json_array_next(arr, &cursor, &item)) == 1) {
        char st[16];
        puddle_gmember *m = NULL;
        char id[PUDDLE_USER_ID_LEN + 1];
        if (member_str(item.p, item.len, "id", id, sizeof id) != 0 || !puddle_id_valid(id) || puddle_group_member(g, id) ||
            member_str(item.p, item.len, "state", st, sizeof st) != 0 || g->nmembers >= PUDDLE_GROUP_MAX_MEMBERS * 4 || !(m = puddle_group_add_member(g, id))) {
            puddle_group_free(g);
            return -1;
        }
        m->state = !strcmp(st, "member") ? PUDDLE_GM_MEMBER : !strcmp(st, "invited") ? PUDDLE_GM_INVITED : PUDDLE_GM_LEFT;
        if (strcmp(st, "member") && strcmp(st, "invited") && strcmp(st, "left")) {
            puddle_group_free(g);
            return -1;
        }
        puddle_span bv, av, tv;
        int trc = puddle_json_member(item.p, item.len, "told", &tv);
        if (trc < 0) {
            puddle_group_free(g);
            return -1;
        }
        m->told = trc == 0 && tv.len == 4 && !memcmp(tv.p, "true", 4);
        int arc = puddle_json_member(item.p, item.len, "address", &av);
        if (arc < 0 || (arc == 0 && puddle_json_string(av, m->address, sizeof m->address) != 0)) {
            puddle_group_free(g);
            return -1;
        }
        int brc = puddle_json_member(item.p, item.len, "by", &bv);
        if (brc < 0 || (brc == 0 && (puddle_json_string(bv, m->by, sizeof m->by) != 0 || !puddle_id_valid(m->by))) ||
            member_int(item.p, item.len, "ts", &m->ts) != 0) {
            puddle_group_free(g);
            return -1;
        }
    }
    if (r < 0) {
        puddle_group_free(g);
        return -1;
    }
    return 0;
}

puddle_groups *puddle_groups_open(const char *data_dir, char *err, size_t errlen)
{
    puddle_storage *st = puddle_storage_open_files(data_dir, err, errlen);
    if (!st)
        return NULL;
    puddle_groups *s = puddle_groups_open_with(st, err, errlen);
    if (!s) {
        puddle_storage_close(st);
        return NULL;
    }
    s->owned = st;
    return s;
}

puddle_groups *puddle_groups_open_with(puddle_storage *st, char *err, size_t errlen)
{
    puddle_groups *s = new (std::nothrow) puddle_groups();
    if (!s) {
        seterr(err, errlen, "out of memory");
        return NULL;
    }
    s->storage = st;
    return s;
}

void puddle_groups_close(puddle_groups *s)
{
    if (!s)
        return;
    puddle_storage_close(s->owned);
    delete s;
}

int puddle_groups_get(puddle_groups *s, const char *id_in, puddle_group *out)
{
    if (!puddle_id_valid(id_in))
        return -1;
    char id[PUDDLE_USER_ID_LEN + 1];
    memcpy(id, id_in, sizeof id);
    size_t len;
    s->mu.lock();
    char *text = read_record(s, id, &len);
    s->mu.unlock();
    if (!text)
        return 1;
    int rc = decode(text, len, id, out);
    free(text);
    return rc == 0 ? 0 : 1;
}

static int put_locked(puddle_groups *s, const puddle_group *g, char *err, size_t errlen)
{
    size_t n;
    char *text = encode(g, &n);
    if (!text) {
        seterr(err, errlen, "cannot encode the group record");
        return -1;
    }
    int rc = puddle_record_put(s->storage, COLLECTION, g->id, text, n);
    free(text);
    if (rc != 0) {
        seterr(err, errlen, "cannot keep group %s: %s", g->id, strerror(errno));
        return -1;
    }
    return 0;
}

int puddle_groups_put(puddle_groups *s, const puddle_group *g, char *err, size_t errlen)
{
    if (!puddle_id_valid(g->id)) {
        seterr(err, errlen, "not a valid group id");
        return -1;
    }
    s->mu.lock();
    int rc = put_locked(s, g, err, errlen);
    s->mu.unlock();
    return rc;
}

int puddle_groups_update(puddle_groups *s, const char *id_in, puddle_group_update_fn fn, void *user, char *err, size_t errlen)
{
    if (!puddle_id_valid(id_in)) {
        seterr(err, errlen, "not a valid group id");
        return -1;
    }
    char id[PUDDLE_USER_ID_LEN + 1];
    memcpy(id, id_in, sizeof id);
    size_t len = 0;
    s->mu.lock();
    int rc = -1;
    puddle_group g;
    char *text = read_record(s, id, &len);
    int exists = text && decode(text, len, id, &g) == 0;
    free(text);
    if (!exists) {
        memset(&g, 0, sizeof g);
        snprintf(g.id, sizeof g.id, "%s", id);
    }
    int r = fn(user, &g, exists);
    if (r > 0)
        rc = r;
    else if (r == 0)
        rc = put_locked(s, &g, err, errlen) == 0 ? 0 : -1;
    else
        seterr(err, errlen, "not changed");
    puddle_group_free(&g);
    s->mu.unlock();
    return rc;
}

typedef struct {
    puddle_group *list;
    size_t n, cap;
} group_list;

static int collect_group(void *user, const char *id, const char *text, size_t len)
{
    group_list *l = (group_list *)user;
    if (!puddle_id_valid(id) || len > FILE_MAX)
        return 0;
    puddle_group g;
    if (decode(text, len, id, &g) != 0) {
        puddle_log(PUDDLE_LOG_WARN, "skipping damaged group record %s", id);
        return 0;
    }
    if (l->n == l->cap) {
        size_t nc = l->cap ? l->cap * 2 : 8;
        puddle_group *more = (puddle_group *)realloc(l->list, nc * sizeof *more);
        if (!more) {
            puddle_group_free(&g);
            return 1;
        }
        l->list = more;
        l->cap = nc;
    }
    l->list[l->n++] = g;
    return 0;
}

int puddle_groups_list(puddle_groups *s, puddle_group **out, size_t *count)
{
    group_list l = { NULL, 0, 0 };
    s->mu.lock();
    int rc = puddle_record_each(s->storage, COLLECTION, collect_group, &l);
    s->mu.unlock();
    *out = l.list;
    *count = l.n;
    return rc;
}

void puddle_group_list_free(puddle_group *list, size_t count)
{
    for (size_t i = 0; i < count; i++)
        puddle_group_free(&list[i]);
    free(list);
}

size_t puddle_groups_pending(puddle_groups *s)
{
    puddle_group *l;
    size_t n, c = 0;
    if (puddle_groups_list(s, &l, &n) != 0)
        return 0;
    for (size_t i = 0; i < n; i++)
        c += l[i].my == PUDDLE_GROUP_INVITED;
    puddle_group_list_free(l, n);
    return c;
}
