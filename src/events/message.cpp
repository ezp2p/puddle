#include "events/message.h"

#include "base/encoding.h"
#include "base/hash.h"
#include "base/json.h"
#include "storage/files.h"

#include <mutex>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIG_DOMAIN "puddle-event-v1\n"
#define TS_MIN 946684800000LL   /* 2000-01-01 */
#define TS_MAX 4102444800000LL  /* 2100-01-01 */

int puddle_dm_chat_id(const char *a, const char *b, char out[PUDDLE_USER_ID_LEN + 1])
{
    if (strlen(a) != PUDDLE_USER_ID_LEN || strlen(b) != PUDDLE_USER_ID_LEN)
        return -1;
    const char *lo = strcmp(a, b) <= 0 ? a : b, *hi = lo == a ? b : a;
    char in[2 + 2 * PUDDLE_USER_ID_LEN];
    memcpy(in, "dm", 2);
    memcpy(in + 2, lo, PUDDLE_USER_ID_LEN);
    memcpy(in + 2 + PUDDLE_USER_ID_LEN, hi, PUDDLE_USER_ID_LEN);
    char hex[65];
    unsigned char raw[32];
    puddle_sha256_hex(in, sizeof in, hex);
    if (puddle_hex_decode(hex, raw, 32) != 0)
        return -1;
    return puddle_base32_encode(raw, 32, out, PUDDLE_USER_ID_LEN + 1) == PUDDLE_USER_ID_LEN ? 0 : -1;
}

/* ---- signatures ---- */

static int domain_wrap(const char *domain, const char *data, size_t len, char **out, size_t *outlen)
{
    size_t dl = strlen(domain);
    char *buf = (char *)malloc(dl + len);
    if (!buf)
        return -1;
    memcpy(buf, domain, dl);
    memcpy(buf + dl, data, len);
    *out = buf;
    *outlen = dl + len;
    return 0;
}

int puddle_text_sign(puddle_identity *me, const char *domain, const char *data, size_t len, char out[PUDDLE_SIG_TEXT_MAX])
{
    char *wrapped;
    size_t wl;
    if (domain_wrap(domain, data, len, &wrapped, &wl) != 0)
        return -1;
    unsigned char sig[64], pub[32];
    int rc = puddle_identity_sign(me, wrapped, wl, sig);
    free(wrapped);
    if (rc != 0)
        return -1;
    puddle_identity_public(me, pub, NULL, NULL);
    char kid[65], b64[100];
    puddle_key_id(pub, kid);
    if (!puddle_base64_encode(sig, 64, b64, sizeof b64))
        return -1;
    int n = snprintf(out, PUDDLE_SIG_TEXT_MAX, "%s:%s", kid, b64);
    return n > 0 && n < PUDDLE_SIG_TEXT_MAX ? 0 : -1;
}

int puddle_text_verify(const unsigned char sign_pub[32], const char *domain, const char *data, size_t len, const unsigned char sig[64])
{
    char *wrapped;
    size_t wl;
    if (domain_wrap(domain, data, len, &wrapped, &wl) != 0)
        return 0;
    int ok = puddle_verify_signature(sign_pub, wrapped, wl, sig);
    free(wrapped);
    return ok;
}

int puddle_event_sign(puddle_identity *me, const char *event, size_t len, char out[PUDDLE_SIG_TEXT_MAX])
{
    return puddle_text_sign(me, SIG_DOMAIN, event, len, out);
}

int puddle_event_sig_parse(const char *text, char kid[65], unsigned char sig[64])
{
    const char *colon = text ? strchr(text, ':') : NULL;
    unsigned char raw[32];
    size_t got = 0;
    if (!colon || colon - text != 64)
        return -1;
    memcpy(kid, text, 64);
    kid[64] = '\0';
    if (puddle_hex_decode(kid, raw, 32) != 0)
        return -1;
    return puddle_base64_decode(colon + 1, sig, 64, &got) == 0 && got == 64 ? 0 : -1;
}

int puddle_event_verify(const unsigned char sign_pub[32], const char *event, size_t len, const unsigned char sig[64])
{
    return puddle_text_verify(sign_pub, SIG_DOMAIN, event, len, sig);
}

/* ---- text ---- */

