#include "storage/store.h"

#include "platform/clock.h"
#include "platform/fsutil.h"
#include "base/hash.h"
#include "base/json.h"
#include "base/log.h"

#include <errno.h>
#include <stdarg.h>
#include <mutex>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <openssl/evp.h>

#define MAX_LOADED_CHATS 64
#define MAX_CHAT_ID 128
#define MAX_EVENT_BYTES (1 << 20)
#define MAX_SIG 512
#define MAX_DAYFILE_BYTES ((int64_t)1 << 30)
#define TS_MIN 946684800000LL  /* 2000-01-01 */
#define TS_MAX 4102444800000LL /* 2100-01-01 */

/* ------------------------------------------------------------------ helpers */

static void seterr(char *err, size_t n, const char *fmt, ...)
{
    if (!err || !n)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

/* Chat ids become directory names, and arrive from other servers: allow nothing that could
 * escape the chats directory. */
static int valid_chat_id(const char *id)
{
    size_t n = strlen(id);
    if (n == 0 || n > MAX_CHAT_ID)
        return 0;
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_'))
            return 0;
    }
    return 1;
}

static int valid_id_hex(const char *id)
{
    if (strlen(id) != PUDDLE_ID_HEX)
        return 0;
    for (int i = 0; i < PUDDLE_ID_HEX; i++)
        if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f')))
            return 0;
    return 1;
}

static int64_t now_ms(void)
{
    return puddle_wall_ms();
}

/* "YYYY-MM-DD" (UTC) for a sender timestamp; -1 if outside the accepted range */
static int day_of_ts(int64_t ts, char day[11])
{
    if (ts < TS_MIN || ts >= TS_MAX)
        return -1;
    time_t sec = (time_t)(ts / 1000);
    struct tm tm;
    if (!gmtime_r(&sec, &tm))
        return -1;
    return strftime(day, 11, "%Y-%m-%d", &tm) == 10 ? 0 : -1;
}

/* ------------------------------------------------------------------- events */

void puddle_event_free(puddle_event *e)
{
    if (!e)
        return;
    free(e->event);
    free(e->sig);
    memset(e, 0, sizeof *e);
}

void puddle_event_list_free(puddle_event_list *l)
{
    if (!l)
        return;
    for (size_t i = 0; i < l->count; i++)
        puddle_event_free(&l->items[i]);
    free(l->items);
    l->items = NULL;
    l->count = 0;
}

void puddle_chunk_list_free(puddle_chunk_list *l)
{
    if (!l)
        return;
    free(l->items);
    l->items = NULL;
    l->count = 0;
}

static int event_init(puddle_event *e, const char *id, int64_t ts, int64_t rts, const char *event,
                      size_t event_len, const char *sig)
{
    memset(e, 0, sizeof *e);
    memcpy(e->id, id, PUDDLE_ID_HEX + 1);
    e->ts = ts;
    e->rts = rts;
    e->event = (char *)malloc(event_len + 1);
    e->sig = strdup(sig ? sig : "");
    if (!e->event || !e->sig) {
        puddle_event_free(e);
        return -1;
    }
    memcpy(e->event, event, event_len);
    e->event[event_len] = '\0';
    e->event_len = event_len;
    return 0;
}

static int event_copy(puddle_event *dst, const puddle_event *src)
{
    return event_init(dst, src->id, src->ts, src->rts, src->event, src->event_len, src->sig);
}

static int event_cmp(const puddle_event *a, const puddle_event *b)
{
    if (a->ts != b->ts)
        return a->ts < b->ts ? -1 : 1;
    return strcmp(a->id, b->id);
}

/* --------------------------------------------------------- in-memory chat */

typedef struct {
    char id[MAX_CHAT_ID + 1];
    puddle_event **ev;    /* sorted by (ts, id) */
    size_t n, cap;
    puddle_event **tab;   /* open-addressing set keyed by event id */
    size_t tabcap, tabn;
    uint64_t used;        /* LRU clock */
} chat;

static size_t id_hash(const char *id)
{
    uint64_t h = 0; /* ids are hex SHA-256, so their leading digits are already well mixed */
    for (int i = 0; i < 16; i++) {
        char c = id[i];
        h = h << 4 | (uint64_t)(c <= '9' ? c - '0' : c - 'a' + 10);
    }
    return (size_t)h;
}

