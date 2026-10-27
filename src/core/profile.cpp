#include "core/profile.h"

#include "platform/fsutil.h"
#include "platform/paths.h"
#include "storage/storage.h"
#include "base/hash.h"
#include "base/log.h"
#include "base/text.h"
#include "core/link.h"
#include "core/yamltree.h"

#include <algorithm>
#include <errno.h>
#include <stdarg.h>
#include <mutex>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

static void seterr(char *err, size_t n, const char *fmt, ...)
{
    if (!err || !n)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ text */

/* 0 if `s` is well-formed UTF-8, with the number of characters in *chars */
static int utf8_count(const char *s, size_t *chars)
{
    const unsigned char *p = (const unsigned char *)s;
    size_t n = 0;
    while (*p) {
        unsigned char c = *p;
        if (c < 0x80) {
            p += 1;
        } else if (c >= 0xC2 && c <= 0xDF && (p[1] & 0xC0) == 0x80) {
            p += 2;
        } else if (c >= 0xE0 && c <= 0xEF && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
                   !(c == 0xE0 && p[1] < 0xA0) && !(c == 0xED && p[1] >= 0xA0)) {
            p += 3;
        } else if (c >= 0xF0 && c <= 0xF4 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
                   (p[3] & 0xC0) == 0x80 && !(c == 0xF0 && p[1] < 0x90) && !(c == 0xF4 && p[1] >= 0x90)) {
            p += 4;
        } else {
            return -1;
        }
        n++;
    }
    *chars = n;
    return 0;
}

/* the rule every text field keeps: 0, or -1 with why in `err` */
static int check_text(const char *key, const char *s, size_t maxchars, int multiline, int required, char *err, size_t errlen)
{
    if (!s || !*s) {
        if (required) {
            seterr(err, errlen, "%s is required", key);
            return -1;
        }
        return 0;
    }
    size_t chars = 0;
    if (utf8_count(s, &chars) != 0) {
        seterr(err, errlen, "%s is not valid UTF-8", key);
        return -1;
    }
    for (const char *q = s; *q; q++) {
        unsigned char ch = (unsigned char)*q;
        if (ch < 0x20 && !(multiline && (ch == '\n' || ch == '\t'))) {
            seterr(err, errlen, "%s cannot contain control characters or line breaks", key);
            return -1;
        }
    }
    if (chars > maxchars) {
        seterr(err, errlen, "%s is %zu characters; the limit is %zu", key, chars, maxchars);
        return -1;
    }
    return 0;
}

int puddle_profile_id_valid(const char *id)
{
    size_t n = id ? strlen(id) : 0;
    if (n == 0 || n > PUDDLE_PROFILE_ID_MAX)
        return 0;
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return 0;
    }
    return 1;
}

static int hex_color(const char *s)
{
    if (strlen(s) != 7 || s[0] != '#')
        return 0;
    for (int i = 1; i < 7; i++)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
            return 0;
    return 1;
}

/* ------------------------------------------------------------ the profile */

void puddle_profile_free(puddle_profile *p)
{
    free(p->display_name);
    free(p->blurb);
    free(p->about);
    free(p->pronouns);
    for (int i = 0; i < p->nlinks; i++) {
        free(p->links[i].text);
        free(p->links[i].url);
    }
    memset(p, 0, sizeof *p);
}

static int dup_text(char **dst, const char *src)
{
    *dst = NULL;
    if (!src)
        return 0;
    *dst = strdup(src);
    return *dst ? 0 : -1;
}

int puddle_profile_copy(puddle_profile *dst, const puddle_profile *src)
{
    memset(dst, 0, sizeof *dst);
    memcpy(dst->id, src->id, sizeof dst->id);
    dst->color = src->color;
    dst->avatar = src->avatar;
    dst->banner = src->banner;
    dst->background = src->background;
    int bad = dup_text(&dst->display_name, src->display_name) || dup_text(&dst->blurb, src->blurb) ||
              dup_text(&dst->about, src->about) || dup_text(&dst->pronouns, src->pronouns);
    for (int i = 0; !bad && i < src->nlinks; i++) {
        bad = dup_text(&dst->links[i].text, src->links[i].text) || dup_text(&dst->links[i].url, src->links[i].url);
        dst->nlinks = i + 1;
    }
    if (bad) {
        puddle_profile_free(dst);
        return -1;
    }
    return 0;
}

int puddle_profile_check(const puddle_profile *p, char *err, size_t errlen)
{
    if (!puddle_profile_id_valid(p->id)) {
        seterr(err, errlen, "the id must be 1 to %d of a-z, 0-9, - and _", PUDDLE_PROFILE_ID_MAX);
        return -1;
    }
    if (check_text("display_name", p->display_name, PUDDLE_PROFILE_NAME_MAX, 0, 1, err, errlen) != 0 ||
        check_text("blurb", p->blurb, PUDDLE_PROFILE_BLURB_MAX, 0, 0, err, errlen) != 0 ||
        check_text("about", p->about, PUDDLE_PROFILE_ABOUT_MAX, 1, 0, err, errlen) != 0 ||
        check_text("pronouns", p->pronouns, PUDDLE_PROFILE_PRONOUNS_MAX, 0, 0, err, errlen) != 0)
        return -1;
    if (p->color.kind != PUDDLE_COLOR_NONE) {
        int solid = p->color.kind == PUDDLE_COLOR_SOLID;
        if (p->color.nstops < (solid ? 1 : 2) || p->color.nstops > (solid ? 1 : PUDDLE_MAX_GRADIENT_STOPS)) {
            seterr(err, errlen, "a colour is one \"#rrggbb\", or a gradient of 2 to %d", PUDDLE_MAX_GRADIENT_STOPS);
            return -1;
        }
        for (int i = 0; i < p->color.nstops; i++)
            if (!hex_color(p->color.stops[i])) {
                seterr(err, errlen, "colours must look like \"#3366ff\"");
                return -1;
            }
        if (!solid && (p->color.angle < 0 || p->color.angle > 360)) {
            seterr(err, errlen, "a gradient's angle is 0 to 360 degrees");
            return -1;
        }
    }
    const puddle_media *imgs[3] = { &p->avatar, &p->banner, &p->background };
    for (int i = 0; i < 3; i++)
        if (imgs[i]->hash[0] && (!puddle_media_hash_valid(imgs[i]->hash) || !imgs[i]->mime[0] || imgs[i]->size == 0)) {
            seterr(err, errlen, "a picture must be one this Puddle holds");
            return -1;
        }
    if (p->nlinks < 0 || p->nlinks > PUDDLE_MAX_PROFILE_LINKS) {
        seterr(err, errlen, "at most %d links", PUDDLE_MAX_PROFILE_LINKS);
        return -1;
    }
    for (int i = 0; i < p->nlinks; i++) {
        if (check_text("a link's text", p->links[i].text, PUDDLE_PROFILE_LINK_TEXT_MAX, 0, 1, err, errlen) != 0)
            return -1;
        if (p->links[i].url && !puddle_url_ok(p->links[i].url, PUDDLE_PROFILE_LINK_URL_MAX)) {
            seterr(err, errlen, "a link's address must start with http:// or https:// and have no spaces or quotes");
            return -1;
        }
    }
    return 0;
}