/* characters in valid UTF-8 text with no control characters but \n and \t, or -1 */
static long chars_of(const char *s, size_t len)
{
    const unsigned char *u = (const unsigned char *)s;
    long n = 0;
    size_t i = 0;
    while (i < len) {
        unsigned char c = u[i];
        size_t k;
        if (c < 0x80) {
            if (c < 0x20 && c != '\n' && c != '\t')
                return -1;
            if (c == 0x7f)
                return -1;
            k = 1;
        } else if (c >= 0xC2 && c <= 0xDF && i + 1 < len && (u[i + 1] & 0xC0) == 0x80) {
            k = 2;
        } else if (c >= 0xE0 && c <= 0xEF && i + 2 < len && (u[i + 1] & 0xC0) == 0x80 && (u[i + 2] & 0xC0) == 0x80 &&
                   !(c == 0xE0 && u[i + 1] < 0xA0) && !(c == 0xED && u[i + 1] >= 0xA0)) {
            k = 3;
            if (c == 0xE2 && u[i + 1] == 0x80 && (u[i + 2] == 0xA8 || u[i + 2] == 0xA9))
                return -1; /* line and paragraph separators: they break lines in places a log should not */
        } else if (c >= 0xF0 && c <= 0xF4 && i + 3 < len && (u[i + 1] & 0xC0) == 0x80 && (u[i + 2] & 0xC0) == 0x80 &&
                   (u[i + 3] & 0xC0) == 0x80 && !(c == 0xF0 && u[i + 1] < 0x90) && !(c == 0xF4 && u[i + 1] >= 0x90)) {
            k = 4;
        } else {
            return -1;
        }
        if (c == 0xC2 && u[i + 1] < 0xA0)
            return -1; /* C1 controls */
        i += k;
        n++;
    }
    return n;
}

int puddle_message_body_ok(const char *body, size_t len)
{
    if (len == 0 || len > PUDDLE_MESSAGE_MAX_CHARS * 4u)
        return 0;
    long n = chars_of(body, len);
    return n >= 1 && n <= PUDDLE_MESSAGE_MAX_CHARS;
}

/* ---- time ---- */

int64_t puddle_event_next_ts(int64_t now_ms)
{
    static std::mutex mu;
    static int64_t last;
    std::lock_guard<std::mutex> hold(mu);
    int64_t t = now_ms > last ? now_ms : last + 1;
    last = t;
    return t;
}

/* ---- the message event ---- */

static int is_event_id(const char *s)
{
    unsigned char raw[32];
    return s && strlen(s) == 64 && puddle_hex_decode(s, raw, 32) == 0;
}

size_t puddle_message_build(const char *chat, const char *sender, int64_t ts, const char *body, size_t body_len,
                            const char *ref, char *out, size_t cap)
{
    return puddle_message_build_files(chat, sender, ts, body, body_len, ref, NULL, 0, out, cap);
}

size_t puddle_message_build_files(const char *chat, const char *sender, int64_t ts, const char *body, size_t body_len, const char *ref,
                                  const puddle_file_ref *files, size_t nfiles, char *out, size_t cap)
{
    if ((nfiles == 0 || body_len > 0) && !puddle_message_body_ok(body, body_len))
        return 0; /* only a message of files may go without words */
    if ((ref && *ref && !is_event_id(ref)) || nfiles > PUDDLE_MESSAGE_MAX_FILES)
        return 0;
    for (size_t i = 0; i < nfiles; i++)
        if (!puddle_file_ref_ok(&files[i]))
            return 0;
    puddle_jw w;
    puddle_jw_init(&w, out, cap);
    puddle_jw_object(&w);
    puddle_jw_str(&w, "type", "message");
    puddle_jw_str(&w, "chat", chat);
    puddle_jw_str(&w, "sender", sender);
    puddle_jw_num(&w, "ts", ts);
    if (body_len > 0) {
        puddle_jw_key(&w, "body");
        puddle_jw_string_n(&w, body, body_len);
    }
    if (ref && *ref)
        puddle_jw_str(&w, "ref", ref);
    if (nfiles) {
        puddle_jw_key(&w, "files");
        puddle_file_refs_write(&w, files, nfiles);
    }
    puddle_jw_end_object(&w);
    return puddle_jw_finish(&w);
}

void puddle_message_free(puddle_message *m)
{
    free(m->body);
    m->body = NULL;
    free(m->files);
    m->files = NULL;
}

/* ---- files ---- */

static int file_name_ok(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > PUDDLE_FILE_NAME_MAX || !strcmp(s, ".") || !strcmp(s, "..") || !puddle_message_body_ok(s, n))
        return 0;
    return strpbrk(s, "/\\\n\t") == NULL; /* a name, not a path; one line */
}