static puddle_event *tab_find(const chat *c, const char *id)
{
    if (c->tabcap == 0)
        return NULL;
    for (size_t i = id_hash(id) & (c->tabcap - 1); c->tab[i]; i = (i + 1) & (c->tabcap - 1))
        if (strcmp(c->tab[i]->id, id) == 0)
            return c->tab[i];
    return NULL;
}

static void tab_put(puddle_event **tab, size_t cap, puddle_event *e)
{
    size_t i = id_hash(e->id) & (cap - 1);
    while (tab[i])
        i = (i + 1) & (cap - 1);
    tab[i] = e;
}

static int tab_insert(chat *c, puddle_event *e)
{
    if ((c->tabn + 1) * 2 > c->tabcap) {
        size_t ncap = c->tabcap ? c->tabcap * 2 : 64;
        puddle_event **nt = (puddle_event **)calloc(ncap, sizeof *nt);
        if (!nt)
            return -1;
        for (size_t i = 0; i < c->tabcap; i++)
            if (c->tab[i])
                tab_put(nt, ncap, c->tab[i]);
        free(c->tab);
        c->tab = nt;
        c->tabcap = ncap;
    }
    tab_put(c->tab, c->tabcap, e);
    c->tabn++;
    return 0;
}

/* 1 added (chat owns `e`), 0 duplicate (caller frees), -1 out of memory */
static int chat_add(chat *c, puddle_event *e)
{
    if (tab_find(c, e->id))
        return 0;
    if (c->n == c->cap) {
        size_t ncap = c->cap ? c->cap * 2 : 64;
        puddle_event **nev = (puddle_event **)realloc(c->ev, ncap * sizeof *nev);
        if (!nev)
            return -1;
        c->ev = nev;
        c->cap = ncap;
    }
    if (tab_insert(c, e) != 0)
        return -1;

    size_t lo = 0, hi = c->n; /* first element that sorts after e (new events are usually newest) */
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (event_cmp(c->ev[mid], e) > 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    memmove(&c->ev[lo + 1], &c->ev[lo], (c->n - lo) * sizeof *c->ev);
    c->ev[lo] = e;
    c->n++;
    return 1;
}

static void chat_free(chat *c)
{
    if (!c)
        return;
    for (size_t i = 0; i < c->n; i++) {
        puddle_event_free(c->ev[i]);
        free(c->ev[i]);
    }
    free(c->ev);
    free(c->tab);
    free(c);
}

static chat *chat_new(const char *id)
{
    chat *c = (chat *)calloc(1, sizeof *c);
    if (c)
        snprintf(c->id, sizeof c->id, "%s", id);
    return c;
}

/* ----------------------------------------------------------- record format */

typedef struct {
    char id[PUDDLE_ID_HEX + 1];
    int64_t ts, rts;
    const char *event;
    size_t event_len;
    char sig[MAX_SIG + 1];
    char chat[MAX_CHAT_ID + 1];
} record;

/* The two fields every event must carry: "chat" and "ts". */
static int event_meta(const char *ev, size_t len, int64_t *ts, char chat_out[MAX_CHAT_ID + 1])
{
    puddle_span v;
    if (puddle_json_member(ev, len, "ts", &v) != 0 || puddle_json_int64(v, ts) != 0)
        return -1;
    char day[11];
    if (day_of_ts(*ts, day) != 0)
        return -1;
    if (puddle_json_member(ev, len, "chat", &v) != 0 ||
        puddle_json_string(v, chat_out, MAX_CHAT_ID + 1) != 0 || !valid_chat_id(chat_out))
        return -1;
    return 0;
}

/* {"id":"<hex>","rts":N,"event":{...},"sig":"..."}; verifies that id is the hash of the event bytes */
static int parse_record(const char *line, size_t len, record *r)
{
    puddle_span v;
    if (!puddle_json_valid_object(line, len))
        return -1;
    if (puddle_json_member(line, len, "id", &v) != 0 ||
        puddle_json_string(v, r->id, sizeof r->id) != 0 || !valid_id_hex(r->id))
        return -1;
    if (puddle_json_member(line, len, "rts", &v) != 0 || puddle_json_int64(v, &r->rts) != 0)
        return -1;
    if (puddle_json_member(line, len, "event", &v) != 0 || v.len == 0 || v.p[0] != '{')
        return -1;
    r->event = v.p;
    r->event_len = v.len;

    int rc = puddle_json_member(line, len, "sig", &v);
    if (rc == 0) {
        if (puddle_json_string(v, r->sig, sizeof r->sig) != 0)
            return -1;
    } else if (rc == 1) {
        r->sig[0] = '\0';
    } else {
        return -1;
    }

    char hash[PUDDLE_ID_HEX + 1];
    puddle_sha256_hex(r->event, r->event_len, hash);
    if (strcmp(hash, r->id) != 0)
        return -1;
    return event_meta(r->event, r->event_len, &r->ts, r->chat);
}

/* ------------------------------------------------------------- the store */

struct puddle_store {
    char dir[PUDDLE_PATH_MAX];
    int lock_fd;
    std::mutex mu;
    chat *chats[MAX_LOADED_CHATS];
    size_t nchats;
    uint64_t clock;
    puddle_store_appended_fn appended;
    void *appended_user;
};

typedef struct {
    char day[11];
} dayname;

static int is_day_file(const char *nm)
{
    if (strlen(nm) != 16 || strcmp(nm + 10, ".jsonl") != 0)
        return 0;
    for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) {
            if (nm[i] != '-')
                return 0;
        } else if (nm[i] < '0' || nm[i] > '9') {
            return 0;
        }
    }
    return 1;
}