static void media_member(puddle_jw *w, const char *key, const puddle_media *m)
{
    if (!m->hash[0])
        return;
    puddle_jw_key(w, key);
    puddle_jw_object(w);
    puddle_jw_str(w, "hash", m->hash);
    puddle_jw_str(w, "mime", m->mime);
    puddle_jw_num(w, "size", (int64_t)m->size);
    puddle_jw_end_object(w);
}

void puddle_profile_write_fields(puddle_jw *w, const puddle_profile *p)
{
    puddle_jw_str(w, "display_name", p->display_name ? p->display_name : "");
    if (p->blurb)
        puddle_jw_str(w, "blurb", p->blurb);
    if (p->about)
        puddle_jw_str(w, "about", p->about);
    if (p->pronouns)
        puddle_jw_str(w, "pronouns", p->pronouns);
    if (p->color.kind != PUDDLE_COLOR_NONE) {
        puddle_jw_key(w, "color");
        puddle_jw_object(w);
        puddle_jw_key(w, "colors");
        puddle_jw_array(w);
        for (int i = 0; i < p->color.nstops; i++)
            puddle_jw_string(w, p->color.stops[i]);
        puddle_jw_end_array(w);
        if (p->color.kind == PUDDLE_COLOR_GRADIENT)
            puddle_jw_num(w, "angle", p->color.angle);
        puddle_jw_end_object(w);
    }
    media_member(w, "avatar", &p->avatar);
    media_member(w, "banner", &p->banner);
    media_member(w, "background", &p->background);
    if (p->nlinks > 0) {
        puddle_jw_key(w, "links");
        puddle_jw_array(w);
        for (int i = 0; i < p->nlinks; i++) {
            puddle_jw_object(w);
            puddle_jw_str(w, "text", p->links[i].text);
            if (p->links[i].url)
                puddle_jw_str(w, "url", p->links[i].url);
            puddle_jw_end_object(w);
        }
        puddle_jw_end_array(w);
    }
}

/* ---------------------------------------------------------------- images */

/* Keep `data` in <data_dir>/media and describe it. 0, or -1 with why in `err`. */
static int keep_image(const char *data_dir, const unsigned char *data, size_t len, puddle_media *out, char *err, size_t errlen)
{
    memset(out, 0, sizeof *out);
    if (len == 0 || len > PUDDLE_IMAGE_MAX_BYTES) {
        seterr(err, errlen, "a picture is 1 byte to %u MiB", PUDDLE_IMAGE_MAX_BYTES >> 20);
        return -1;
    }
    const char *mime = puddle_image_sniff(data, len);
    if (!mime) {
        seterr(err, errlen, "not a png, jpg, gif or webp image");
        return -1;
    }
    char hash[65], why[200];
    puddle_sha256_hex(data, len, hash);
    if (puddle_media_store(data_dir, hash, data, len, len, mime, why, sizeof why) != 0) {
        seterr(err, errlen, "cannot keep the picture: %s", why);
        return -1;
    }
    snprintf(out->hash, sizeof out->hash, "%s", hash);
    snprintf(out->mime, sizeof out->mime, "%s", mime);
    out->size = len;
    return 0;
}

/* describe the image `m->hash` that we hold: 0, or 1 if we do not */
static int describe_image(const char *data_dir, puddle_media *m)
{
    unsigned char *data;
    size_t len;
    char mime[40];
    if (puddle_media_read(data_dir, m->hash, PUDDLE_IMAGE_MAX_BYTES, &data, &len, mime) != 0)
        return 1;
    free(data);
    snprintf(m->mime, sizeof m->mime, "%s", mime);
    m->size = len;
    return 0;
}

