#include "base/json.h"

#include <stdio.h>
#include <string.h>

#define MAX_DEPTH 64

static int is_digit(char c) { return c >= '0' && c <= '9'; }

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
        p++;
    return p;
}

/* p at the opening quote; returns just past the closing quote, or NULL */
static const char *scan_string(const char *p, const char *end)
{
    if (p >= end || *p != '"')
        return NULL;
    p++;
    while (p < end) {
        unsigned char c = (unsigned char)*p;
        if (c == '"')
            return p + 1;
        if (c < 0x20)
            return NULL;
        if (c == '\\') {
            p++;
            if (p >= end)
                return NULL;
            switch (*p) {
            case '"': case '\\': case '/': case 'b': case 'f': case 'n': case 'r': case 't':
                p++;
                break;
            case 'u':
                if (end - p < 5)
                    return NULL;
                for (int i = 1; i <= 4; i++)
                    if (hexval(p[i]) < 0)
                        return NULL;
                p += 5;
                break;
            default:
                return NULL;
            }
        } else {
            p++;
        }
    }
    return NULL;
}

static const char *scan_number(const char *p, const char *end)
{
    if (p < end && *p == '-')
        p++;
    if (p >= end)
        return NULL;
    if (*p == '0') {
        p++;
    } else if (*p >= '1' && *p <= '9') {
        while (p < end && is_digit(*p)) p++;
    } else {
        return NULL;
    }
    if (p < end && *p == '.') {
        p++;
        if (p >= end || !is_digit(*p))
            return NULL;
        while (p < end && is_digit(*p)) p++;
    }
    if (p < end && (*p == 'e' || *p == 'E')) {
        p++;
        if (p < end && (*p == '+' || *p == '-'))
            p++;
        if (p >= end || !is_digit(*p))
            return NULL;
        while (p < end && is_digit(*p)) p++;
    }
    return p;
}

static const char *scan_literal(const char *p, const char *end, const char *lit)
{
    size_t n = strlen(lit);
    if ((size_t)(end - p) < n || memcmp(p, lit, n) != 0)
        return NULL;
    return p + n;
}

static const char *scan_value(const char *p, const char *end, int depth)
{
    if (depth > MAX_DEPTH)
        return NULL;
    p = skip_ws(p, end);
    if (p >= end)
        return NULL;

    switch (*p) {
    case '{':
        p = skip_ws(p + 1, end);
        if (p < end && *p == '}')
            return p + 1;
        for (;;) {
            p = scan_string(skip_ws(p, end), end);
            if (!p) return NULL;
            p = skip_ws(p, end);
            if (p >= end || *p != ':') return NULL;
            p = scan_value(p + 1, end, depth + 1);
            if (!p) return NULL;
            p = skip_ws(p, end);
            if (p >= end) return NULL;
            if (*p == ',') { p++; continue; }
            if (*p == '}') return p + 1;
            return NULL;
        }
    case '[':
        p = skip_ws(p + 1, end);
        if (p < end && *p == ']')
            return p + 1;
        for (;;) {
            p = scan_value(p, end, depth + 1);
            if (!p) return NULL;
            p = skip_ws(p, end);
            if (p >= end) return NULL;
            if (*p == ',') { p++; continue; }
            if (*p == ']') return p + 1;
            return NULL;
        }
    case '"': return scan_string(p, end);
    case 't': return scan_literal(p, end, "true");
    case 'f': return scan_literal(p, end, "false");
    case 'n': return scan_literal(p, end, "null");
    default:  return scan_number(p, end);
    }
}

int puddle_json_array_next(puddle_span array, size_t *cursor, puddle_span *item)
{
    const char *end = array.p + array.len;
    const char *p;
    if (*cursor == 0) {
        p = skip_ws(array.p, end);
        if (p >= end || *p != '[')
            return -1;
        p = skip_ws(p + 1, end);
        if (p < end && *p == ']') {
            *cursor = (size_t)-1;
            return 0;
        }
    } else {
        if (*cursor == (size_t)-1)
            return 0; /* already ended */
        if (*cursor >= array.len)
            return -1; /* ran out before the closing bracket */
        p = skip_ws(array.p + *cursor, end);
        if (p >= end)
            return -1;
        if (*p == ']') {
            *cursor = (size_t)-1;
            return 0;
        }
        if (*p != ',')
            return -1;
        p = skip_ws(p + 1, end);
    }
    const char *e = scan_value(p, end, 1);
    if (!e)
        return -1;
    item->p = p;
    item->len = (size_t)(e - p);
    *cursor = (size_t)(e - array.p);
    return 1;
}