static int day_cmp(const void *a, const void *b)
{
    return strcmp(((const dayname *)a)->day, ((const dayname *)b)->day);
}

/* sorted ascending; -1 with errno set if the directory can't be opened */
static int list_days(const char *dir, dayname **out, size_t *count)
{
    puddle_dir *d = puddle_dir_open(dir);
    if (!d)
        return -1;
    dayname *arr = NULL;
    size_t n = 0, cap = 0;
    const char *entry;
    while ((entry = puddle_dir_next(d))) {
        if (!is_day_file(entry))
            continue;
        if (n == cap) {
            size_t ncap = cap ? cap * 2 : 16;
            dayname *na = (dayname *)realloc(arr, ncap * sizeof *na);
            if (!na) {
                free(arr);
                puddle_dir_close(d);
                errno = ENOMEM;
                return -1;
            }
            arr = na;
            cap = ncap;
        }
        memcpy(arr[n].day, entry, 10);
        arr[n].day[10] = '\0';
        n++;
    }
    puddle_dir_close(d);
    if (n)
        qsort(arr, n, sizeof *arr, day_cmp);
    *out = arr;
    *count = n;
    return 0;
}

static int day_path(const puddle_store *s, const char *chat_id, const char *day, char *out, size_t n)
{
    int w = snprintf(out, n, "%s/chats/%s/%s.jsonl", s->dir, chat_id, day);
    return (w < 0 || (size_t)w >= n) ? -1 : 0;
}

static int chat_dir(const puddle_store *s, const char *chat_id, char *out, size_t n)
{
    int w = snprintf(out, n, "%s/chats/%s", s->dir, chat_id);
    return (w < 0 || (size_t)w >= n) ? -1 : 0;
}

/* Read one day file into `c`. With `repair`, an unterminated last line (a write cut short by a crash)
 * is truncated away. Bad records elsewhere are skipped and logged, never deleted. */
