#include "core/streams.h"

#include "base/encoding.h"
#include "base/json.h"
#include "base/log.h"

#include <errno.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <stdarg.h>
#include <mutex>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FILE_MAX (2u << 20)
#define RECORD_VERSION 1
#define INVITE_MAX_TTL_MS (30LL * 24 * 3600 * 1000)

typedef struct {
    char id[PUDDLE_USER_ID_LEN + 1];
    puddle_stream_state state;
    int has_key;
    int provisional; /* a request to an address, not yet answered (kept in the index so finding one does not read every file) */
    char kid[65];
    unsigned char sign_pub[32];
} entry;

typedef struct {
    char id[PUDDLE_USER_ID_LEN + 1];
    int64_t ts;
} blocked;

struct puddle_streams {
    std::mutex mu;
    puddle_storage *storage;
    puddle_storage *owned; /* the files driver opened for a data directory; NULL when handed one */
    entry *entries;
    size_t nentries, capentries;
    blocked *blocks;
    size_t nblocks;
    blocked *reads; /* reuses the shape: an id and a time */
    size_t nreads;
    puddle_invite invites[PUDDLE_MAX_INVITES];
    size_t ninvites;
    puddle_access access;
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

/* ---- names ---- */

static const char *const STATE_NAMES[] = { "pending_out", "pending_in", "active", "closed", "scoped" };
#define STATE_COUNT 5

const char *puddle_stream_state_name(puddle_stream_state s)
{
    return (unsigned)s < STATE_COUNT ? STATE_NAMES[s] : "closed";
}

int puddle_stream_state_parse(const char *name, puddle_stream_state *out)
{
    for (int i = 0; i < STATE_COUNT; i++)
        if (name && !strcmp(name, STATE_NAMES[i])) {
            *out = (puddle_stream_state)i;
            return 0;
        }
    return -1;
}

const char *puddle_access_name(puddle_access a)
{
    return a == PUDDLE_ACCESS_OPEN ? "open" : a == PUDDLE_ACCESS_CLOSED ? "closed" : "manual";
}

int puddle_access_parse(const char *name, puddle_access *out)
{
    if (!name)
        return -1;
    if (!strcmp(name, "manual")) *out = PUDDLE_ACCESS_MANUAL;
    else if (!strcmp(name, "open")) *out = PUDDLE_ACCESS_OPEN;
    else if (!strcmp(name, "closed")) *out = PUDDLE_ACCESS_CLOSED;
    else return -1;
    return 0;
}

/* ---- the record ---- */

int puddle_stream_carries_groups(const puddle_stream *st)
{
    return st->state == PUDDLE_STREAM_ACTIVE || st->state == PUDDLE_STREAM_SCOPED || st->group_ok;
}

int puddle_stream_init(puddle_stream *st, const char *id, puddle_stream_state state, int64_t now_ms)
{
    memset(st, 0, sizeof *st);
    st->chain_sent = st->addr_sent = -1;
    if (!puddle_id_valid(id))
        return -1;
    memcpy(st->id, id, PUDDLE_USER_ID_LEN + 1);
    st->state = state;
    st->created_ms = st->updated_ms = now_ms;
    return 0;
}

static int set_text(char **dst, size_t *dst_len, const char *text, size_t len, size_t max)
{
    free(*dst);
    *dst = NULL;
    if (dst_len)
        *dst_len = 0;
    if (!text)
        return 0;
    if (len > max)
        return -1;
    char *c = (char *)malloc(len + 1);
    if (!c)
        return -1;
    memcpy(c, text, len);
    c[len] = '\0';
    *dst = c;
    if (dst_len)
        *dst_len = len;
    return 0;
}

int puddle_stream_set_chain(puddle_stream *st, const char *text, size_t len)
{
    return set_text(&st->chain, &st->chain_len, text, len, PUDDLE_CHAIN_MAX_BYTES);
}

int puddle_stream_add_address(puddle_stream *st, const char *address)
{
    puddle_address a;
    if (puddle_address_parse(address, &a) != 0)
        return -1;
    for (int i = 0; i < st->naddr; i++)
        if (!strcmp(st->addresses[i], a.address))
            return 0;
    if (st->naddr >= PUDDLE_STREAM_MAX_ADDRS)
        return -1;
    snprintf(st->addresses[st->naddr++], PUDDLE_ADDR_MAX, "%s", a.address);
    return 0;
}

void puddle_stream_free(puddle_stream *st)
{
    free(st->chain);
    st->chain = NULL;
    st->chain_len = 0;
}

/* ---- files ---- */

#define COLLECTION "streams" /* one record per peer, keyed by their user id */

/* a stream record, or NULL if there is none (or it is too large to be one) */
static char *read_record(const puddle_streams *s, const char *id, size_t *len)
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

/* one of the single documents (blocks.tsv, read.tsv, invites.tsv, access-policy), or NULL */
static char *read_state(const puddle_streams *s, const char *name, size_t *len)
{
    char *text;
    if (puddle_record_get(s->storage, "state", name, &text, len) != 0)
        return NULL;
    if (*len > FILE_MAX) {
        free(text);
        return NULL;
    }
    return text;
}

/* what goes in a file: JSON, one object */
static char *encode(const puddle_stream *st, size_t *out_len)
{
    size_t note_len = strlen(st->note);
    size_t cap = (st->chain_len + note_len) * 6 + 4096;
    char *buf = (char *)malloc(cap);
    if (!buf)
        return NULL;
    puddle_jw w;
    puddle_jw_init(&w, buf, cap);
    puddle_jw_object(&w);
    puddle_jw_num(&w, "v", RECORD_VERSION);
    puddle_jw_str(&w, "id", st->id);
    puddle_jw_str(&w, "state", puddle_stream_state_name(st->state));
    if (st->owed != PUDDLE_OWED_NONE)
        puddle_jw_num(&w, "owed", st->owed);
    puddle_jw_num(&w, "created", st->created_ms);
    puddle_jw_num(&w, "updated", st->updated_ms);
    if (st->tls_fp[0])
        puddle_jw_str(&w, "tls_fp", st->tls_fp);
    if (st->tls_fp_seen[0])
        puddle_jw_str(&w, "tls_fp_seen", st->tls_fp_seen);
    if (st->ticket[0])
        puddle_jw_str(&w, "ticket", st->ticket);
    if (st->provisional)
        puddle_jw_flag(&w, "provisional", 1);
    if (st->scoped_req)
        puddle_jw_flag(&w, "scoped_req", 1);
    if (st->group_ok)
        puddle_jw_flag(&w, "group_ok", 1);
    if (st->error[0])
        puddle_jw_str(&w, "error", st->error);
    if (st->chain_sent >= 0)
        puddle_jw_num(&w, "chain_sent", st->chain_sent);
    if (st->addr_sent >= 0)
        puddle_jw_num(&w, "addr_sent", st->addr_sent);
    puddle_jw_str(&w, "note", st->note);
    char addrs[PUDDLE_STREAM_MAX_ADDRS * PUDDLE_ADDR_MAX + 8] = "";
    for (int i = 0; i < st->naddr; i++) {
        if (i)
            strcat(addrs, " ");
        strcat(addrs, st->addresses[i]); /* an address has no spaces, and the list is bounded */
    }
    puddle_jw_str(&w, "addresses", addrs);
    puddle_jw_num(&w, "addr_seq", st->addr_seq);
    if (st->chain) {
        puddle_jw_key(&w, "chain");
        puddle_jw_string_n(&w, st->chain, st->chain_len);
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

static int member_string(const char *json, size_t len, const char *key, char *out, size_t cap)
{
    puddle_span v;
    return puddle_json_member(json, len, key, &v) == 0 && puddle_json_string(v, out, cap) == 0 ? 0 : -1;
}

static int member_int(const char *json, size_t len, const char *key, int64_t *out)
{
    puddle_span v;
    return puddle_json_member(json, len, key, &v) == 0 && puddle_json_int64(v, out) == 0 ? 0 : -1;
}

/* Read one record back, checking everything: the file is ours, but it may be damaged or edited. */
/* `old_profile`, if given, is pointed at the profile a record from before core/people.h still carries (len 0 if none) */
static int decode(const char *json, size_t len, const char *expect_id, puddle_stream *st, puddle_chain_head *head,
                  int *has_head, puddle_span *old_profile = nullptr)
{
    char tmp[PUDDLE_STREAM_MAX_ADDRS * PUDDLE_ADDR_MAX + 8];
    int64_t v;
    puddle_stream_state state;
    *has_head = 0;
    if (!puddle_json_valid_object(json, len))
        return -1;
    if (member_int(json, len, "v", &v) != 0 || v != RECORD_VERSION)
        return -1;
    if (member_string(json, len, "id", tmp, PUDDLE_USER_ID_LEN + 1) != 0 || strcmp(tmp, expect_id) != 0)
        return -1;
    if (member_string(json, len, "state", tmp, sizeof tmp) != 0 || puddle_stream_state_parse(tmp, &state) != 0)
        return -1;
    if (puddle_stream_init(st, expect_id, state, 0) != 0)
        return -1;
    if (member_int(json, len, "created", &st->created_ms) != 0 || member_int(json, len, "updated", &st->updated_ms) != 0 ||
        member_int(json, len, "addr_seq", &st->addr_seq) != 0)
        return -1;
    puddle_span ow;
    int orc = puddle_json_member(json, len, "owed", &ow);
    if (orc < 0)
        return -1;
    if (orc == 0) {
        int64_t o;
        if (puddle_json_int64(ow, &o) != 0 || o < 0 || o > PUDDLE_OWED_SCOPED)
            return -1;
        st->owed = (puddle_owed)o;
    }
    for (int k = 0; k < 2; k++) {
        const char *key = k ? "tls_fp_seen" : "tls_fp";
        char *dst = k ? st->tls_fp_seen : st->tls_fp;
        puddle_span fv;
        int frc = puddle_json_member(json, len, key, &fv);
        if (frc < 0)
            return -1;
        if (frc == 0) {
            unsigned char raw[32];
            if (puddle_json_string(fv, dst, 65) != 0 || puddle_hex_decode(dst, raw, 32) != 0)
                return -1;
        }
    }
    for (int k = 0; k < 2; k++) {
        puddle_span nv;
        int nrc = puddle_json_member(json, len, k ? "addr_sent" : "chain_sent", &nv);
        int64_t *dst = k ? &st->addr_sent : &st->chain_sent;
        if (nrc < 0 || (nrc == 0 && (puddle_json_int64(nv, dst) != 0 || *dst < 0)))
            return -1;
    }
    {
        puddle_span tv;
        int trc = puddle_json_member(json, len, "ticket", &tv);
        unsigned char raw[16];
        if (trc < 0 || (trc == 0 && (puddle_json_string(tv, st->ticket, sizeof st->ticket) != 0 || puddle_hex_decode(st->ticket, raw, 16) != 0)))
            return -1;
        int prc = puddle_json_member(json, len, "provisional", &tv);
        if (prc < 0)
            return -1;
        st->provisional = prc == 0 && tv.len == 4 && !memcmp(tv.p, "true", 4);
        int src = puddle_json_member(json, len, "scoped_req", &tv);
        if (src < 0)
            return -1;
        st->scoped_req = src == 0 && tv.len == 4 && !memcmp(tv.p, "true", 4);
        int grc = puddle_json_member(json, len, "group_ok", &tv);
        if (grc < 0)
            return -1;
        st->group_ok = grc == 0 && tv.len == 4 && !memcmp(tv.p, "true", 4);
    }
    {
        puddle_span ev;
        int erc = puddle_json_member(json, len, "error", &ev);
        if (erc < 0 || (erc == 0 && puddle_json_string(ev, st->error, sizeof st->error) != 0))
            return -1;
    }
    if (member_string(json, len, "note", st->note, sizeof st->note) != 0)
        return -1;
    if (member_string(json, len, "addresses", tmp, sizeof tmp) != 0)
        return -1;
    for (char *save = NULL, *tok = strtok_r(tmp, " ", &save); tok; tok = strtok_r(NULL, " ", &save))
        if (puddle_stream_add_address(st, tok) != 0)
            goto bad;
    puddle_span sp;
    int rc; rc = puddle_json_member(json, len, "profile", &sp);
    if (rc < 0 || (rc == 0 && (sp.len > PUDDLE_PROFILE_MAX_BYTES || !puddle_json_valid_object(sp.p, sp.len))))
        goto bad;
    if (old_profile)
        *old_profile = rc == 0 ? sp : puddle_span{ NULL, 0 };
    rc = puddle_json_member(json, len, "chain", &sp);
    if (rc < 0)
        goto bad;
    if (rc == 0) {
        char *chain = (char *)malloc(sp.len + 1);
        if (!chain)
            goto bad;
        if (puddle_json_string(sp, chain, sp.len + 1) != 0) {
            free(chain);
            goto bad;
        }
        size_t cl = strlen(chain);
        char err[200];
        int ok = puddle_chain_verify(chain, cl, expect_id, head, err, sizeof err) == 0;
        if (ok && puddle_stream_set_chain(st, chain, cl) != 0)
            ok = 0;
        free(chain);
        if (!ok)
            goto bad;
        *has_head = 1;
    }
    return 0;
bad:
    puddle_stream_free(st);
    return -1;
}

/* ---- the index ---- */

static entry *find_entry(puddle_streams *s, const char *id)
{
    for (size_t i = 0; i < s->nentries; i++)
        if (!strcmp(s->entries[i].id, id))
            return &s->entries[i];
    return NULL;
}

static int index_put(puddle_streams *s, const char *id, puddle_stream_state state, const puddle_chain_head *head, int provisional)
{
    entry *e = find_entry(s, id);
    if (!e) {
        if (s->nentries == s->capentries) {
            size_t cap = s->capentries ? s->capentries * 2 : 16;
            entry *n = (entry *)realloc(s->entries, cap * sizeof *n);
            if (!n)
                return -1;
            s->entries = n;
            s->capentries = cap;
        }
        e = &s->entries[s->nentries++];
        memset(e, 0, sizeof *e);
        memcpy(e->id, id, PUDDLE_USER_ID_LEN + 1);
    }
    e->state = state;
    e->provisional = provisional;
    e->has_key = head != NULL;
    if (head) {
        memcpy(e->sign_pub, head->sign_pub, 32);
        puddle_key_id(head->sign_pub, e->kid);
    }
    return 0;
}

/* ---- small tab-separated files ---- */

static int write_state_file(puddle_streams *s, const char *name, const char *data, size_t len)
{
    return puddle_record_put(s->storage, "state", name, data, len);
}

static int save_blocks(puddle_streams *s)
{
    const size_t cap = s->nblocks * (PUDDLE_USER_ID_LEN + 24) + 1;
    char *buf = (char *)malloc(cap);
    if (!buf)
        return -1;
    size_t n = 0;
    for (size_t i = 0; i < s->nblocks; i++)
        n += (size_t)snprintf(buf + n, cap - n, "%s\t%lld\n", s->blocks[i].id, (long long)s->blocks[i].ts);
    int rc = write_state_file(s, "blocks.tsv", buf, n);
    free(buf);
    return rc;
}

static int save_invites(puddle_streams *s)
{
    char buf[PUDDLE_MAX_INVITES * 96 + 1];
    size_t n = 0;
    for (size_t i = 0; i < s->ninvites; i++)
        n += (size_t)snprintf(buf + n, sizeof buf - n, "%s\t%lld\t%lld\t%d\n", s->invites[i].token, (long long)s->invites[i].created_ms,
                             (long long)s->invites[i].expires_ms, s->invites[i].uses);
    return write_state_file(s, "invites.tsv", buf, n);
}

static void load_blocks(puddle_streams *s)
{
    size_t len;
    char *text = read_state(s, "blocks.tsv", &len);
    if (!text)
        return;
    for (char *save = NULL, *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *tab = strchr(line, '\t');
        if (!tab)
            continue;
        *tab = '\0';
        if (!puddle_id_valid(line))
            continue;
        blocked *n = (blocked *)realloc(s->blocks, (s->nblocks + 1) * sizeof *n);
        if (!n)
            break;
        s->blocks = n;
        memcpy(s->blocks[s->nblocks].id, line, PUDDLE_USER_ID_LEN + 1);
        s->blocks[s->nblocks].ts = atoll(tab + 1);
        s->nblocks++;
    }
    free(text);
}

static int save_reads(puddle_streams *s)
{
    char *buf = (char *)malloc(s->nreads * (PUDDLE_USER_ID_LEN + 24) + 1);
    if (!buf)
        return -1;
    size_t n = 0;
    for (size_t i = 0; i < s->nreads; i++)
        n += (size_t)snprintf(buf + n, PUDDLE_USER_ID_LEN + 24, "%s\t%lld\n", s->reads[i].id, (long long)s->reads[i].ts);
    int rc = write_state_file(s, "read.tsv", buf, n);
    free(buf);
    return rc;
}

static void load_reads(puddle_streams *s)
{
    size_t len;
    char *text = read_state(s, "read.tsv", &len);
    if (!text)
        return;
    for (char *save = NULL, *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *tab = strchr(line, '\t');
        if (!tab)
            continue;
        *tab = '\0';
        if (!puddle_id_valid(line))
            continue;
        blocked *n = (blocked *)realloc(s->reads, (s->nreads + 1) * sizeof *n);
        if (!n)
            break;
        s->reads = n;
        memcpy(s->reads[s->nreads].id, line, PUDDLE_USER_ID_LEN + 1);
        s->reads[s->nreads].ts = atoll(tab + 1);
        s->nreads++;
    }
    free(text);
}

int64_t puddle_streams_read_marker(puddle_streams *s, const char *id)
{
    int64_t ts = 0;
    if (!puddle_id_valid(id))
        return 0;
    s->mu.lock();
    for (size_t i = 0; i < s->nreads; i++)
        if (!strcmp(s->reads[i].id, id))
            ts = s->reads[i].ts;
    s->mu.unlock();
    return ts;
}

int puddle_streams_set_read_marker(puddle_streams *s, const char *id, int64_t ts_ms, char *err, size_t errlen)
{
    if (!puddle_id_valid(id) || ts_ms < 0) {
        seterr(err, errlen, "not a valid user id");
        return -1;
    }
    int rc = 0;
    s->mu.lock();
    blocked *at = NULL;
    for (size_t i = 0; i < s->nreads; i++)
        if (!strcmp(s->reads[i].id, id))
            at = &s->reads[i];
    if (at && at->ts >= ts_ms) {
        s->mu.unlock(); /* never backwards */
        return 0;
    }
    if (at) {
        int64_t old = at->ts;
        at->ts = ts_ms;
        if (save_reads(s) != 0) {
            at->ts = old;
            seterr(err, errlen, "cannot save: %s", strerror(errno));
            rc = -1;
        }
    } else {
        blocked *n = (blocked *)realloc(s->reads, (s->nreads + 1) * sizeof *n);
        if (!n) {
            seterr(err, errlen, "out of memory");
            rc = -1;
        } else {
            s->reads = n;
            memcpy(s->reads[s->nreads].id, id, PUDDLE_USER_ID_LEN + 1);
            s->reads[s->nreads].ts = ts_ms;
            s->nreads++;
            if (save_reads(s) != 0) {
                s->nreads--;
                seterr(err, errlen, "cannot save: %s", strerror(errno));
                rc = -1;
            }
        }
    }
    s->mu.unlock();
    return rc;
}

static void load_invites(puddle_streams *s)
{
    size_t len;
    char *text = read_state(s, "invites.tsv", &len);
    if (!text)
        return;
    for (char *save = NULL, *line = strtok_r(text, "\n", &save); line && s->ninvites < PUDDLE_MAX_INVITES;
         line = strtok_r(NULL, "\n", &save)) {
        puddle_invite inv;
        char tok[64];
        long long c, e;
        int u;
        if (sscanf(line, "%63[^\t]\t%lld\t%lld\t%d", tok, &c, &e, &u) != 4 || !puddle_invite_token_valid(tok) || u < 1)
            continue;
        memset(&inv, 0, sizeof inv);
        memcpy(inv.token, tok, sizeof inv.token);
        inv.created_ms = c;
        inv.expires_ms = e;
        inv.uses = u;
        s->invites[s->ninvites++] = inv;
    }
    free(text);
}

/* ---- open / close ---- */

/* one stream record, into the index; a damaged one is skipped rather than stopping the Puddle */
static int load_record(void *user, const char *id, const char *text, size_t len)
{
    puddle_streams *s = (puddle_streams *)user;
    if (!puddle_id_valid(id) || len > FILE_MAX)
        return 0;
    puddle_stream st;
    puddle_chain_head head;
    int has_head;
    if (decode(text, len, id, &st, &head, &has_head) != 0) {
        puddle_log(PUDDLE_LOG_WARN, "skipping damaged stream record %s", id);
        return 0;
    }
    index_put(s, id, st.state, has_head ? &head : NULL, st.provisional);
    puddle_stream_free(&st);
    return 0;
}

puddle_streams *puddle_streams_open(const char *data_dir, char *err, size_t errlen)
{
    puddle_storage *st = puddle_storage_open_files(data_dir, err, errlen);
    if (!st)
        return NULL;
    puddle_streams *s = puddle_streams_open_with(st, err, errlen);
    if (!s) {
        puddle_storage_close(st);
        return NULL;
    }
    s->owned = st;
    return s;
}

puddle_streams *puddle_streams_open_with(puddle_storage *storage, char *err, size_t errlen)
{
    puddle_streams *s = new (std::nothrow) puddle_streams();
    if (!s) {
        seterr(err, errlen, "out of memory");
        return NULL;
    }
    s->storage = storage;
    if (puddle_record_each(storage, COLLECTION, load_record, s) != 0) {
        seterr(err, errlen, "cannot read the stream records: %s", strerror(errno));
        puddle_streams_close(s);
        return NULL;
    }

    load_blocks(s);
    load_reads(s);
    load_invites(s);
    char word[32] = "";
    size_t len;
    char *text = read_state(s, "access-policy", &len);
    if (text) {
        sscanf(text, "%31s", word);
        free(text);
    }
    if (puddle_access_parse(word, &s->access) != 0)
        s->access = PUDDLE_ACCESS_MANUAL;
    return s;
}

int puddle_streams_take_profiles(puddle_streams *s, puddle_stream_profile_fn fn, void *user)
{
    std::lock_guard<std::mutex> hold(s->mu);
    int moved = 0, rc = 0;
    for (size_t i = 0; i < s->nentries; i++) {
        const char *id = s->entries[i].id;
        size_t len;
        char *text = read_record(s, id, &len);
        if (!text)
            continue;
        puddle_stream st;
        puddle_chain_head head;
        int has_head;
        puddle_span prof = { NULL, 0 };
        if (decode(text, len, id, &st, &head, &has_head, &prof) == 0) {
            if (prof.len && fn(user, id, prof.p, prof.len, st.updated_ms) == 0) {
                size_t n;
                char *out = encode(&st, &n); /* the same record, now without it */
                if (out && puddle_record_put(s->storage, COLLECTION, id, out, n) == 0)
                    moved++;
                else
                    rc = -1;
                free(out);
            }
            puddle_stream_free(&st);
        }
        free(text);
    }
    return rc < 0 ? -1 : moved;
}

void puddle_streams_close(puddle_streams *s)
{
    if (!s)
        return;
    free(s->entries);
    free(s->blocks);
    free(s->reads);
    puddle_storage_close(s->owned);
    delete s;
}

/* ---- peers ---- */

static int is_blocked_locked(const puddle_streams *s, const char *id)
{
    for (size_t i = 0; i < s->nblocks; i++)
        if (!strcmp(s->blocks[i].id, id))
            return 1;
    return 0;
}

int puddle_streams_get(puddle_streams *s, const char *id_in, puddle_stream *out)
{
    if (!puddle_id_valid(id_in))
        return -1;
    char id[PUDDLE_USER_ID_LEN + 1]; /* `id_in` may point into `out`, which decoding starts by clearing */
    memcpy(id, id_in, sizeof id);
    size_t len;
    s->mu.lock();
    char *text = read_record(s, id, &len);
    s->mu.unlock();
    if (!text)
        return 1;
    puddle_chain_head head;
    int has_head;
    int rc = decode(text, len, id, out, &head, &has_head);
    free(text);
    return rc == 0 ? 0 : 1;
}

/* caller holds the lock */
static int put_locked(puddle_streams *s, const puddle_stream *st, char *err, size_t errlen)
{
    puddle_chain_head head;
    if (st->chain && puddle_chain_verify(st->chain, st->chain_len, st->id, &head, err, errlen) != 0)
        return -1;
    size_t n;
    char *text = encode(st, &n);
    if (!text) {
        seterr(err, errlen, "cannot encode the stream record");
        return -1;
    }
    int rc = puddle_record_put(s->storage, COLLECTION, st->id, text, n);
    free(text);
    if (rc != 0) {
        seterr(err, errlen, "cannot write the stream record for %s: %s", st->id, strerror(errno));
        return -1;
    }
    if (index_put(s, st->id, st->state, st->chain ? &head : NULL, st->provisional) != 0) {
        seterr(err, errlen, "out of memory");
        return -1;
    }
    return 0;
}

int puddle_streams_put(puddle_streams *s, const puddle_stream *st, char *err, size_t errlen)
{
    if (!puddle_id_valid(st->id)) {
        seterr(err, errlen, "not a valid user id");
        return -1;
    }
    if (st->naddr < 0 || st->naddr > PUDDLE_STREAM_MAX_ADDRS || memchr(st->note, '\0', sizeof st->note) == NULL) {
        seterr(err, errlen, "the stream record is malformed");
        return -1;
    }
    s->mu.lock();
    int rc;
    if (is_blocked_locked(s, st->id)) {
        seterr(err, errlen, "that user is blocked");
        rc = -1;
    } else {
        rc = put_locked(s, st, err, errlen);
    }
    s->mu.unlock();
    return rc;
}

int puddle_streams_update(puddle_streams *s, const char *id_in, puddle_stream_update_fn fn, void *user, char *err,
                          size_t errlen)
{
    if (!puddle_id_valid(id_in)) {
        seterr(err, errlen, "not a valid user id");
        return -1;
    }
    char id[PUDDLE_USER_ID_LEN + 1];
    memcpy(id, id_in, sizeof id);
    size_t len = 0;
    s->mu.lock();
    int rc = -1;
    puddle_stream st;
    puddle_chain_head head;
    int has_head;
    char *text = read_record(s, id, &len);
    int exists = text && decode(text, len, id, &st, &head, &has_head) == 0;
    free(text);
    if (!exists && puddle_stream_init(&st, id, PUDDLE_STREAM_PENDING_IN, 0) != 0) {
        seterr(err, errlen, "not a valid user id");
        goto out;
    }
    int r; r = fn(user, &st, exists);
    if (r > 0) {
        rc = r;
    } else if (r == 0) {
        if (is_blocked_locked(s, id)) /* nothing new is recorded about someone blocked */
            seterr(err, errlen, "that user is blocked");
        else
            rc = put_locked(s, &st, err, errlen) == 0 ? 0 : -1;
    } else {
        seterr(err, errlen, "not changed");
    }
    puddle_stream_free(&st);
out:
    s->mu.unlock();
    return rc;
}

/* ---- requests to an address ---- */

int puddle_ticket_new(char out[33])
{
    unsigned char raw[16];
    static const char hex[] = "0123456789abcdef";
    if (RAND_bytes(raw, sizeof raw) != 1)
        return -1;
    for (int i = 0; i < 16; i++) {
        out[2 * i] = hex[raw[i] >> 4];
        out[2 * i + 1] = hex[raw[i] & 15];
    }
    out[32] = '\0';
    return 0;
}

int puddle_provisional_id(const char *ticket, char out[PUDDLE_USER_ID_LEN + 1])
{
    char text[64];
    unsigned char h[32];
    if (!ticket || strlen(ticket) != 32)
        return -1;
    snprintf(text, sizeof text, "puddle-provisional\n%s", ticket);
    SHA256((const unsigned char *)text, strlen(text), h);
    return puddle_base32_encode(h, 32, out, PUDDLE_USER_ID_LEN + 1) == PUDDLE_USER_ID_LEN ? 0 : -1;
}

/* caller holds the lock: the provisional record that `match` accepts */
static int find_provisional_locked(puddle_streams *s, int (*match)(const puddle_stream *, const char *), const char *key, puddle_stream *out)
{
    for (size_t i = 0; i < s->nentries; i++) {
        if (!s->entries[i].provisional)
            continue;
        size_t len;
        char *text = read_record(s, s->entries[i].id, &len);
        if (!text)
            continue;
        puddle_stream st;
        puddle_chain_head head;
        int has_head;
        int ok = decode(text, len, s->entries[i].id, &st, &head, &has_head) == 0;
        free(text);
        if (!ok)
            continue;
        if (st.provisional && match(&st, key)) {
            *out = st;
            return 0;
        }
        puddle_stream_free(&st);
    }
    return 1;
}

static int match_ticket(const puddle_stream *st, const char *t) { return t[0] && !strcmp(st->ticket, t); }
static int match_address(const puddle_stream *st, const char *a) { return st->state == PUDDLE_STREAM_PENDING_OUT && st->naddr > 0 && !strcmp(st->addresses[0], a); }

int puddle_streams_find_ticket(puddle_streams *s, const char *ticket, puddle_stream *out)
{
    s->mu.lock();
    int rc = find_provisional_locked(s, match_ticket, ticket, out);
    s->mu.unlock();
    return rc;
}

int puddle_streams_find_provisional(puddle_streams *s, const char *address, puddle_stream *out)
{
    s->mu.lock();
    int rc = find_provisional_locked(s, match_address, address, out);
    s->mu.unlock();
    return rc;
}

/* caller holds the lock */
static void drop_locked(puddle_streams *s, const char *id)
{
    puddle_record_remove(s->storage, COLLECTION, id);
    for (size_t i = 0; i < s->nentries; i++)
        if (!strcmp(s->entries[i].id, id)) {
            memmove(&s->entries[i], &s->entries[i + 1], (s->nentries - i - 1) * sizeof *s->entries);
            s->nentries--;
            break;
        }
}

int puddle_streams_adopt(puddle_streams *s, const char *provisional_id, const char *real_id, char *err, size_t errlen)
{
    if (!puddle_id_valid(provisional_id) || !puddle_id_valid(real_id) || !strcmp(provisional_id, real_id)) {
        seterr(err, errlen, "not a valid user id");
        return -1;
    }
    size_t len;
    s->mu.lock();
    int rc = -1;
    char *text = read_record(s, provisional_id, &len);
    puddle_stream st;
    puddle_chain_head head;
    int has_head;
    if (!text || decode(text, len, provisional_id, &st, &head, &has_head) != 0) {
        free(text);
        seterr(err, errlen, "no such request");
        goto out;
    }
    free(text);
    if (!st.provisional) {
        puddle_stream_free(&st);
        seterr(err, errlen, "not a provisional request");
        goto out;
    }
    if (is_blocked_locked(s, real_id)) {
        drop_locked(s, provisional_id);
        puddle_stream_free(&st);
        rc = 2;
        goto out;
    }
    if (find_entry(s, real_id)) {
        /* We know them already. If that stream was closed, or only for groups, this request starts it afresh; any other state stands. */
        puddle_stream old;
        size_t rlen;
        char *rtext = read_record(s, real_id, &rlen);
        if (rtext && decode(rtext, rlen, real_id, &old, &head, &has_head) == 0) {
            if (old.state == PUDDLE_STREAM_CLOSED || old.state == PUDDLE_STREAM_SCOPED) { /* a group-only connection becomes a friend request */
                old.state = PUDDLE_STREAM_PENDING_OUT;
                old.owed = PUDDLE_OWED_NONE;
                old.error[0] = '\0';
                old.updated_ms = st.updated_ms;
                snprintf(old.note, sizeof old.note, "%s", st.note);
                for (int k = 0; k < st.naddr; k++)
                    puddle_stream_add_address(&old, st.addresses[k]);
                if (put_locked(s, &old, err, errlen) == 0) {
                    drop_locked(s, provisional_id);
                    rc = 0;
                }
            } else {
                drop_locked(s, provisional_id);
                rc = 1;
            }
            puddle_stream_free(&old);
        } else {
            drop_locked(s, provisional_id);
            rc = 1;
        }
        free(rtext);
        puddle_stream_free(&st);
        goto out;
    }
    memcpy(st.id, real_id, PUDDLE_USER_ID_LEN + 1);
    st.provisional = 0;
    st.ticket[0] = '\0';
    if (put_locked(s, &st, err, errlen) != 0) {
        puddle_stream_free(&st);
        goto out;
    }
    drop_locked(s, provisional_id);
    puddle_stream_free(&st);
    rc = 0;
out:
    s->mu.unlock();
    return rc;
}

int puddle_streams_forget(puddle_streams *s, const char *provisional_id)
{
    if (!puddle_id_valid(provisional_id))
        return -1;
    size_t len;
    s->mu.lock();
    char *text = read_record(s, provisional_id, &len);
    puddle_stream st;
    puddle_chain_head head;
    int has_head;
    int rc = 1;
    if (text && decode(text, len, provisional_id, &st, &head, &has_head) == 0) {
        if (st.provisional) {
            drop_locked(s, provisional_id);
            rc = 0;
        }
        puddle_stream_free(&st);
    }
    free(text);
    s->mu.unlock();
    return rc;
}

int puddle_streams_list(puddle_streams *s, puddle_stream **out, size_t *count)
{
    *out = NULL;
    *count = 0;
    s->mu.lock();
    size_t n = s->nentries;
    char (*ids)[PUDDLE_USER_ID_LEN + 1] = (char (*)[53])malloc((n ? n : 1) * sizeof *ids);
    if (!ids) {
        s->mu.unlock();
        return -1;
    }
    for (size_t i = 0; i < n; i++)
        memcpy(ids[i], s->entries[i].id, PUDDLE_USER_ID_LEN + 1);
    s->mu.unlock();

    puddle_stream *list = (puddle_stream *)calloc(n ? n : 1, sizeof *list);
    if (!list) {
        free(ids);
        return -1;
    }
    size_t k = 0;
    for (size_t i = 0; i < n; i++)
        if (puddle_streams_get(s, ids[i], &list[k]) == 0)
            k++;
    free(ids);
    *out = list;
    *count = k;
    return 0;
}

void puddle_stream_list_free(puddle_stream *list, size_t count)
{
    for (size_t i = 0; i < count; i++)
        puddle_stream_free(&list[i]);
    free(list);
}

size_t puddle_streams_count(puddle_streams *s, puddle_stream_state state)
{
    size_t n = 0;
    s->mu.lock();
    for (size_t i = 0; i < s->nentries; i++)
        if (s->entries[i].state == state)
            n++;
    s->mu.unlock();
    return n;
}

size_t puddle_streams_size(puddle_streams *s)
{
    s->mu.lock();
    size_t n = s->nentries;
    s->mu.unlock();
    return n;
}

int puddle_streams_peer_key(void *streams, const char *sender_id, const char *kid, unsigned char sign_pub[32])
{
    puddle_streams *s = (puddle_streams *)streams;
    if (!puddle_id_valid(sender_id) || !kid)
        return 0;
    int ok = 0;
    s->mu.lock();
    entry *e = find_entry(s, sender_id);
    if (e && e->has_key && !is_blocked_locked(s, sender_id) && strcmp(e->kid, kid) == 0) {
        memcpy(sign_pub, e->sign_pub, 32);
        ok = 1;
    }
    s->mu.unlock();
    return ok;
}

/* ---- certificates ---- */

static int is_fp(const char *fp)
{
    unsigned char raw[32];
    return fp && strlen(fp) == 64 && puddle_hex_decode(fp, raw, 32) == 0;
}

typedef struct {
    const char *fp;
    int result;
} fp_change;

static int fp_apply(void *user, puddle_stream *st, int exists)
{
    fp_change *c = (fp_change *)user;
    c->result = 1;
    if (!exists)
        return 1; /* no record of them: nobody's to judge */
    if (!st->tls_fp[0]) {
        snprintf(st->tls_fp, sizeof st->tls_fp, "%s", c->fp);
        st->tls_fp_seen[0] = '\0';
        return 0;
    }
    if (strcmp(st->tls_fp, c->fp) != 0) {
        c->result = 0;
        if (strcmp(st->tls_fp_seen, c->fp) == 0)
            return 2; /* already noted */
        snprintf(st->tls_fp_seen, sizeof st->tls_fp_seen, "%s", c->fp);
        return 0;
    }
    if (st->tls_fp_seen[0]) {
        st->tls_fp_seen[0] = '\0'; /* the old certificate is back: nothing to decide */
        return 0;
    }
    return 2;
}

int puddle_streams_check_fingerprint(puddle_streams *s, const char *id, const char *fp)
{
    if (!puddle_id_valid(id) || !is_fp(fp))
        return 0;
    fp_change c = { fp, 1 };
    char err[200];
    puddle_streams_update(s, id, fp_apply, &c, err, sizeof err); /* if the write fails we only lose the note, and ask again next time */
    return c.result;
}

static int trust_apply(void *user, puddle_stream *st, int exists)
{
    (void)user;
    if (!exists)
        return -1;
    if (!st->tls_fp_seen[0])
        return 1;
    snprintf(st->tls_fp, sizeof st->tls_fp, "%s", st->tls_fp_seen);
    st->tls_fp_seen[0] = '\0';
    return 0;
}

int puddle_streams_trust_fingerprint(puddle_streams *s, const char *id, char *err, size_t errlen)
{
    int rc = puddle_streams_update(s, id, trust_apply, NULL, err, errlen);
    if (rc < 0 && err && !err[0])
        seterr(err, errlen, "no such stream");
    return rc;
}

/* ---- blocks ---- */

int puddle_streams_block(puddle_streams *s, const char *id, int64_t now_ms, char *err, size_t errlen)
{
    if (!puddle_id_valid(id)) {
        seterr(err, errlen, "not a valid user id");
        return -1;
    }
    int rc = 0;
    s->mu.lock();
    if (!is_blocked_locked(s, id)) {
        blocked *n = (blocked *)realloc(s->blocks, (s->nblocks + 1) * sizeof *n);
        if (!n) {
            seterr(err, errlen, "out of memory");
            rc = -1;
            goto out;
        }
        s->blocks = n;
        memcpy(s->blocks[s->nblocks].id, id, PUDDLE_USER_ID_LEN + 1);
        s->blocks[s->nblocks].ts = now_ms;
        s->nblocks++;
        if (save_blocks(s) != 0) {
            s->nblocks--;
            seterr(err, errlen, "cannot save the block list: %s", strerror(errno));
            rc = -1;
            goto out;
        }
    }
    /* the block is on disk; now close whatever stream there was (and do it even if it was blocked before) */
    entry *e; e = find_entry(s, id);
    if (e && e->state != PUDDLE_STREAM_CLOSED) {
        size_t len;
        char *text = read_record(s, id, &len);
        puddle_stream st;
        puddle_chain_head head;
        int has_head;
        if (text && decode(text, len, id, &st, &head, &has_head) == 0) {
            st.state = PUDDLE_STREAM_CLOSED;
            st.owed = PUDDLE_OWED_NONE; /* a block is silent: nothing more is said to them */
            st.updated_ms = now_ms;
            put_locked(s, &st, err, errlen); /* the block stands even if this fails: lookups already refuse them */
            puddle_stream_free(&st);
        }
        free(text);
    }
out:
    s->mu.unlock();
    return rc;
}

int puddle_streams_unblock(puddle_streams *s, const char *id, char *err, size_t errlen)
{
    if (!puddle_id_valid(id)) {
        seterr(err, errlen, "not a valid user id");
        return -1;
    }
    int rc = 1;
    s->mu.lock();
    for (size_t i = 0; i < s->nblocks; i++)
        if (!strcmp(s->blocks[i].id, id)) {
            blocked gone = s->blocks[i];
            memmove(&s->blocks[i], &s->blocks[i + 1], (s->nblocks - i - 1) * sizeof *s->blocks);
            s->nblocks--;
            if (save_blocks(s) != 0) {
                memmove(&s->blocks[i + 1], &s->blocks[i], (s->nblocks - i) * sizeof *s->blocks);
                s->blocks[i] = gone;
                s->nblocks++;
                seterr(err, errlen, "cannot save the block list: %s", strerror(errno));
                rc = -1;
            } else {
                rc = 0;
            }
            break;
        }
    s->mu.unlock();
    return rc;
}

int puddle_streams_is_blocked(puddle_streams *s, const char *id)
{
    if (!puddle_id_valid(id))
        return 0;
    s->mu.lock();
    int b = is_blocked_locked(s, id);
    s->mu.unlock();
    return b;
}

size_t puddle_streams_blocked(puddle_streams *s, puddle_block *out, size_t max)
{
    s->mu.lock();
    size_t n = s->nblocks;
    for (size_t i = 0; i < n && i < max; i++) {
        memcpy(out[i].id, s->blocks[i].id, PUDDLE_USER_ID_LEN + 1);
        out[i].ts_ms = s->blocks[i].ts;
    }
    s->mu.unlock();
    return n;
}

/* ---- who may ask ---- */

puddle_access puddle_streams_access(puddle_streams *s)
{
    s->mu.lock();
    puddle_access a = s->access;
    s->mu.unlock();
    return a;
}

int puddle_streams_set_access(puddle_streams *s, puddle_access a, char *err, size_t errlen)
{
    char text[32];
    int n = snprintf(text, sizeof text, "%s\n", puddle_access_name(a));
    s->mu.lock();
    int rc = write_state_file(s, "access-policy", text, (size_t)n);
    if (rc == 0)
        s->access = a;
    else
        seterr(err, errlen, "cannot save the access setting: %s", strerror(errno));
    s->mu.unlock();
    return rc;
}

/* ---- invites ---- */

static void purge_expired(puddle_streams *s, int64_t now_ms)
{
    size_t k = 0;
    for (size_t i = 0; i < s->ninvites; i++)
        if (s->invites[i].expires_ms > now_ms && s->invites[i].uses > 0)
            s->invites[k++] = s->invites[i];
    s->ninvites = k;
}

int puddle_streams_invite_create(puddle_streams *s, int64_t now_ms, int64_t ttl_ms, int uses, puddle_invite *out,
                                 char *err, size_t errlen)
{
    if (uses < 1 || uses > 1000 || ttl_ms < 1000 || ttl_ms > INVITE_MAX_TTL_MS) {
        seterr(err, errlen, "an invite needs 1 to 1000 uses and a lifetime of up to 30 days");
        return -1;
    }
    unsigned char raw[16];
    puddle_invite inv;
    memset(&inv, 0, sizeof inv);
    if (RAND_bytes(raw, sizeof raw) != 1 || puddle_base32_encode(raw, sizeof raw, inv.token, sizeof inv.token) != PUDDLE_INVITE_TOKEN_LEN) {
        seterr(err, errlen, "cannot make a random token");
        return -1;
    }
    inv.created_ms = now_ms;
    inv.expires_ms = now_ms + ttl_ms;
    inv.uses = uses;

    s->mu.lock();
    purge_expired(s, now_ms);
    int rc = -1;
    if (s->ninvites >= PUDDLE_MAX_INVITES) {
        seterr(err, errlen, "too many live invites (%d); revoke one first", PUDDLE_MAX_INVITES);
    } else {
        s->invites[s->ninvites++] = inv;
        if (save_invites(s) != 0) {
            s->ninvites--;
            seterr(err, errlen, "cannot save the invite: %s", strerror(errno));
        } else {
            *out = inv;
            rc = 0;
        }
    }
    s->mu.unlock();
    return rc;
}

int puddle_streams_invite_consume(puddle_streams *s, const char *token, int64_t now_ms)
{
    if (!puddle_invite_token_valid(token))
        return 0;
    s->mu.lock();
    purge_expired(s, now_ms);
    /* look at every token, not just until one matches, so the time says nothing about which was right */
    size_t hit = (size_t)-1;
    for (size_t i = 0; i < s->ninvites; i++)
        if (CRYPTO_memcmp(s->invites[i].token, token, PUDDLE_INVITE_TOKEN_LEN) == 0)
            hit = i;
    int ok = 0;
    if (hit != (size_t)-1) {
        puddle_invite saved[PUDDLE_MAX_INVITES];
        size_t nsaved = s->ninvites;
        memcpy(saved, s->invites, sizeof saved);
        if (--s->invites[hit].uses <= 0)
            purge_expired(s, now_ms);
        if (save_invites(s) == 0) {
            ok = 1;
        } else { /* could not make the spend durable: don't let it be used */
            memcpy(s->invites, saved, sizeof saved);
            s->ninvites = nsaved;
        }
    }
    s->mu.unlock();
    return ok;
}

size_t puddle_streams_invites(puddle_streams *s, int64_t now_ms, puddle_invite *out, size_t max)
{
    s->mu.lock();
    purge_expired(s, now_ms);
    size_t n = s->ninvites;
    for (size_t i = 0; i < n && i < max; i++)
        out[i] = s->invites[i];
    s->mu.unlock();
    return n;
}

int puddle_streams_invite_revoke(puddle_streams *s, const char *token)
{
    if (!puddle_invite_token_valid(token))
        return 1;
    int rc = 1;
    s->mu.lock();
    for (size_t i = 0; i < s->ninvites; i++)
        if (CRYPTO_memcmp(s->invites[i].token, token, PUDDLE_INVITE_TOKEN_LEN) == 0) {
            puddle_invite saved[PUDDLE_MAX_INVITES];
            size_t nsaved = s->ninvites;
            memcpy(saved, s->invites, sizeof saved);
            memmove(&s->invites[i], &s->invites[i + 1], (s->ninvites - i - 1) * sizeof *s->invites);
            s->ninvites--;
            if (save_invites(s) == 0) {
                rc = 0;
            } else {
                memcpy(s->invites, saved, sizeof saved);
                s->ninvites = nsaved;
                rc = -1;
            }
            break;
        }
    s->mu.unlock();
    return rc;
}