static int mime_ok(const char *s)
{
    size_t n = strlen(s), slash = 0;
    if (n < 3 || n > PUDDLE_FILE_MIME_MAX)
        return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '/')
            slash++;
        else if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("!#$&^_.+-", c)))
            return 0;
    }
    return slash == 1 && s[0] != '/' && s[n - 1] != '/';
}

int puddle_file_ref_ok(const puddle_file_ref *f)
{
    if (!puddle_file_id_valid(f->id) || !file_name_ok(f->name) || !mime_ok(f->mime) || f->size < 1 || f->size > PUDDLE_FILE_HARD_MAX)
        return 0;
    if (f->w < 0 || f->w > 100000 || f->h < 0 || f->h > 100000 || (f->w == 0) != (f->h == 0))
        return 0;
    if (f->duration_ms < 0 || f->duration_ms > 1000LL * 3600 * 24 * 7)
        return 0;
    if (f->preview[0]) {
        unsigned char img[PUDDLE_FILE_PREVIEW_MAX];
        size_t got;
        if (strlen(f->preview) > 4 * ((PUDDLE_FILE_PREVIEW_MAX + 2) / 3) || puddle_base64_decode(f->preview, img, sizeof img, &got) != 0 || got == 0)
            return 0;
    }
    return 1;
}

static int ref_int(const char *e, size_t n, const char *key, int64_t lo, int64_t hi, int64_t *out)
{
    puddle_span v;
    int rc = puddle_json_member(e, n, key, &v);
    if (rc == 1) {
        *out = 0;
        return 0;
    }
    return rc == 0 && puddle_json_int64(v, out) == 0 && *out >= lo && *out <= hi ? 0 : -1;
}

int puddle_file_refs_parse(const char *json, size_t len, puddle_file_ref **out, size_t *n)
{
    *out = NULL;
    *n = 0;
    puddle_span arr = { json, len }, item;
    size_t cur = 0, count = 0;
    puddle_file_ref *files = (puddle_file_ref *)calloc(PUDDLE_MESSAGE_MAX_FILES, sizeof *files);
    if (!files)
        return -1;
    int rc;
    while ((rc = puddle_json_array_next(arr, &cur, &item)) == 1) {
        if (count == PUDDLE_MESSAGE_MAX_FILES || !puddle_json_valid_object(item.p, item.len))
            goto bad;
        puddle_file_ref *f = &files[count];
        puddle_span v;
        int64_t w, h;
        if (puddle_json_member(item.p, item.len, "id", &v) != 0 || puddle_json_string(v, f->id, sizeof f->id) != 0 ||
            puddle_json_member(item.p, item.len, "name", &v) != 0 || puddle_json_string(v, f->name, sizeof f->name) != 0 ||
            puddle_json_member(item.p, item.len, "mime", &v) != 0 || puddle_json_string(v, f->mime, sizeof f->mime) != 0 ||
            puddle_json_member(item.p, item.len, "size", &v) != 0 || puddle_json_int64(v, &f->size) != 0 ||
            ref_int(item.p, item.len, "w", 1, 100000, &w) != 0 || ref_int(item.p, item.len, "h", 1, 100000, &h) != 0 ||
            ref_int(item.p, item.len, "duration_ms", 1, INT64_MAX, &f->duration_ms) != 0)
            goto bad;
        f->w = (int)w;
        f->h = (int)h;
        int p = puddle_json_member(item.p, item.len, "preview", &v);
        if (p < 0 || (p == 0 && puddle_json_string(v, f->preview, sizeof f->preview) != 0))
            goto bad;
        if (!puddle_file_ref_ok(f))
            goto bad;
        count++;
    }
    if (rc < 0 || count == 0)
        goto bad;
    *out = files;
    *n = count;
    return 0;
bad:
    free(files);
    return -1;
}

void puddle_file_refs_write(puddle_jw *w, const puddle_file_ref *files, size_t n)
{
    puddle_jw_array(w);
    for (size_t i = 0; i < n; i++) {
        const puddle_file_ref *f = &files[i];
        puddle_jw_object(w);
        puddle_jw_str(w, "id", f->id);
        puddle_jw_str(w, "name", f->name);
        puddle_jw_str(w, "mime", f->mime);
        puddle_jw_num(w, "size", f->size);
        if (f->w) {
            puddle_jw_num(w, "w", f->w);
            puddle_jw_num(w, "h", f->h);
        }
        if (f->duration_ms)
            puddle_jw_num(w, "duration_ms", f->duration_ms);
        if (f->preview[0])
            puddle_jw_str(w, "preview", f->preview);
        puddle_jw_end_object(w);
    }
    puddle_jw_end_array(w);
}