static int read_day_file(chat *c, const char *path, const char *day, int repair)
{
    int fd = puddle_fd_open(path, PUDDLE_F_READ);
    if (fd < 0)
        return errno == ENOENT ? 0 : -1;
    int64_t fsize = puddle_fd_size(fd);
    if (fsize < 0 || fsize > MAX_DAYFILE_BYTES) {
        puddle_fd_close(fd);
        puddle_log(PUDDLE_LOG_ERROR, "store: cannot read %s (missing or too large)", path);
        return -1;
    }
    size_t size = (size_t)fsize;
    char *buf = (char *)malloc(size + 1);
    if (!buf) {
        puddle_fd_close(fd);
        return -1;
    }
    size_t got = 0;
    while (got < size) {
        int64_t r = puddle_fd_read(fd, buf + got, size - got);
        if (r <= 0)
            break;
        got += (size_t)r;
    }
    puddle_fd_close(fd);
    size = got;

    size_t pos = 0;
    int rc = 0;
    while (pos < size) {
        char *nl = (char *)memchr(buf + pos, '\n', size - pos);
        if (!nl) {
            puddle_log(PUDDLE_LOG_WARN, "store: %s ends with an incomplete record (%zu bytes)%s",
                       path, size - pos, repair ? "; truncating" : "");
            if (repair) {
                int wfd = puddle_fd_open(path, PUDDLE_F_WRITE);
                if (wfd >= 0) {
                    if (puddle_fd_truncate(wfd, (int64_t)pos) == 0)
                        puddle_fsync_fd(wfd);
                    puddle_fd_close(wfd);
                }
            }
            break;
        }
        size_t len = (size_t)(nl - (buf + pos));
        const char *line = buf + pos;
        size_t offset = pos;
        pos = (size_t)(nl - buf) + 1;
        if (len == 0)
            continue;

        record r;
        char rday[11];
        if (parse_record(line, len, &r) != 0 || day_of_ts(r.ts, rday) != 0 ||
            strcmp(rday, day) != 0 || strcmp(r.chat, c->id) != 0) {
            puddle_log(PUDDLE_LOG_WARN, "store: skipping bad record in %s at byte %zu", path, offset);
            continue;
        }
        puddle_event *e = (puddle_event *)malloc(sizeof *e);
        if (!e || event_init(e, r.id, r.ts, r.rts, r.event, r.event_len, r.sig) != 0) {
            free(e);
            rc = -1;
            break;
        }
        int added = chat_add(c, e);
        if (added <= 0) {
            puddle_event_free(e);
            free(e);
            if (added < 0) {
                rc = -1;
                break;
            }
        }
    }
    free(buf);
    return rc;
}

static void evict_lru(puddle_store *s)
{
    size_t victim = 0;
    for (size_t i = 1; i < s->nchats; i++)
        if (s->chats[i]->used < s->chats[victim]->used)
            victim = i;
    chat_free(s->chats[victim]);
    s->chats[victim] = s->chats[--s->nchats];
}

static chat *find_loaded(puddle_store *s, const char *chat_id)
{
    for (size_t i = 0; i < s->nchats; i++)
        if (strcmp(s->chats[i]->id, chat_id) == 0) {
            s->chats[i]->used = ++s->clock;
            return s->chats[i];
        }
    return NULL;
}

/* Load a chat into the cache. A chat with no directory is "not found": NULL with *missing set, unless
 * `create`, which returns an empty chat (its directory is made on first append). */
static chat *load_chat(puddle_store *s, const char *chat_id, int create, int *missing, char *err,
                       size_t errlen)
{
    *missing = 0;
    chat *c = find_loaded(s, chat_id);
    if (c)
        return c;

    char dir[PUDDLE_PATH_MAX];
    if (chat_dir(s, chat_id, dir, sizeof dir) != 0) {
        seterr(err, errlen, "path too long");
        return NULL;
    }
    c = chat_new(chat_id);
    if (!c) {
        seterr(err, errlen, "out of memory");
        return NULL;
    }

    dayname *days = NULL;
    size_t ndays = 0;
    if (list_days(dir, &days, &ndays) != 0) {
        if (errno == ENOENT && create) {
            /* new chat */
        } else if (errno == ENOENT) {
            chat_free(c);
            *missing = 1;
            return NULL;
        } else {
            seterr(err, errlen, "cannot read %s: %s", dir, strerror(errno));
            chat_free(c);
            return NULL;
        }
    }
    for (size_t i = 0; i < ndays; i++) {
        char path[PUDDLE_PATH_MAX];
        if (day_path(s, chat_id, days[i].day, path, sizeof path) != 0 ||
            read_day_file(c, path, days[i].day, 1) != 0) {
            seterr(err, errlen, "cannot load chat %s", chat_id);
            free(days);
            chat_free(c);
            return NULL;
        }
    }
    free(days);

    if (s->nchats == MAX_LOADED_CHATS)
        evict_lru(s);
    c->used = ++s->clock;
    s->chats[s->nchats++] = c;
    return c;
}

/* ------------------------------------------------------------------ public */

