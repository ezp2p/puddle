#include "wire/wireprofile.h"

#include "base/text.h"
#include "base/encoding.h"
#include "base/json.h"
#include "core/profile.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const MIMES[] = { "image/png", "image/jpeg", "image/gif", "image/webp" };

static int mime_ok(const char *m)
{
    for (size_t i = 0; i < sizeof MIMES / sizeof *MIMES; i++)
        if (!strcmp(m, MIMES[i]))
            return 1;
    return 0;
}

static int hash_ok(const char *h)
{
    unsigned char raw[32];
    if (strlen(h) != 64)
        return 0;
    for (int i = 0; i < 64; i++)
        if (!((h[i] >= '0' && h[i] <= '9') || (h[i] >= 'a' && h[i] <= 'f')))
            return 0; /* lower case only: one spelling of a hash */
    return puddle_hex_decode(h, raw, 32) == 0;
}

/* {"hash","mime","size"} -> a reference, 0 or -1 */
static int media_of(puddle_span obj, puddle_media_ref *r)
{
    puddle_span v;
    int64_t size;
    if (obj.len < 2 || obj.p[0] != '{')
        return -1;
    if (puddle_json_member(obj.p, obj.len, "hash", &v) != 0 || puddle_json_string(v, r->hash, sizeof r->hash) != 0 || !hash_ok(r->hash))
        return -1;
    if (puddle_json_member(obj.p, obj.len, "mime", &v) != 0 || puddle_json_string(v, r->mime, sizeof r->mime) != 0 || !mime_ok(r->mime))
        return -1;
    if (puddle_json_member(obj.p, obj.len, "size", &v) != 0 || puddle_json_int64(v, &size) != 0 || size < 1 || (uint64_t)size > PUDDLE_IMAGE_MAX_BYTES)
        return -1;
    r->size = (uint64_t)size;
    return 0;
}

static int hex_color_ok(const char *c)
{
    if (strlen(c) != 7 || c[0] != '#')
        return 0;
    for (int i = 1; i < 7; i++)
        if (!isxdigit((unsigned char)c[i]))
            return 0;
    return 1;
}

/* ---- notes ---- */

typedef struct {
    char *buf;
    size_t cap, len;
} notes_t;