static int member_str(const char *e, size_t n, const char *key, char *out, size_t cap)
{
    puddle_span v;
    return puddle_json_member(e, n, key, &v) == 0 && puddle_json_string(v, out, cap) == 0 ? 0 : -1;
}

int puddle_chat_event_parse(const char *event, size_t len, puddle_message *m, const char **why)
{
    memset(m, 0, sizeof *m);
    *why = "malformed";
    if (len == 0 || len > PUDDLE_EVENT_MAX_BYTES || !puddle_json_valid_object(event, len))
        return -1;
    char type[16];
    if (member_str(event, len, "type", type, sizeof type) != 0)
        return -1;
    if (!strcmp(type, "message"))
        m->type = PUDDLE_EV_MESSAGE;
    else if (!strcmp(type, "edit"))
        m->type = PUDDLE_EV_EDIT;
    else if (!strcmp(type, "redact"))
        m->type = PUDDLE_EV_REDACT;
    else {
        *why = "unsupported_type";
        return -1;
    }
    if (member_str(event, len, "chat", m->chat, sizeof m->chat) != 0 || member_str(event, len, "sender", m->sender, sizeof m->sender) != 0 ||
        strlen(m->chat) != PUDDLE_USER_ID_LEN || strlen(m->sender) != PUDDLE_USER_ID_LEN)
        return -1;
    puddle_span v;
    if (puddle_json_member(event, len, "ts", &v) != 0 || puddle_json_int64(v, &m->ts) != 0)
        return -1;
    if (m->ts < TS_MIN || m->ts >= TS_MAX) {
        *why = "bad_time";
        return -1;
    }
    int has_body = puddle_json_member(event, len, "body", &v);
    if (has_body < 0)
        return -1;
    puddle_span fv;
    int has_files = puddle_json_member(event, len, "files", &fv);
    if (has_files < 0 || (has_files == 0 && m->type != PUDDLE_EV_MESSAGE)) /* an edit changes the words, never the files */
        return -1;
    if (has_files == 0) {
        puddle_file_ref *refs;
        size_t nrefs;
        if (puddle_file_refs_parse(fv.p, fv.len, &refs, &nrefs) != 0) {
            *why = "bad_files";
            return -1;
        }
        free(refs);
        m->files = (char *)malloc(fv.len + 1);
        if (!m->files)
            return -1;
        memcpy(m->files, fv.p, fv.len);
        m->files[fv.len] = '\0';
    }
    if (m->type == PUDDLE_EV_REDACT) {
        if (has_body == 0) /* a redaction carries nothing: no text to smuggle in under cover of "deleting" */
            return -1;
    } else if (has_body == 1 && m->files) { /* files alone, with no words */
        m->body = (char *)calloc(1, 1);
        if (!m->body)
            goto bad_body;
    } else {
        if (has_body != 0 || v.len < 2 || v.p[0] != '"')
            goto bad_body;
        m->body = (char *)malloc(v.len + 1);
        if (!m->body || puddle_json_string(v, m->body, v.len + 1) != 0 || !puddle_message_body_ok(m->body, strlen(m->body)))
            goto bad_body;
    }
    int rc; rc = puddle_json_member(event, len, "ref", &v);
    if (rc < 0 || (rc == 1 && m->type != PUDDLE_EV_MESSAGE)) { /* an edit or a redaction has to say what it is about */
        puddle_message_free(m);
        return -1;
    }
    if (rc == 0 && (puddle_json_string(v, m->ref, sizeof m->ref) != 0 || !is_event_id(m->ref))) {
        puddle_message_free(m);
        return -1;
    }
    *why = NULL;
    return 0;
bad_body:
    puddle_message_free(m);
    *why = "bad_body";
    return -1;
}

int puddle_message_parse(const char *event, size_t len, puddle_message *m, const char **why)
{
    char type[16];
    if (len > 0 && len <= PUDDLE_EVENT_MAX_BYTES && puddle_json_valid_object(event, len) && member_str(event, len, "type", type, sizeof type) == 0 &&
        strcmp(type, "message") != 0) {
        memset(m, 0, sizeof *m);
        *why = "unsupported_type"; /* edits and redactions are events too, but not messages */
        return -1;
    }
    return puddle_chat_event_parse(event, len, m, why);
}