puddle_store *puddle_store_open(const char *data_dir, char *err, size_t errlen)
{
    if (!data_dir || !*data_dir || strlen(data_dir) > PUDDLE_PATH_MAX - 64) {
        seterr(err, errlen, "invalid data directory");
        return NULL;
    }
    puddle_store *s = new (std::nothrow) puddle_store();
    if (!s) {
        seterr(err, errlen, "out of memory");
        return NULL;
    }
    snprintf(s->dir, sizeof s->dir, "%s", data_dir);
    s->lock_fd = -1;

    char path[PUDDLE_PATH_MAX];
    snprintf(path, sizeof path, "%s/chats", s->dir);
    if (puddle_mkdir_p(path) != 0) {
        seterr(err, errlen, "cannot create %s: %s", path, strerror(errno));
        goto fail;
    }
    snprintf(path, sizeof path, "%s/lock", s->dir);
    s->lock_fd = puddle_fd_open(path, PUDDLE_F_READ | PUDDLE_F_WRITE | PUDDLE_F_CREATE);
    if (s->lock_fd < 0) {
        seterr(err, errlen, "cannot open %s: %s", path, strerror(errno));
        goto fail;
    }
    if (puddle_fd_lock(s->lock_fd) != 0) {
        if (errno == EWOULDBLOCK)
            seterr(err, errlen, "data directory %s is in use by another Puddle", s->dir);
        else
            seterr(err, errlen, "cannot lock %s: %s", path, strerror(errno));
        goto fail;
    }
    return s;
fail:
    puddle_store_close(s);
    return NULL;
}

void puddle_store_close(puddle_store *s)
{
    if (!s)
        return;
    for (size_t i = 0; i < s->nchats; i++)
        chat_free(s->chats[i]);
    if (s->lock_fd >= 0)
        puddle_fd_close(s->lock_fd); /* releases the lock */
    delete s;
}

size_t puddle_store_loaded_chats(puddle_store *s)
{
    s->mu.lock();
    size_t n = s->nchats;
    s->mu.unlock();
    return n;
}

static int append_locked(puddle_store *s, const char *chat_id, const char *event, size_t event_len,
                         const char *sig, int64_t rts_ms, char id_out[PUDDLE_ID_HEX + 1],
                         char *err, size_t errlen)
{
    int64_t ts;
    char meta_chat[MAX_CHAT_ID + 1], day[11];
    if (event_meta(event, event_len, &ts, meta_chat) != 0) {
        seterr(err, errlen, "event needs an integer \"ts\" (UTC ms, 2000-2100) and a \"chat\" id");
        return -1;
    }
    if (strcmp(meta_chat, chat_id) != 0) {
        seterr(err, errlen, "event is for chat \"%s\", not \"%s\"", meta_chat, chat_id);
        return -1;
    }
    day_of_ts(ts, day);

    char id[PUDDLE_ID_HEX + 1];
    puddle_sha256_hex(event, event_len, id);
    if (id_out)
        memcpy(id_out, id, sizeof id);

    int missing;
    chat *c = load_chat(s, chat_id, 1, &missing, err, errlen);
    if (!c)
        return -1;
    if (tab_find(c, id))
        return PUDDLE_APPEND_DUPLICATE;

    if (rts_ms == 0)
        rts_ms = now_ms();
    const char *sg = sig ? sig : "";

    size_t cap = event_len + strlen(sg) + 128;
    char *line = (char *)malloc(cap);
    if (!line) {
        seterr(err, errlen, "out of memory");
        return -1;
    }
    int n = snprintf(line, cap, "{\"id\":\"%s\",\"rts\":%lld,\"event\":", id, (long long)rts_ms);
    memcpy(line + n, event, event_len);
    n += (int)event_len;
    n += snprintf(line + n, cap - (size_t)n, ",\"sig\":\"%s\"}\n", sg);

    char dir[PUDDLE_PATH_MAX], path[PUDDLE_PATH_MAX], chats_dir[PUDDLE_PATH_MAX];
    snprintf(chats_dir, sizeof chats_dir, "%s/chats", s->dir);
    if (chat_dir(s, chat_id, dir, sizeof dir) != 0 || day_path(s, chat_id, day, path, sizeof path) != 0) {
        seterr(err, errlen, "path too long");
        free(line);
        return -1;
    }
    int made_dir = !puddle_path_exists(dir);
    int made_file = !puddle_path_exists(path);
    if (puddle_mkdir_p(dir) != 0) {
        seterr(err, errlen, "cannot create %s: %s", dir, strerror(errno));
        free(line);
        return -1;
    }
    int fd = puddle_fd_open(path, PUDDLE_F_WRITE | PUDDLE_F_APPEND | PUDDLE_F_CREATE);
    if (fd < 0) {
        seterr(err, errlen, "cannot open %s: %s", path, strerror(errno));
        free(line);
        return -1;
    }
    int64_t before = puddle_fd_end(fd);
    if (puddle_fd_write_all(fd, line, (size_t)n) != 0 || puddle_fsync_fd(fd) != 0) {
        seterr(err, errlen, "cannot write %s: %s", path, strerror(errno));
        /* don't leave a half-written record for the next append to land behind */
        if (before >= 0 && puddle_fd_truncate(fd, before) != 0)
            puddle_log(PUDDLE_LOG_ERROR, "store: could not roll back partial write to %s", path);
        puddle_fd_close(fd);
        free(line);
        return -1;
    }
    puddle_fd_close(fd);
    free(line);
    if (made_file)
        puddle_fsync_dir(dir);
    if (made_dir)
        puddle_fsync_dir(chats_dir);

    puddle_event *e = (puddle_event *)malloc(sizeof *e);
    if (!e || event_init(e, id, ts, rts_ms, event, event_len, sg) != 0) {
        free(e);
        seterr(err, errlen, "out of memory (event is on disk; it will load next time)");
        return -1;
    }
    if (chat_add(c, e) != 1) {
        puddle_event_free(e);
        free(e);
    }
    return PUDDLE_APPEND_NEW;
}