/* an image file on disk, brought into media: 0 kept, 1 left out with a warning */
static int import_image_file(const char *data_dir, const char *path, const char *who, puddle_media *out)
{
    memset(out, 0, sizeof *out);
    int fd = puddle_fd_open(path, PUDDLE_F_READ);
    if (fd < 0) {
        puddle_log(PUDDLE_LOG_WARN, "profile %s: cannot read image %s (%s); leaving it out", who, path, strerror(errno));
        return 1;
    }
    int64_t fsize = puddle_fd_size(fd);
    if (fsize <= 0 || (uint64_t)fsize > PUDDLE_IMAGE_MAX_BYTES) {
        puddle_fd_close(fd);
        puddle_log(PUDDLE_LOG_WARN, "profile %s: %s is not a regular file of 1 byte to %u MiB; leaving it out", who, path,
                   PUDDLE_IMAGE_MAX_BYTES >> 20);
        return 1;
    }
    size_t size = (size_t)fsize, got = 0;
    unsigned char *buf = (unsigned char *)malloc(size);
    while (buf && got < size) {
        int64_t r = puddle_fd_read(fd, buf + got, size - got);
        if (r <= 0)
            break;
        got += (size_t)r;
    }
    puddle_fd_close(fd);
    char why[200];
    int whole = buf && got == size;
    int rc = whole && keep_image(data_dir, buf, size, out, why, sizeof why) == 0 ? 0 : 1;
    if (rc != 0)
        puddle_log(PUDDLE_LOG_WARN, "profile %s: %s: %s; leaving it out", who, path, whole ? why : "cannot be read");
    free(buf);
    return rc;
}

/* ---------------------------------------------------------- reading YAML */

typedef struct {
    const char *file;
    char dir[PUDDLE_PATH_MAX]; /* where relative paths in it start */
    const char *data_dir;
    char *err;
    size_t errlen;
} loadctx;