static void note(notes_t *n, const char *fmt, ...)
{
    if (!n->buf || n->cap == 0 || n->len + 1 >= n->cap)
        return;
    char line[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    int w = snprintf(n->buf + n->len, n->cap - n->len, "%s%s", n->len ? "\n" : "", line);
    if (w < 0 || (size_t)w >= n->cap - n->len) {
        n->buf[n->len] = '\0'; /* that line does not fit: leave the notes as they were */
        n->cap = n->len + 1;   /* and take no more */
        return;
    }
    n->len += (size_t)w;
}

/* A little of what they sent, safe to show: cleaned, on one line, at most 24 characters. */
static void excerpt(puddle_span v, char out[100])
{
    char raw[100];
    size_t n = v.len < sizeof raw - 1 ? v.len : sizeof raw - 1;
    memcpy(raw, v.p, n);
    raw[n] = '\0';
    size_t chars;
    if (puddle_clean_text(raw, n, 24, 0, out, 100, &chars) != 0) {
        if (puddle_clean_text(raw, n, SIZE_MAX, 0, out, 100, &chars) != 0)
            snprintf(out, 100, "?");
        else
            puddle_text_cut(out, 24);
    }
}

/* ---- text ---- */

/* Clean the string `v` into `out` (malloc'd), cut to `max` characters if it is longer. 0 kept (*out may be "" if nothing
 * is left), 1 cut, -1 not a string. */
static int text_of(puddle_span v, size_t max, int multiline, char **out)
{
    *out = NULL;
    if (v.len < 2 || v.p[0] != '"')
        return -1;
    char *raw = (char *)malloc(v.len + 1);
    size_t cap = v.len * 3 + 16; /* every bad byte may become a three-byte U+FFFD */
    char *clean = (char *)malloc(cap);
    size_t chars = 0;
    if (!raw || !clean || puddle_json_string(v, raw, v.len + 1) != 0 ||
        puddle_clean_text(raw, strlen(raw), SIZE_MAX, multiline, clean, cap, &chars) != 0) {
        free(raw);
        free(clean);
        return -1;
    }
    free(raw);
    int cut = 0;
    if (chars > max) {
        puddle_text_cut(clean, max);
        cut = 1;
    }
    *out = clean;
    return cut;
}

/* ---- links ---- */

static void clean_links(puddle_span arr, puddle_jw *w, notes_t *notes)
{
    size_t cur = 0;
    puddle_span it;
    int r, n = 0, kept = 0, dropped = 0;
    if (arr.len < 2 || arr.p[0] != '[') {
        note(notes, "links: not a list; left out");
        return;
    }
    puddle_jw_key(w, "links");
    puddle_jw_array(w);
    while ((r = puddle_json_array_next(arr, &cur, &it)) == 1) {
        n++;
        if (kept >= PUDDLE_MAX_PROFILE_LINKS) {
            dropped++;
            continue;
        }
        puddle_span tv, uv;
        char *text = NULL;
        int trc = it.len >= 2 && it.p[0] == '{' ? puddle_json_member(it.p, it.len, "text", &tv) : -1;
        int cut = trc == 0 ? text_of(tv, PUDDLE_PROFILE_LINK_TEXT_MAX, 0, &text) : -1;
        if (cut < 0 || !*text) {
            note(notes, "links: link %d has no text; left out", n);
            free(text);
            continue;
        }
        if (cut)
            note(notes, "links: the text of link %d was longer than %d characters; cut short", n, PUDDLE_PROFILE_LINK_TEXT_MAX);
        puddle_jw_object(w);
        puddle_jw_str(w, "text", text);
        free(text);
        int urc = puddle_json_member(it.p, it.len, "url", &uv);
        if (urc == 0 && !(uv.len == 4 && !memcmp(uv.p, "null", 4))) {
            char url[PUDDLE_PROFILE_LINK_URL_MAX + 1];
            if (puddle_json_string(uv, url, sizeof url) == 0 && puddle_url_ok(url, PUDDLE_PROFILE_LINK_URL_MAX))
                puddle_jw_str(w, "url", url);
            else
                note(notes, "links: link %d's address is not an http or https address; the text is kept without it", n);
        } else if (urc < 0) {
            note(notes, "links: link %d's address could not be read; the text is kept without it", n);
        }
        puddle_jw_end_object(w);
        kept++;
    }
    puddle_jw_end_array(w);
    if (r < 0)
        note(notes, "links: the list could not be read to the end; kept what came before");
    if (dropped)
        note(notes, "links: %d more than the %d allowed; left out", dropped, PUDDLE_MAX_PROFILE_LINKS);
}

/* ---- the colour ---- */

static void clean_color(puddle_span cv, puddle_jw *w, notes_t *notes)
{
    puddle_span arr, it, av;
    char stops[PUDDLE_MAX_GRADIENT_STOPS][8];
    int n = 0, bad = 0;
    int64_t angle = -1;
    if (cv.len < 2 || cv.p[0] != '{' || puddle_json_member(cv.p, cv.len, "colors", &arr) != 0 || arr.len < 2 || arr.p[0] != '[') {
        note(notes, "color: not a list of colours; left out");
        return;
    }
    size_t cur = 0;
    int r;
    while ((r = puddle_json_array_next(arr, &cur, &it)) == 1) {
        char t[16], shown[100];
        if (puddle_json_string(it, t, sizeof t) != 0 || !hex_color_ok(t)) {
            excerpt(it, shown);
            note(notes, "color: %s is not a colour like \"#3366ff\"; left out", shown);
            continue;
        }
        if (n >= PUDDLE_MAX_GRADIENT_STOPS) {
            if (!bad++)
                note(notes, "color: more than %d colours; the rest left out", PUDDLE_MAX_GRADIENT_STOPS);
            continue;
        }
        for (int k = 0; k < 7; k++)
            stops[n][k] = (char)tolower((unsigned char)t[k]);
        stops[n][7] = '\0';
        n++;
    }
    if (r < 0 || n == 0) {
        note(notes, "color: no colour in it could be used; left out");
        return;
    }
    int arc = puddle_json_member(cv.p, cv.len, "angle", &av);
    if (arc == 0 && (puddle_json_int64(av, &angle) != 0 || angle < 0 || angle > 360)) {
        note(notes, "color: the angle is not a number of degrees from 0 to 360; left out");
        angle = -1;
    }
    puddle_jw_key(w, "color");
    puddle_jw_object(w);
    puddle_jw_key(w, "colors");
    puddle_jw_array(w);
    for (int k = 0; k < n; k++)
        puddle_jw_string(w, stops[k]);
    puddle_jw_end_array(w);
    if (n > 1 && angle >= 0) /* an angle means nothing for one color */
        puddle_jw_num(w, "angle", angle);
    puddle_jw_end_object(w);
}

int puddle_profile_wire_clean(const char *json, size_t len, char *out, size_t cap, char *notes_buf, size_t notes_cap)
{
    static const struct { const char *key; size_t max; int multiline; } texts[] = {
        { "display_name", PUDDLE_PROFILE_NAME_MAX, 0 },
        { "blurb", PUDDLE_PROFILE_BLURB_MAX, 0 },
        { "about", PUDDLE_PROFILE_ABOUT_MAX, 1 },
        { "pronouns", PUDDLE_PROFILE_PRONOUNS_MAX, 0 },
    };
    notes_t notes = { notes_buf, notes_cap, 0 };
    if (notes_buf && notes_cap)
        notes_buf[0] = '\0';
    if (len < 2 || json[0] != '{' || !puddle_json_valid_object(json, len))
        return -1;
    puddle_jw w;
    puddle_jw_init(&w, out, cap);
    puddle_jw_object(&w);
    for (size_t i = 0; i < sizeof texts / sizeof *texts; i++) {
        const char *key = texts[i].key;
        puddle_span v;
        int rc = puddle_json_member(json, len, key, &v);
        if (rc < 0) {
            note(&notes, "%s: given more than once; left out", key);
            continue;
        }
        if (rc == 1 || (v.len == 4 && !memcmp(v.p, "null", 4)))
            continue;
        char *text;
        int cut = text_of(v, texts[i].max, texts[i].multiline, &text);
        if (cut < 0)
            note(&notes, "%s: not text; left out", key);
        else if (cut)
            note(&notes, "%s: longer than %zu characters; cut short", key, texts[i].max);
        if (cut >= 0 && *text)
            puddle_jw_str(&w, key, text);
        free(text);
    }

    puddle_span cv;
    int rc = puddle_json_member(json, len, "color", &cv);
    if (rc < 0)
        note(&notes, "color: given more than once; left out");
    else if (rc == 0 && !(cv.len == 4 && !memcmp(cv.p, "null", 4)))
        clean_color(cv, &w, &notes);

    static const char *const kinds[3] = { "avatar", "banner", "background" };
    for (int k = 0; k < 3; k++) {
        puddle_span mv;
        puddle_media_ref ref;
        int mrc = puddle_json_member(json, len, kinds[k], &mv);
        if (mrc < 0) {
            note(&notes, "%s: given more than once; left out", kinds[k]);
            continue;
        }
        if (mrc == 1 || (mv.len == 4 && !memcmp(mv.p, "null", 4)))
            continue;
        if (media_of(mv, &ref) != 0) {
            note(&notes, "%s: not a picture we can fetch (a png, jpg, gif or webp, named by its hash, up to %u MiB); left out",
                 kinds[k], PUDDLE_IMAGE_MAX_BYTES >> 20);
            continue;
        }
        puddle_jw_key(&w, kinds[k]);
        puddle_jw_object(&w);
        puddle_jw_str(&w, "hash", ref.hash);
        puddle_jw_str(&w, "mime", ref.mime);
        puddle_jw_num(&w, "size", (int64_t)ref.size);
        puddle_jw_end_object(&w);
    }

    puddle_span lv;
    rc = puddle_json_member(json, len, "links", &lv);
    if (rc < 0)
        note(&notes, "links: given more than once; left out");
    else if (rc == 0 && !(lv.len == 4 && !memcmp(lv.p, "null", 4)))
        clean_links(lv, &w, &notes);

    puddle_jw_end_object(&w);
    return puddle_jw_finish(&w) ? 0 : -1;
}

size_t puddle_profile_wire_media(const char *json, size_t len, puddle_media_ref refs[3])
{
    static const char *const kinds[3] = { "avatar", "banner", "background" };
    size_t n = 0;
    for (int k = 0; k < 3; k++) {
        puddle_span mv;
        if (puddle_json_member(json, len, kinds[k], &mv) == 0 && media_of(mv, &refs[n]) == 0)
            n++;
    }
    return n;
}