int puddle_store_append(puddle_store *s, const char *chat_id, const char *event, size_t event_len,
                        const char *sig, int64_t rts_ms, char id_out[PUDDLE_ID_HEX + 1],
                        char *err, size_t errlen)
{
    if (!valid_chat_id(chat_id)) {
        seterr(err, errlen, "invalid chat id");
        return -1;
    }
    if (event_len == 0 || event_len > MAX_EVENT_BYTES || memchr(event, '\n', event_len) ||
        memchr(event, '\r', event_len)) {
        seterr(err, errlen, "event must be one compact JSON object of at most %d bytes", MAX_EVENT_BYTES);
        return -1;
    }
    if (sig) {
        size_t sl = strlen(sig);
        if (sl > MAX_SIG) {
            seterr(err, errlen, "signature too long");
            return -1;
        }
        for (size_t i = 0; i < sl; i++) {
            char c = sig[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '+' || c == '/' || c == '=' || c == '-' || c == '_' || c == ':')) { /* ':' joins a key id to a signature */
                seterr(err, errlen, "signature must be base64");
                return -1;
            }
        }
    }
    s->mu.lock();
    int rc = append_locked(s, chat_id, event, event_len, sig, rts_ms, id_out, err, errlen);
    s->mu.unlock();
    if (rc == PUDDLE_APPEND_NEW && s->appended)
        s->appended(s->appended_user, chat_id);
    return rc;
}

void puddle_store_on_append(puddle_store *s, puddle_store_appended_fn fn, void *user)
{
    s->appended = fn;
    s->appended_user = user;
}

static int copy_tail(chat *c, size_t n, puddle_event_list *out)
{
    size_t take = c->n < n ? c->n : n;
    out->items = (puddle_event *)(take ? calloc(take, sizeof *out->items) : NULL);
    out->count = 0;
    if (take && !out->items)
        return -1;
    for (size_t i = 0; i < take; i++) {
        if (event_copy(&out->items[i], c->ev[c->n - take + i]) != 0) {
            out->count = i;
            puddle_event_list_free(out);
            return -1;
        }
    }
    out->count = take;
    return 0;
}

static int tail_locked(puddle_store *s, const char *chat_id, size_t n, puddle_event_list *out,
                       char *err, size_t errlen)
{
    out->items = NULL;
    out->count = 0;

    chat *loaded = find_loaded(s, chat_id);
    if (loaded)
        return copy_tail(loaded, n, out) == 0 ? 0 : (seterr(err, errlen, "out of memory"), -1);

    char dir[PUDDLE_PATH_MAX];
    dayname *days = NULL;
    size_t ndays = 0;
    if (chat_dir(s, chat_id, dir, sizeof dir) != 0) {
        seterr(err, errlen, "path too long");
        return -1;
    }
    if (list_days(dir, &days, &ndays) != 0) {
        if (errno == ENOENT)
            return 0;
        seterr(err, errlen, "cannot read %s: %s", dir, strerror(errno));
        return -1;
    }

    /* newest day first, until there are enough events. Newer days always sort after older ones. */
    chat *tmp = chat_new(chat_id);
    int rc = tmp ? 0 : -1;
    for (size_t i = ndays; rc == 0 && n > 0 && i-- > 0 && tmp->n < n;) {
        char path[PUDDLE_PATH_MAX];
        if (day_path(s, chat_id, days[i].day, path, sizeof path) != 0 ||
            read_day_file(tmp, path, days[i].day, 0) != 0)
            rc = -1;
    }
    free(days);
    if (rc == 0 && copy_tail(tmp, n, out) != 0)
        rc = -1;
    chat_free(tmp);
    if (rc != 0)
        seterr(err, errlen, "cannot read chat %s", chat_id);
    return rc;
}