size_t puddle_edit_build(const char *chat, const char *sender, int64_t ts, const char *target, const char *body, size_t body_len,
                         char *out, size_t cap)
{
    if (!puddle_message_body_ok(body, body_len) || !is_event_id(target))
        return 0;
    puddle_jw w;
    puddle_jw_init(&w, out, cap);
    puddle_jw_object(&w);
    puddle_jw_str(&w, "type", "edit");
    puddle_jw_str(&w, "chat", chat);
    puddle_jw_str(&w, "sender", sender);
    puddle_jw_num(&w, "ts", ts);
    puddle_jw_key(&w, "body");
    puddle_jw_string_n(&w, body, body_len);
    puddle_jw_str(&w, "ref", target);
    puddle_jw_end_object(&w);
    return puddle_jw_finish(&w);
}

size_t puddle_redact_build(const char *chat, const char *sender, int64_t ts, const char *target, char *out, size_t cap)
{
    if (!is_event_id(target))
        return 0;
    puddle_jw w;
    puddle_jw_init(&w, out, cap);
    puddle_jw_object(&w);
    puddle_jw_str(&w, "type", "redact");
    puddle_jw_str(&w, "chat", chat);
    puddle_jw_str(&w, "sender", sender);
    puddle_jw_num(&w, "ts", ts);
    puddle_jw_str(&w, "ref", target);
    puddle_jw_end_object(&w);
    return puddle_jw_finish(&w);
}

/* ---- the view ---- */

typedef struct {
    char id[65];
    size_t index;
} idx_entry;

static int cmp_idx(const void *a, const void *b) { return strcmp(((const idx_entry *)a)->id, ((const idx_entry *)b)->id); }

void puddle_view_clear(puddle_view_msg *v)
{
    free(v->body);
    v->body = NULL;
    free(v->files);
    v->files = NULL;
}

void puddle_view_free(puddle_view_msg *v, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        free(v[i].body);
        free(v[i].files);
    }
    free(v);
}

int puddle_chat_view(const puddle_event *events, size_t n, puddle_view_msg **out, size_t *count)
{
    *out = NULL;
    *count = 0;
    puddle_view_msg *v = (puddle_view_msg *)calloc(n ? n : 1, sizeof *v);
    idx_entry *idx = (idx_entry *)calloc(n ? n : 1, sizeof *idx);
    if (!v || !idx) {
        free(v);
        free(idx);
        return -1;
    }
    size_t m = 0;
    for (size_t i = 0; i < n; i++) { /* the messages first */
        puddle_message ev;
        const char *why;
        if (puddle_chat_event_parse(events[i].event, events[i].event_len, &ev, &why) != 0)
            continue;
        if (ev.type == PUDDLE_EV_MESSAGE) {
            snprintf(v[m].id, sizeof v[m].id, "%s", events[i].id);
            snprintf(v[m].sender, sizeof v[m].sender, "%s", ev.sender);
            snprintf(v[m].ref, sizeof v[m].ref, "%s", ev.ref);
            v[m].ts = ev.ts;
            v[m].rts = events[i].rts;
            v[m].body = ev.body;
            ev.body = NULL;
            v[m].files = ev.files;
            ev.files = NULL;
            snprintf(idx[m].id, sizeof idx[m].id, "%s", events[i].id);
            idx[m].index = m;
            m++;
        }
        puddle_message_free(&ev);
    }
    qsort(idx, m, sizeof *idx, cmp_idx);
    for (size_t i = 0; i < n; i++) { /* then what was done to them, in chat order, so the last edit is the one that stands */
        puddle_message ev;
        const char *why;
        if (puddle_chat_event_parse(events[i].event, events[i].event_len, &ev, &why) != 0)
            continue;
        if (ev.type != PUDDLE_EV_MESSAGE) {
            idx_entry key;
            snprintf(key.id, sizeof key.id, "%s", ev.ref);
            idx_entry *hit = (idx_entry *)bsearch(&key, idx, m, sizeof *idx, cmp_idx);
            if (hit) {
                puddle_view_msg *t = &v[hit->index];
                if (!strcmp(t->sender, ev.sender) && ev.ts > t->ts) {
                    if (ev.type == PUDDLE_EV_REDACT) {
                        t->deleted = 1;
                        free(t->body);
                        t->body = NULL;
                        free(t->files);
                        t->files = NULL;
                    } else if (!t->deleted) {
                        free(t->body);
                        t->body = ev.body;
                        ev.body = NULL;
                        t->edited = 1;
                        t->edited_ts = ev.ts;
                    }
                }
            }
        }
        puddle_message_free(&ev);
    }
    free(idx);
    *out = v;
    *count = m;
    return 0;
}