static int fail(const loadctx *c, const ynode *n, const char *profile, const char *fmt, ...)
{
    char what[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(what, sizeof what, fmt, ap);
    va_end(ap);
    if (profile && *profile)
        seterr(c->err, c->errlen, "%s:%d: profile '%s': %s", c->file, ynode_line(n), profile, what);
    else
        seterr(c->err, c->errlen, "%s:%d: %s", c->file, ynode_line(n), what);
    return -1;
}

static char *trimmed_copy(const char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
        s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' || s[n - 1] == '\r'))
        n--;
    char *out = (char *)malloc(n + 1);
    if (out) {
        memcpy(out, s, n);
        out[n] = '\0';
    }
    return out;
}

/* a text field into *out. A null/absent value leaves *out NULL. 0 or -1 (error already set). */
static int text_field(const loadctx *c, const ynode *v, const char *profile, const char *key, size_t maxchars, int multiline, char **out)
{
    if (ynode_is_null(v))
        return 0;
    const char *s = ynode_scalar(v);
    if (!s)
        return fail(c, v, profile, "%s must be text", key);
    char *t = trimmed_copy(s);
    if (!t)
        return fail(c, v, profile, "out of memory");
    char why[200];
    if (check_text(key, t, maxchars, multiline, 0, why, sizeof why) != 0) {
        free(t);
        return fail(c, v, profile, "%s", why);
    }
    if (!*t) {
        free(t);
        return 0;
    }
    free(*out);
    *out = t;
    return 0;
}

/* "#rgb" or "#rrggbb" into "#rrggbb" lowercase */
static int parse_hex_color(const char *s, char out[8])
{
    size_t n = strlen(s);
    if ((n != 4 && n != 7) || s[0] != '#')
        return -1;
    for (size_t i = 1; i < n; i++) {
        char ch = s[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F')))
            return -1;
    }
    out[0] = '#';
    for (int i = 0; i < 6; i++) {
        char ch = n == 4 ? s[1 + i / 2] : s[1 + i];
        out[1 + i] = (ch >= 'A' && ch <= 'F') ? (char)(ch - 'A' + 'a') : ch;
    }
    out[7] = '\0';
    return 0;
}

static int color_field(const loadctx *c, const ynode *v, const char *profile, puddle_color *out)
{
    if (ynode_is_null(v))
        return 0;
    if (ynode_kind(v) == Y_SCALAR) {
        if (parse_hex_color(ynode_scalar(v), out->stops[0]) != 0)
            return fail(c, v, profile, "color must look like \"#3366ff\", or be a gradient");
        out->kind = PUDDLE_COLOR_SOLID;
        out->nstops = 1;
        return 0;
    }
    if (ynode_kind(v) != Y_MAP)
        return fail(c, v, profile, "color must be \"#rrggbb\" or a gradient");

    const ynode *g = NULL;
    int angle = 180;
    for (size_t i = 0; i < ynode_count(v); i++) {
        const char *k = ynode_key(v, i);
        const ynode *x = ynode_item(v, i);
        if (strcmp(k, "gradient") == 0) {
            g = x;
        } else if (strcmp(k, "angle") == 0) {
            const char *a = ynode_scalar(x);
            char *end = NULL;
            long val = a ? strtol(a, &end, 10) : -1;
            if (!a || !*a || *end != '\0' || val < 0 || val > 360)
                return fail(c, x, profile, "angle must be a number of degrees from 0 to 360");
            angle = (int)val;
        } else {
            return fail(c, x, profile, "unknown color setting '%.40s' (use gradient and angle)", k);
        }
    }
    if (!g || ynode_kind(g) != Y_SEQ || ynode_count(g) < 2 || ynode_count(g) > PUDDLE_MAX_GRADIENT_STOPS)
        return fail(c, g ? g : v, profile, "gradient needs a list of 2 to %d colors", PUDDLE_MAX_GRADIENT_STOPS);
    for (size_t i = 0; i < ynode_count(g); i++) {
        const char *s = ynode_scalar(ynode_item(g, i));
        if (!s || parse_hex_color(s, out->stops[i]) != 0)
            return fail(c, ynode_item(g, i), profile, "gradient colors must look like \"#3366ff\"");
    }
    out->kind = PUDDLE_COLOR_GRADIENT;
    out->nstops = (int)ynode_count(g);
    out->angle = angle;
    return 0;
}

/* An image: the hash of one in media (as the Puddle writes it), or a path to a file to bring in. One that cannot be had is
 * left out with a warning, never an error: a path on an unplugged drive should not cost the whole profile. */
static int image_field(const loadctx *c, const ynode *v, const char *profile, puddle_media *out)
{
    memset(out, 0, sizeof *out);
    if (ynode_is_null(v))
        return 0;
    const char *s = ynode_scalar(v);
    if (!s || !*s)
        return fail(c, v, profile, "a picture is the hash of one in the media folder, or a file path");
    if (puddle_media_hash_valid(s)) {
        snprintf(out->hash, sizeof out->hash, "%s", s);
        if (describe_image(c->data_dir, out) != 0) {
            puddle_log(PUDDLE_LOG_WARN, "%s:%d: profile %s: the picture %.12s… is not in the media folder; leaving it out", c->file,
                       ynode_line(v), profile, s);
            memset(out, 0, sizeof *out);
        }
        return 0;
    }
    char path[PUDDLE_PATH_MAX];
    if (puddle_resolve_path(c->dir, s, path, sizeof path) != 0)
        return fail(c, v, profile, "a picture is the hash of one in the media folder, or a file path");
    import_image_file(c->data_dir, path, profile, out);
    return 0;
}

static int links_field(const loadctx *c, const ynode *v, const char *profile, puddle_profile *p)
{
    if (ynode_is_null(v))
        return 0;
    if (ynode_kind(v) != Y_SEQ)
        return fail(c, v, profile, "links must be a list; start each with \"- text: ...\"");
    if (ynode_count(v) > PUDDLE_MAX_PROFILE_LINKS)
        return fail(c, v, profile, "at most %d links", PUDDLE_MAX_PROFILE_LINKS);
    for (size_t i = 0; i < ynode_count(v); i++) {
        const ynode *l = ynode_item(v, i);
        if (ynode_kind(l) != Y_MAP)
            return fail(c, l, profile, "each link is \"text:\" and, if it has one, \"url:\"");
        puddle_profile_link *link = &p->links[p->nlinks++];
        for (size_t k = 0; k < ynode_count(l); k++) {
            const char *key = ynode_key(l, k);
            const ynode *x = ynode_item(l, k);
            if (!strcmp(key, "text")) {
                if (text_field(c, x, profile, "a link's text", PUDDLE_PROFILE_LINK_TEXT_MAX, 0, &link->text) != 0)
                    return -1;
            } else if (!strcmp(key, "url")) {
                if (ynode_is_null(x))
                    continue;
                const char *u = ynode_scalar(x);
                if (!u || !puddle_url_ok(u, PUDDLE_PROFILE_LINK_URL_MAX))
                    return fail(c, x, profile, "a link's url must start with http:// or https:// and have no spaces or quotes");
                free(link->url);
                link->url = strdup(u);
                if (!link->url)
                    return fail(c, x, profile, "out of memory");
            } else {
                return fail(c, x, profile, "unknown link setting '%.40s' (use text and url)", key);
            }
        }
        if (!link->text)
            return fail(c, l, profile, "every link needs text");
    }
    return 0;
}

/* The settings of one profile, from a map. `id_key` says whether an `id:` setting belongs in it (the old shared files),
 * or the id comes from the file's name (already in p->id). 0, or -1 with the error set and `p` freed. */
static int load_fields(const loadctx *c, const ynode *item, int id_key, puddle_profile *p)
{
    if (ynode_kind(item) != Y_MAP)
        return fail(c, item, NULL, "a profile is settings like \"display_name: ...\"");
    if (id_key) {
        const ynode *idn = ynode_get(item, "id");
        const char *id = idn ? ynode_scalar(idn) : NULL;
        if (!id || !puddle_profile_id_valid(id))
            return fail(c, idn ? idn : item, NULL, "every profile needs an id of 1-%d letters, digits, - or _ (lowercase)",
                        PUDDLE_PROFILE_ID_MAX);
        snprintf(p->id, sizeof p->id, "%s", id);
    }
    const char *id = p->id;
    for (size_t i = 0; i < ynode_count(item); i++) {
        const char *k = ynode_key(item, i);
        const ynode *v = ynode_item(item, i);
        int rc = 0;
        if (strcmp(k, "id") == 0 && id_key) {
            /* done */
        } else if (strcmp(k, "display_name") == 0) {
            rc = text_field(c, v, id, k, PUDDLE_PROFILE_NAME_MAX, 0, &p->display_name);
        } else if (strcmp(k, "blurb") == 0) {
            rc = text_field(c, v, id, k, PUDDLE_PROFILE_BLURB_MAX, 0, &p->blurb);
        } else if (strcmp(k, "about") == 0) {
            rc = text_field(c, v, id, k, PUDDLE_PROFILE_ABOUT_MAX, 1, &p->about);
        } else if (strcmp(k, "pronouns") == 0) {
            rc = text_field(c, v, id, k, PUDDLE_PROFILE_PRONOUNS_MAX, 0, &p->pronouns);
        } else if (strcmp(k, "color") == 0) {
            rc = color_field(c, v, id, &p->color);
        } else if (strcmp(k, "avatar") == 0) {
            rc = image_field(c, v, id, &p->avatar);
        } else if (strcmp(k, "banner") == 0) {
            rc = image_field(c, v, id, &p->banner);
        } else if (strcmp(k, "background") == 0) {
            rc = image_field(c, v, id, &p->background);
        } else if (strcmp(k, "links") == 0) {
            rc = links_field(c, v, id, p);
        } else {
            rc = fail(c, v, id, "unknown setting '%.40s'", k);
        }
        if (rc != 0) {
            puddle_profile_free(p);
            return -1;
        }
    }
    if (!p->display_name) {
        int rc = fail(c, item, id, "display_name is required");
        puddle_profile_free(p);
        return rc;
    }
    return 0;
}

static void ctx_for(loadctx *c, const char *file, const char *data_dir, char *err, size_t errlen)
{
    memset(c, 0, sizeof *c);
    c->file = file;
    c->data_dir = data_dir;
    c->err = err;
    c->errlen = errlen;
    snprintf(c->dir, sizeof c->dir, "%s", file);
    char *slash = strrchr(c->dir, '/');
    if (slash)
        *slash = '\0';
    else
        snprintf(c->dir, sizeof c->dir, ".");
}

/* One file of the data folder, <id>.yaml: the settings of the profile <id>. */
static int load_profile_file(const char *data_dir, const char *path, const char *id, puddle_profile *p, char *err, size_t errlen)
{
    memset(p, 0, sizeof *p);
    snprintf(p->id, sizeof p->id, "%s", id);
    ynode *root = puddle_yaml_parse_file(path, err, errlen);
    if (!root)
        return -1;
    loadctx c;
    ctx_for(&c, path, data_dir, err, errlen);
    int rc = load_fields(&c, root, 0, p);
    puddle_yaml_free(root);
    return rc;
}

/* A file of the kind the `profiles:` setting named: "profiles:" and a list, each with an id. */
static int load_old_file(const char *data_dir, const char *file, std::vector<puddle_profile> &out, char *err, size_t errlen)
{
    ynode *root = puddle_yaml_parse_file(file, err, errlen);
    if (!root)
        return -1;
    loadctx c;
    ctx_for(&c, file, data_dir, err, errlen);
    int rc = 0;
    const ynode *list = NULL;
    if (ynode_kind(root) != Y_MAP) {
        rc = fail(&c, root, NULL, "expected \"profiles:\" followed by a list");
        goto done;
    }
    for (size_t i = 0; i < ynode_count(root); i++) {
        if (strcmp(ynode_key(root, i), "profiles") != 0) {
            rc = fail(&c, ynode_item(root, i), NULL, "unknown setting '%.40s' (a profile file holds \"profiles:\" only)", ynode_key(root, i));
            goto done;
        }
        list = ynode_item(root, i);
    }
    if (!list || ynode_is_null(list))
        goto done;
    if (ynode_kind(list) != Y_SEQ) {
        rc = fail(&c, list, NULL, "profiles must be a list; start each profile with \"- id: ...\"");
        goto done;
    }
    for (size_t i = 0; i < ynode_count(list); i++) {
        const ynode *item = ynode_item(list, i);
        if (out.size() >= PUDDLE_MAX_PROFILES) {
            rc = fail(&c, item, NULL, "at most %d profiles", PUDDLE_MAX_PROFILES);
            goto done;
        }
        puddle_profile p;
        memset(&p, 0, sizeof p);
        if (load_fields(&c, item, 1, &p) != 0) {
            rc = -1;
            goto done;
        }
        bool taken = false;
        for (const auto &q : out)
            taken = taken || !strcmp(q.id, p.id);
        if (taken) {
            rc = fail(&c, item, NULL, "there is already a profile with id '%s'", p.id);
            puddle_profile_free(&p);
            goto done;
        }
        out.push_back(p);
    }
done:
    puddle_yaml_free(root);
    return rc;
}

/* ---------------------------------------------------------- writing YAML */

/* `s` as a YAML double-quoted scalar: the JSON spelling of a string is one */
static void yaml_string(std::string &out, const char *s)
{
    size_t cap = strlen(s) * 6 + 8;
    std::string buf(cap, '\0');
    puddle_jw w;
    puddle_jw_init(&w, &buf[0], cap);
    puddle_jw_string(&w, s);
    size_t n = puddle_jw_finish(&w);
    out.append(buf.data(), n);
}

static std::string to_yaml(const puddle_profile *p)
{
    std::string y = "# A Puddle profile. Puddle writes this file when the profile is changed in the app; you can also edit it\n"
                    "# while Puddle is not running (docs/profiles.md). A picture is named by its hash in the media folder, or\n"
                    "# by the path of a file to bring in.\n";
    const struct { const char *key; const char *val; } texts[] = {
        { "display_name", p->display_name }, { "blurb", p->blurb }, { "about", p->about }, { "pronouns", p->pronouns },
    };
    for (const auto &t : texts)
        if (t.val) {
            y += t.key;
            y += ": ";
            yaml_string(y, t.val);
            y += "\n";
        }
    if (p->color.kind == PUDDLE_COLOR_SOLID) {
        y += "color: \"";
        y += p->color.stops[0];
        y += "\"\n";
    } else if (p->color.kind == PUDDLE_COLOR_GRADIENT) {
        y += "color:\n  gradient: [";
        for (int i = 0; i < p->color.nstops; i++) {
            y += i ? ", \"" : "\"";
            y += p->color.stops[i];
            y += "\"";
        }
        y += "]\n  angle: " + std::to_string(p->color.angle) + "\n";
    }
    const struct { const char *key; const puddle_media *m; } imgs[] = {
        { "avatar", &p->avatar }, { "banner", &p->banner }, { "background", &p->background },
    };
    for (const auto &i : imgs)
        if (i.m->hash[0]) {
            y += i.key;
            y += ": ";
            y += i.m->hash;
            y += "\n";
        }
    if (p->nlinks > 0) {
        y += "links:\n";
        for (int i = 0; i < p->nlinks; i++) {
            y += "  - text: ";
            yaml_string(y, p->links[i].text);
            y += "\n";
            if (p->links[i].url) {
                y += "    url: ";
                yaml_string(y, p->links[i].url);
                y += "\n";
            }
        }
    }
    return y;
}

/* ------------------------------------------------------------- the store */

struct puddle_profiles {
    std::string data_dir, dir; /* dir: <data_dir>/profiles */
    std::mutex mu;
    std::vector<puddle_profile> items; /* sorted by id */
};

static std::string file_in(const std::string &dir, const char *id)
{
    return dir + "/" + id + ".yaml";
}

static void sort_items(puddle_profiles *p)
{
    std::sort(p->items.begin(), p->items.end(), [](const puddle_profile &a, const puddle_profile &b) { return strcmp(a.id, b.id) < 0; });
}

static int write_file(const std::string &dir, const puddle_profile *pr)
{
    std::string y = to_yaml(pr);
    return puddle_write_file_atomic(dir.c_str(), file_in(dir, pr->id).c_str(), y.data(), y.size());
}

static int read_folder(puddle_profiles *p)
{
    puddle_dir *d = puddle_dir_open(p->dir.c_str());
    if (!d)
        return -1;
    const char *name;
    while ((name = puddle_dir_next(d))) {
        size_t n = strlen(name);
        if (n <= 5 || strcmp(name + n - 5, ".yaml") != 0)
            continue;
        std::string id(name, n - 5);
        std::string path = p->dir + "/" + name;
        if (!puddle_profile_id_valid(id.c_str())) {
            puddle_log(PUDDLE_LOG_WARN, "%s: a profile file is named <id>.yaml, an id being 1 to %d of a-z, 0-9, - and _; leaving it out",
                       path.c_str(), PUDDLE_PROFILE_ID_MAX);
            continue;
        }
        if (p->items.size() >= PUDDLE_MAX_PROFILES) {
            puddle_log(PUDDLE_LOG_WARN, "%s: there are already %d profiles, the most there can be; leaving it out", path.c_str(),
                       PUDDLE_MAX_PROFILES);
            continue;
        }
        puddle_profile pr;
        char err[400];
        if (load_profile_file(p->data_dir.c_str(), path.c_str(), id.c_str(), &pr, err, sizeof err) != 0) {
            puddle_log(PUDDLE_LOG_WARN, "%s; this profile is left out until the file is fixed", err);
            continue;
        }
        p->items.push_back(pr);
    }
    puddle_dir_close(d);
    sort_items(p);
    return 0;
}

/* The first start with profiles in the data folder: write the folder's first files. 0 or -1 (error set). */
static int make_folder(puddle_profiles *p, const puddle_config *cfg, char old_default[PUDDLE_PROFILE_ID_MAX + 1], char *err, size_t errlen)
{
    /* before profiles lived here: bring in the ones the `profiles:` setting names, in their order */
    std::vector<puddle_profile> old;
    for (size_t i = 0; i < cfg->nprofile_files; i++)
        if (load_old_file(p->data_dir.c_str(), cfg->profile_files[i], old, err, errlen) != 0) {
            for (auto &q : old)
                puddle_profile_free(&q);
            return -1;
        }
    if (old.empty()) { /* nothing to bring in: one to start from, named after the OS account, shown to no one yet */
        puddle_profile q;
        memset(&q, 0, sizeof q);
        snprintf(q.id, sizeof q.id, "default");
        char name[64];
        puddle_login_name(name, sizeof name);
        q.display_name = strdup(name[0] ? name : "Me");
        if (!q.display_name) {
            seterr(err, errlen, "out of memory");
            return -1;
        }
        old.push_back(q);
    } else {
        snprintf(old_default, PUDDLE_PROFILE_ID_MAX + 1, "%s", old[0].id);
    }
    /* written into a folder beside it, which takes the folder's name only once every file is in */
    std::string fresh = p->dir + ".new";
    int bad = puddle_mkdir_p(fresh.c_str()) != 0;
    for (size_t i = 0; !bad && i < old.size(); i++)
        bad = write_file(fresh, &old[i]) != 0;
    for (auto &q : old)
        puddle_profile_free(&q);
    if (bad || rename(fresh.c_str(), p->dir.c_str()) != 0) {
        seterr(err, errlen, "cannot write the profiles into %s: %s", p->dir.c_str(), strerror(errno));
        old_default[0] = '\0';
        return -1;
    }
    if (old_default[0])
        puddle_log(PUDDLE_LOG_INFO, "profiles brought into %s; the `profiles:` setting in puddle.yaml is no longer read", p->dir.c_str());
    return 0;
}

puddle_profiles *puddle_profiles_open(const puddle_config *cfg, char old_default[PUDDLE_PROFILE_ID_MAX + 1], char *err, size_t errlen)
{
    old_default[0] = '\0';
    puddle_profiles *p = new (std::nothrow) puddle_profiles();
    if (!p) {
        seterr(err, errlen, "out of memory");
        return NULL;
    }
    p->data_dir = cfg->data_dir;
    p->dir = p->data_dir + "/profiles";
    if (!puddle_path_exists(p->dir.c_str()) && make_folder(p, cfg, old_default, err, errlen) != 0) {
        puddle_profiles_close(p);
        return NULL;
    }
    if (read_folder(p) != 0) {
        seterr(err, errlen, "cannot read %s: %s", p->dir.c_str(), strerror(errno));
        old_default[0] = '\0';
        puddle_profiles_close(p);
        return NULL;
    }
    return p;
}

void puddle_profiles_close(puddle_profiles *p)
{
    if (!p)
        return;
    for (auto &q : p->items)
        puddle_profile_free(&q);
    delete p;
}

static puddle_profile *find_locked(puddle_profiles *p, const char *id)
{
    for (auto &q : p->items)
        if (!strcmp(q.id, id))
            return &q;
    return NULL;
}

size_t puddle_profiles_ids(puddle_profiles *p, char (*ids)[PUDDLE_PROFILE_ID_MAX + 1], size_t cap)
{
    std::lock_guard<std::mutex> hold(p->mu);
    for (size_t i = 0; i < p->items.size() && i < cap; i++)
        memcpy(ids[i], p->items[i].id, PUDDLE_PROFILE_ID_MAX + 1);
    return p->items.size();
}

int puddle_profiles_get(puddle_profiles *p, const char *id, puddle_profile *out)
{
    memset(out, 0, sizeof *out);
    std::lock_guard<std::mutex> hold(p->mu);
    const puddle_profile *q = id ? find_locked(p, id) : NULL;
    if (!q)
        return 1;
    return puddle_profile_copy(out, q) == 0 ? 0 : 1;
}

int puddle_profiles_has(puddle_profiles *p, const char *id)
{
    std::lock_guard<std::mutex> hold(p->mu);
    return id && find_locked(p, id) != NULL;
}

int puddle_profiles_save(puddle_profiles *p, const puddle_profile *pr, int create, const char **code, char *err, size_t errlen)
{
    *code = "bad_request";
    if (puddle_profile_check(pr, err, errlen) != 0)
        return -1;
    const puddle_media *imgs[3] = { &pr->avatar, &pr->banner, &pr->background };
    for (int i = 0; i < 3; i++)
        if (imgs[i]->hash[0] && !puddle_media_have(p->data_dir.c_str(), imgs[i]->hash)) {
            *code = "no_such_image";
            seterr(err, errlen, "that picture is not here; upload it first");
            return -1;
        }
    std::lock_guard<std::mutex> hold(p->mu);
    puddle_profile *have = find_locked(p, pr->id);
    if (create && have) {
        *code = "profile_exists";
        seterr(err, errlen, "there is already a profile called '%s'", pr->id);
        return -1;
    }
    if (!create && !have) {
        *code = "no_such_profile";
        seterr(err, errlen, "there is no profile called '%s'", pr->id);
        return -1;
    }
    if (create && p->items.size() >= PUDDLE_MAX_PROFILES) {
        *code = "too_many_profiles";
        seterr(err, errlen, "there can be at most %d profiles", PUDDLE_MAX_PROFILES);
        return -1;
    }
    puddle_profile copy;
    if (puddle_profile_copy(&copy, pr) != 0 || puddle_mkdir_p(p->dir.c_str()) != 0 || write_file(p->dir, pr) != 0) {
        puddle_profile_free(&copy);
        *code = "internal";
        seterr(err, errlen, "cannot write %s: %s", file_in(p->dir, pr->id).c_str(), strerror(errno));
        return -1;
    }
    if (have) {
        puddle_profile_free(have);
        *have = copy;
    } else {
        p->items.push_back(copy);
        sort_items(p);
    }
    return 0;
}

int puddle_profiles_remove(puddle_profiles *p, const char *id)
{
    std::lock_guard<std::mutex> hold(p->mu);
    for (size_t i = 0; id && i < p->items.size(); i++)
        if (!strcmp(p->items[i].id, id)) {
            if (puddle_remove_file(file_in(p->dir, id).c_str()) != 0 && errno != ENOENT)
                return -1;
            puddle_profile_free(&p->items[i]);
            p->items.erase(p->items.begin() + (long)i);
            return 0;
        }
    return 1;
}

int puddle_profiles_add_image(puddle_profiles *p, const unsigned char *data, size_t len, puddle_media *out, char *err, size_t errlen)
{
    return keep_image(p->data_dir.c_str(), data, len, out, err, errlen);
}

int puddle_profiles_image(puddle_profiles *p, puddle_media *m)
{
    if (!puddle_media_hash_valid(m->hash))
        return 1;
    return describe_image(p->data_dir.c_str(), m);
}

/* ---------------------------------------------------------------- places */

int puddle_place_dm(const char *user_id, char out[PUDDLE_PLACE_MAX])
{
    if (!puddle_id_valid(user_id))
        return -1;
    snprintf(out, PUDDLE_PLACE_MAX, "dm:%s", user_id);
    return 0;
}

int puddle_place_group(const char *group_id, char out[PUDDLE_PLACE_MAX])
{
    if (!puddle_id_valid(group_id))
        return -1;
    snprintf(out, PUDDLE_PLACE_MAX, "group:%s", group_id);
    return 0;
}

const char *puddle_place_group_id(const char *place)
{
    return place && !strncmp(place, "group:", 6) && puddle_id_valid(place + 6) ? place + 6 : NULL;
}

const char *puddle_place_dm_id(const char *place)
{
    return place && !strncmp(place, "dm:", 3) && puddle_id_valid(place + 3) ? place + 3 : NULL;
}

int puddle_place_valid(const char *place)
{
    return puddle_place_dm_id(place) || puddle_place_group_id(place);
}

/* ---------------------------------------------------------- assignments */

typedef struct {
    char place[PUDDLE_PLACE_MAX];
    char profile[PUDDLE_PROFILE_ID_MAX + 1];
} pair;

#define ASSIGNMENTS "profile-places.tsv"     /* a single document in the "state" collection */
#define OLD_ASSIGNMENTS "stream-profiles.tsv" /* before: one line per stream, "<user id>\t<profile id>" */

struct puddle_assignments {
    puddle_storage *storage;
    puddle_storage *owned; /* the files driver opened for a data directory; NULL when handed one */
    std::mutex mu;
    pair *items;
    size_t n;
};

static int persist_locked(puddle_assignments *a)
{
    size_t cap = a->n * (PUDDLE_PLACE_MAX + PUDDLE_PROFILE_ID_MAX + 4) + 16;
    char *buf = (char *)malloc(cap);
    if (!buf)
        return -1;
    size_t len = 0;
    for (size_t i = 0; i < a->n; i++)
        len += (size_t)snprintf(buf + len, cap - len, "%s\t%s\n", a->items[i].place, a->items[i].profile);
    int rc = puddle_record_put(a->storage, "state", ASSIGNMENTS, buf, len);
    free(buf);
    return rc;
}

/* the lines of a document into `a`; an `old` document names a user, which is the place "dm:<user>" */
static void read_lines(puddle_assignments *a, char *doc, const char *name, int old)
{
    char *next = doc;
    while (next && *next) {
        char *line = next;
        next = strchr(line, '\n');
        if (next)
            *next++ = '\0';
        line[strcspn(line, "\r")] = '\0';
        char *tab = strchr(line, '\t');
        char place[PUDDLE_PLACE_MAX];
        if (tab)
            *tab = '\0';
        int ok = tab && puddle_profile_id_valid(tab + 1) &&
                 (old ? puddle_place_dm(line, place) == 0
                      : strlen(line) < sizeof place && puddle_place_valid(line) && snprintf(place, sizeof place, "%s", line) > 0);
        if (!ok) {
            if (*line)
                puddle_log(PUDDLE_LOG_WARN, "state: ignoring a bad line in %s", name);
            continue;
        }
        pair *g = (pair *)realloc(a->items, (a->n + 1) * sizeof *g);
        if (!g)
            break;
        a->items = g;
        snprintf(g[a->n].place, sizeof g[a->n].place, "%s", place);
        snprintf(g[a->n].profile, sizeof g[a->n].profile, "%s", tab + 1);
        a->n++;
    }
}

puddle_assignments *puddle_assignments_open(const char *data_dir, char *err, size_t errlen)
{
    puddle_storage *st = puddle_storage_open_files(data_dir, err, errlen);
    if (!st)
        return NULL;
    puddle_assignments *a = puddle_assignments_open_with(st, err, errlen);
    if (!a) {
        puddle_storage_close(st);
        return NULL;
    }
    a->owned = st;
    return a;
}

puddle_assignments *puddle_assignments_open_with(puddle_storage *st, char *err, size_t errlen)
{
    puddle_assignments *a = new (std::nothrow) puddle_assignments();
    if (!a) {
        seterr(err, errlen, "out of memory");
        return NULL;
    }
    a->storage = st;
    char *doc;
    size_t dl;
    int have = puddle_record_get(st, "state", ASSIGNMENTS, &doc, &dl);
    int old = 0;
    if (have == 1) { /* not yet: perhaps the choices made per stream before groups had their own */
        have = puddle_record_get(st, "state", OLD_ASSIGNMENTS, &doc, &dl);
        old = have == 0;
    }
    if (have < 0) {
        seterr(err, errlen, "cannot read the profile choices: %s", strerror(errno));
        puddle_assignments_close(a);
        return NULL;
    }
    if (have == 0) {
        read_lines(a, doc, old ? OLD_ASSIGNMENTS : ASSIGNMENTS, old);
        free(doc);
    }
    if (old && persist_locked(a) != 0) {
        seterr(err, errlen, "cannot write %s: %s", ASSIGNMENTS, strerror(errno));
        puddle_assignments_close(a);
        return NULL;
    }
    return a;
}

void puddle_assignments_close(puddle_assignments *a)
{
    if (!a)
        return;
    free(a->items);
    puddle_storage_close(a->owned);
    delete a;
}

int puddle_assignments_get(puddle_assignments *a, const char *place, char out[PUDDLE_PROFILE_ID_MAX + 1])
{
    std::lock_guard<std::mutex> hold(a->mu);
    for (size_t i = 0; place && i < a->n; i++)
        if (strcmp(a->items[i].place, place) == 0) {
            memcpy(out, a->items[i].profile, PUDDLE_PROFILE_ID_MAX + 1);
            return 0;
        }
    return 1;
}

int puddle_assignments_set(puddle_assignments *a, const char *place, const char *profile_id, char *err, size_t errlen)
{
    if (!place || strlen(place) >= PUDDLE_PLACE_MAX || !puddle_place_valid(place) || (profile_id && !puddle_profile_id_valid(profile_id))) {
        seterr(err, errlen, "not a place or not a profile id");
        return -1;
    }
    std::lock_guard<std::mutex> hold(a->mu);
    size_t idx = a->n;
    for (size_t i = 0; i < a->n; i++)
        if (strcmp(a->items[i].place, place) == 0) {
            idx = i;
            break;
        }
    pair saved = idx < a->n ? a->items[idx] : pair{ { 0 }, { 0 } };
    size_t saved_n = a->n;
    int rc = 0;
    if (!profile_id) {
        if (idx == a->n)
            return 0; /* nothing was chosen there */
        a->items[idx] = a->items[--a->n];
    } else if (idx < a->n) {
        snprintf(a->items[idx].profile, sizeof a->items[idx].profile, "%s", profile_id);
    } else {
        pair *g = (pair *)realloc(a->items, (a->n + 1) * sizeof *g);
        if (!g) {
            rc = -1;
        } else {
            a->items = g;
            snprintf(g[a->n].place, sizeof g[a->n].place, "%s", place);
            snprintf(g[a->n].profile, sizeof g[a->n].profile, "%s", profile_id);
            a->n++;
        }
    }
    if (rc == 0 && persist_locked(a) != 0) {
        rc = -1;
        /* keep memory and disk in step: undo */
        if (!profile_id && idx < saved_n) {
            a->items[a->n++] = a->items[idx];
            a->items[idx] = saved;
        } else if (idx < saved_n) {
            a->items[idx] = saved;
        } else if (a->n > saved_n) {
            a->n = saved_n;
        }
    }
    if (rc != 0)
        seterr(err, errlen, "cannot save %s: %s", ASSIGNMENTS, strerror(errno));
    return rc;
}

int puddle_assignments_list(puddle_assignments *a, puddle_assignment **out, size_t *n)
{
    std::lock_guard<std::mutex> hold(a->mu);
    *out = (puddle_assignment *)malloc((a->n ? a->n : 1) * sizeof **out);
    if (!*out)
        return -1;
    for (size_t i = 0; i < a->n; i++) {
        memcpy((*out)[i].place, a->items[i].place, sizeof (*out)[i].place);
        memcpy((*out)[i].profile, a->items[i].profile, sizeof (*out)[i].profile);
    }
    *n = a->n;
    return 0;
}

int puddle_profile_for_place(puddle_profiles *p, puddle_assignments *a, const char *place, puddle_profile *out)
{
    memset(out, 0, sizeof *out);
    char id[PUDDLE_PROFILE_ID_MAX + 1];
    if (!p || !a || puddle_assignments_get(a, place, id) != 0)
        return 1;
    return puddle_profiles_get(p, id, out);
}