int puddle_json_valid_object(const char *json, size_t len)
{
    const char *end = json + len;
    const char *p = skip_ws(json, end);
    if (p >= end || *p != '{')
        return 0;
    p = scan_value(p, end, 0);
    return p && skip_ws(p, end) == end;
}

int puddle_json_string(puddle_span v, char *out, size_t cap)
{
    if (v.len < 2 || v.p[0] != '"' || v.p[v.len - 1] != '"' || cap == 0)
        return -1;

    const char *p = v.p + 1, *end = v.p + v.len - 1;
    size_t o = 0;
#define PUT(ch) do { if (o + 1 >= cap) return -1; out[o++] = (char)(ch); } while (0)

    while (p < end) {
        if (*p != '\\') {
            PUT(*p++);
            continue;
        }
        p++;
        switch (*p) {
        case '"': PUT('"'); p++; break;
        case '\\': PUT('\\'); p++; break;
        case '/': PUT('/'); p++; break;
        case 'b': PUT('\b'); p++; break;
        case 'f': PUT('\f'); p++; break;
        case 'n': PUT('\n'); p++; break;
        case 'r': PUT('\r'); p++; break;
        case 't': PUT('\t'); p++; break;
        case 'u': {
            if (end - p < 5) return -1;
            unsigned cp = 0;
            for (int i = 1; i <= 4; i++) {
                int h = hexval(p[i]);
                if (h < 0) return -1;
                cp = cp << 4 | (unsigned)h;
            }
            p += 5;
            if (cp >= 0xD800 && cp <= 0xDBFF) { /* needs a following low surrogate */
                if (end - p < 6 || p[0] != '\\' || p[1] != 'u') return -1;
                unsigned lo = 0;
                for (int i = 2; i <= 5; i++) {
                    int h = hexval(p[i]);
                    if (h < 0) return -1;
                    lo = lo << 4 | (unsigned)h;
                }
                if (lo < 0xDC00 || lo > 0xDFFF) return -1;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                p += 6;
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                return -1;
            }
            if (cp == 0)
                return -1; /* would truncate the C string */
            if (cp < 0x80) {
                PUT(cp);
            } else if (cp < 0x800) {
                PUT(0xC0 | (cp >> 6)); PUT(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                PUT(0xE0 | (cp >> 12)); PUT(0x80 | ((cp >> 6) & 0x3F)); PUT(0x80 | (cp & 0x3F));
            } else {
                PUT(0xF0 | (cp >> 18)); PUT(0x80 | ((cp >> 12) & 0x3F));
                PUT(0x80 | ((cp >> 6) & 0x3F)); PUT(0x80 | (cp & 0x3F));
            }
            break;
        }
        default:
            return -1;
        }
    }
#undef PUT
    out[o] = '\0';
    return 0;
}

int puddle_json_int64(puddle_span v, int64_t *out)
{
    size_t i = 0;
    int neg = 0;
    if (v.len == 0)
        return -1;
    if (v.p[0] == '-') {
        neg = 1;
        i++;
    }
    if (i >= v.len)
        return -1;
    if (v.p[i] == '0' && v.len - i > 1)
        return -1;
    int64_t acc = 0;
    for (; i < v.len; i++) {
        if (!is_digit(v.p[i]))
            return -1;
        int d = v.p[i] - '0';
        if (acc > (INT64_MAX - d) / 10)
            return -1;
        acc = acc * 10 + d;
    }
    *out = neg ? -acc : acc;
    return 0;
}

static int key_equals(const char *ks, const char *ke, const char *key)
{
    char buf[128];
    puddle_span s = { ks, (size_t)(ke - ks) };
    return puddle_json_string(s, buf, sizeof buf) == 0 && strcmp(buf, key) == 0;
}

int puddle_json_member(const char *json, size_t len, const char *key, puddle_span *val)
{
    const char *end = json + len;
    const char *p = skip_ws(json, end);
    int found = 0;
    puddle_span result = { NULL, 0 };

    if (p >= end || *p != '{')
        return -1;
    p = skip_ws(p + 1, end);
    if (p < end && *p == '}') {
        p++;
    } else {
        for (;;) {
            const char *ks = skip_ws(p, end);
            const char *ke = scan_string(ks, end);
            if (!ke) return -1;
            p = skip_ws(ke, end);
            if (p >= end || *p != ':') return -1;
            const char *vs = skip_ws(p + 1, end);
            const char *ve = scan_value(vs, end, 1);
            if (!ve) return -1;
            if (key_equals(ks, ke, key)) {
                if (found) return -1; /* duplicate key */
                found = 1;
                result.p = vs;
                result.len = (size_t)(ve - vs);
            }
            p = skip_ws(ve, end);
            if (p >= end) return -1;
            if (*p == ',') { p++; continue; }
            if (*p == '}') { p++; break; }
            return -1;
        }
    }
    if (skip_ws(p, end) != end)
        return -1;
    if (!found)
        return 1;
    *val = result;
    return 0;
}

/* ---- writer ---- */

void puddle_jw_init(puddle_jw *w, char *buf, size_t cap)
{
    memset(w, 0, sizeof *w);
    w->buf = buf;
    w->cap = cap;
    if (cap == 0)
        w->err = 1;
}

static void put(puddle_jw *w, const char *s, size_t n)
{
    if (w->err)
        return;
    if (n >= w->cap - w->len) { /* keeps one byte for the NUL */
        w->err = 1;
        return;
    }
    memcpy(w->buf + w->len, s, n);
    w->len += n;
}

static void putc_(puddle_jw *w, char c) { put(w, &c, 1); }

/* what comes before a value: nothing, or a comma, and the check that a value is allowed here */
static void before_value(puddle_jw *w)
{
    if (w->err)
        return;
    if (w->depth == 0) {
        if (w->done)
            w->err = 1;
        return;
    }
    int top = w->depth - 1;
    if (w->kind[top] == 'o') {
        if (!w->key_pending) {
            w->err = 1;
            return;
        }
        w->key_pending = 0; /* the comma came with the key */
    } else {
        if (w->count[top])
            putc_(w, ',');
        w->count[top] = 1;
    }
}

static void after_value(puddle_jw *w)
{
    if (w->depth == 0)
        w->done = 1;
}

static void open_(puddle_jw *w, char kind)
{
    before_value(w);
    if (w->err)
        return;
    if (w->depth >= 64) {
        w->err = 1;
        return;
    }
    putc_(w, kind == 'o' ? '{' : '[');
    w->kind[w->depth] = kind;
    w->count[w->depth] = 0;
    w->depth++;
}

static void close_(puddle_jw *w, char kind)
{
    if (w->err)
        return;
    if (w->depth == 0 || w->kind[w->depth - 1] != kind || w->key_pending) {
        w->err = 1;
        return;
    }
    putc_(w, kind == 'o' ? '}' : ']');
    w->depth--;
    after_value(w);
}

void puddle_jw_object(puddle_jw *w) { open_(w, 'o'); }
void puddle_jw_array(puddle_jw *w) { open_(w, 'a'); }
void puddle_jw_end_object(puddle_jw *w) { close_(w, 'o'); }
void puddle_jw_end_array(puddle_jw *w) { close_(w, 'a'); }

/* Length of the valid UTF-8 sequence at s (n bytes left), or 0 if it is not valid: no overlong forms, no
 * surrogates, nothing above U+10FFFF. */
static size_t utf8_len(const unsigned char *s, size_t n)
{
    if (s[0] < 0x80)
        return 1;
    if (s[0] >= 0xC2 && s[0] <= 0xDF)
        return n >= 2 && (s[1] & 0xC0) == 0x80 ? 2 : 0;
    if (s[0] >= 0xE0 && s[0] <= 0xEF) {
        if (n < 3 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80)
            return 0;
        if (s[0] == 0xE0 && s[1] < 0xA0)
            return 0; /* overlong */
        if (s[0] == 0xED && s[1] >= 0xA0)
            return 0; /* surrogate */
        return 3;
    }
    if (s[0] >= 0xF0 && s[0] <= 0xF4) {
        if (n < 4 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 || (s[3] & 0xC0) != 0x80)
            return 0;
        if (s[0] == 0xF0 && s[1] < 0x90)
            return 0; /* overlong */
        if (s[0] == 0xF4 && s[1] >= 0x90)
            return 0; /* above U+10FFFF */
        return 4;
    }
    return 0;
}

static void quoted(puddle_jw *w, const char *s, size_t n)
{
    putc_(w, '"');
    const unsigned char *u = (const unsigned char *)s;
    size_t i = 0;
    while (i < n && !w->err) {
        unsigned char c = u[i];
        if (c == '"' || c == '\\') {
            char e[2] = {'\\', (char)c};
            put(w, e, 2);
            i++;
        } else if (c < 0x20) {
            char e[8];
            switch (c) {
            case '\n': strcpy(e, "\\n"); break;
            case '\r': strcpy(e, "\\r"); break;
            case '\t': strcpy(e, "\\t"); break;
            case '\b': strcpy(e, "\\b"); break;
            case '\f': strcpy(e, "\\f"); break;
            default: snprintf(e, sizeof e, "\\u%04x", c); break;
            }
            put(w, e, strlen(e));
            i++;
        } else if (c < 0x80) {
            putc_(w, (char)c);
            i++;
        } else {
            size_t k = utf8_len(u + i, n - i);
            if (k) {
                put(w, s + i, k);
                i += k;
            } else {
                put(w, "\xEF\xBF\xBD", 3); /* U+FFFD, in place of the one bad byte */
                i++;
            }
        }
    }
    putc_(w, '"');
}

void puddle_jw_key(puddle_jw *w, const char *key)
{
    if (w->err)
        return;
    if (w->depth == 0 || w->kind[w->depth - 1] != 'o' || w->key_pending) {
        w->err = 1;
        return;
    }
    int top = w->depth - 1;
    if (w->count[top])
        putc_(w, ',');
    w->count[top] = 1;
    quoted(w, key, strlen(key));
    putc_(w, ':');
    w->key_pending = 1;
}

void puddle_jw_string_n(puddle_jw *w, const char *s, size_t n)
{
    before_value(w);
    quoted(w, s, n);
    after_value(w);
}

void puddle_jw_string(puddle_jw *w, const char *s)
{
    if (!s) {
        puddle_jw_null(w);
        return;
    }
    puddle_jw_string_n(w, s, strlen(s));
}

void puddle_jw_int(puddle_jw *w, int64_t v)
{
    char t[32];
    snprintf(t, sizeof t, "%lld", (long long)v);
    before_value(w);
    put(w, t, strlen(t));
    after_value(w);
}

void puddle_jw_uint(puddle_jw *w, uint64_t v)
{
    char t[32];
    snprintf(t, sizeof t, "%llu", (unsigned long long)v);
    before_value(w);
    put(w, t, strlen(t));
    after_value(w);
}

void puddle_jw_bool(puddle_jw *w, int v)
{
    before_value(w);
    put(w, v ? "true" : "false", v ? 4 : 5);
    after_value(w);
}

void puddle_jw_null(puddle_jw *w)
{
    before_value(w);
    put(w, "null", 4);
    after_value(w);
}

void puddle_jw_raw(puddle_jw *w, const char *json, size_t n)
{
    if (n == 0) {
        w->err = 1;
        return;
    }
    before_value(w);
    put(w, json, n);
    after_value(w);
}

void puddle_jw_str(puddle_jw *w, const char *key, const char *s) { puddle_jw_key(w, key); puddle_jw_string(w, s); }
void puddle_jw_num(puddle_jw *w, const char *key, int64_t v) { puddle_jw_key(w, key); puddle_jw_int(w, v); }
void puddle_jw_flag(puddle_jw *w, const char *key, int v) { puddle_jw_key(w, key); puddle_jw_bool(w, v); }

size_t puddle_jw_finish(puddle_jw *w)
{
    if (w->err || w->depth != 0 || !w->done)
        return 0;
    w->buf[w->len] = '\0';
    return w->len;
}