int puddle_store_tail(puddle_store *s, const char *chat_id, size_t n, puddle_event_list *out,
                      char *err, size_t errlen)
{
    if (!valid_chat_id(chat_id)) {
        seterr(err, errlen, "invalid chat id");
        return -1;
    }
    s->mu.lock();
    int rc = tail_locked(s, chat_id, n, out, err, errlen);
    s->mu.unlock();
    return rc;
}

int puddle_store_get(puddle_store *s, const char *chat_id, const char *id, puddle_event *out,
                     char *err, size_t errlen)
{
    if (!valid_chat_id(chat_id) || !valid_id_hex(id)) {
        seterr(err, errlen, "invalid chat or event id");
        return -1;
    }
    s->mu.lock();
    int missing, rc;
    chat *c = load_chat(s, chat_id, 0, &missing, err, errlen);
    if (!c) {
        rc = missing ? 1 : -1;
    } else {
        puddle_event *e = tab_find(c, id);
        if (!e)
            rc = 1;
        else if (event_copy(out, e) != 0) {
            seterr(err, errlen, "out of memory");
            rc = -1;
        } else {
            rc = 0;
        }
    }
    s->mu.unlock();
    return rc;
}

static int id_ptr_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

/* hash the ids of ev[from..to) sorted ascending, joined by '\n' */
static int chunk_hash(puddle_event **ev, size_t from, size_t to, char out[PUDDLE_ID_HEX + 1])
{
    size_t n = to - from;
    const char **ids = (const char **)malloc(n * sizeof *ids);
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ids || !ctx) {
        free(ids);
        EVP_MD_CTX_free(ctx);
        return -1;
    }
    for (size_t i = 0; i < n; i++)
        ids[i] = ev[from + i]->id;
    qsort(ids, n, sizeof *ids, id_ptr_cmp);

    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    for (size_t i = 0; i < n; i++) {
        if (i)
            EVP_DigestUpdate(ctx, "\n", 1);
        EVP_DigestUpdate(ctx, ids[i], PUDDLE_ID_HEX);
    }
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned mdlen = 0;
    EVP_DigestFinal_ex(ctx, md, &mdlen);
    puddle_hex_encode(md, mdlen, out);
    EVP_MD_CTX_free(ctx);
    free(ids);
    return 0;
}

int puddle_store_chunks(puddle_store *s, const char *chat_id, puddle_chunk_list *out, char *err,
                        size_t errlen)
{
    out->items = NULL;
    out->count = 0;
    if (!valid_chat_id(chat_id)) {
        seterr(err, errlen, "invalid chat id");
        return -1;
    }
    s->mu.lock();
    int missing, rc = 0;
    chat *c = load_chat(s, chat_id, 0, &missing, err, errlen);
    if (!c) {
        rc = missing ? 0 : -1;
    } else if (c->n) {
        puddle_chunk *items = (puddle_chunk *)calloc(c->n, sizeof *items); /* upper bound: one chunk per event */
        size_t nitems = 0, start = 0;
        if (!items) {
            seterr(err, errlen, "out of memory");
            rc = -1;
        }
        while (rc == 0 && start < c->n) {
            char day[11], d2[11];
            day_of_ts(c->ev[start]->ts, day);
            size_t end = start + 1;
            while (end < c->n && day_of_ts(c->ev[end]->ts, d2) == 0 && strcmp(day, d2) == 0)
                end++;
            puddle_chunk *ch = &items[nitems++];
            memcpy(ch->day, day, sizeof ch->day);
            ch->count = end - start;
            if (chunk_hash(c->ev, start, end, ch->hash) != 0) {
                seterr(err, errlen, "out of memory");
                rc = -1;
            }
            start = end;
        }
        if (rc == 0) {
            out->items = items;
            out->count = nitems;
        } else {
            free(items);
        }
    }
    s->mu.unlock();
    return rc;
}
