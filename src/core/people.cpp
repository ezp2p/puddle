#include "core/people.h"

#include "base/json.h"
#include "base/log.h"

#include <stdarg.h>
#include <mutex>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RECORD_VERSION 2 /* 1 held one profile, which is the "dm" one */
#define COLLECTION "people"

struct puddle_people {
    puddle_storage *storage;
    puddle_storage *owned; /* the files driver opened for a data directory; NULL when handed one */
    std::mutex mu;         /* setting a profile reads the record and writes it back */
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

int puddle_people_place_valid(const char *place)
{
    return place && (!strcmp(place, PUDDLE_PEOPLE_DM) || puddle_place_group_id(place) != NULL);
}

static void entry_free(puddle_person_profile *e)
{
    free(e->profile);
    free(e->notes);
    memset(e, 0, sizeof *e);
}

void puddle_person_free(puddle_person *p)
{
    for (size_t i = 0; i < p->nprofiles; i++)
        entry_free(&p->profiles[i]);
    free(p->profiles);
    p->profiles = NULL;
    p->nprofiles = 0;
}

static char *copy_span(const char *s, size_t n)
{
    char *out = (char *)malloc(n + 1);
    if (out) {
        memcpy(out, s, n);
        out[n] = '\0';
    }
    return out;
}

static char *encode(const puddle_person *p, size_t *out_len)
{
    size_t cap = 512;
    for (size_t i = 0; i < p->nprofiles; i++)
        cap += strlen(p->profiles[i].profile) + (p->profiles[i].notes ? strlen(p->profiles[i].notes) * 6 : 0) + 256;
    char *buf = (char *)malloc(cap);
    if (!buf)
        return NULL;
    puddle_jw w;
    puddle_jw_init(&w, buf, cap);
    puddle_jw_object(&w);
    puddle_jw_num(&w, "v", RECORD_VERSION);
    puddle_jw_str(&w, "id", p->id);
    puddle_jw_num(&w, "first_seen", p->first_seen);
    puddle_jw_key(&w, "profiles");
    puddle_jw_array(&w);
    for (size_t i = 0; i < p->nprofiles; i++) {
        const puddle_person_profile *e = &p->profiles[i];
        puddle_jw_object(&w);
        puddle_jw_str(&w, "place", e->place);
        puddle_jw_key(&w, "profile");
        puddle_jw_raw(&w, e->profile, strlen(e->profile));
        puddle_jw_num(&w, "at", e->at);
        if (e->notes && *e->notes)
            puddle_jw_str(&w, "notes", e->notes);
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

static int add_entry(puddle_person *p, const char *place, puddle_span profile, int64_t at, const char *notes)
{
    if (!puddle_people_place_valid(place) || p->nprofiles >= PUDDLE_PEOPLE_MAX_PLACES || profile.len > PUDDLE_PROFILE_JSON_MAX ||
        !puddle_json_valid_object(profile.p, profile.len))
        return -1;
    puddle_person_profile *g = (puddle_person_profile *)realloc(p->profiles, (p->nprofiles + 1) * sizeof *g);
    if (!g)
        return -1;
    p->profiles = g;
    puddle_person_profile *e = &g[p->nprofiles];
    memset(e, 0, sizeof *e);
    snprintf(e->place, sizeof e->place, "%s", place);
    e->profile = copy_span(profile.p, profile.len);
    e->notes = notes && *notes ? strdup(notes) : NULL;
    e->at = at;
    if (!e->profile || (notes && *notes && !e->notes)) {
        entry_free(e);
        return -1;
    }
    p->nprofiles++;
    return 0;
}

static int decode(const char *json, size_t len, const char *id, puddle_person *out)
{
    memset(out, 0, sizeof *out);
    snprintf(out->id, sizeof out->id, "%s", id);
    puddle_span v;
    int64_t version;
    char got[PUDDLE_USER_ID_LEN + 2];
    if (puddle_json_member(json, len, "v", &v) != 0 || puddle_json_int64(v, &version) != 0 || version < 1 || version > RECORD_VERSION)
        return -1;
    if (puddle_json_member(json, len, "id", &v) != 0 || puddle_json_string(v, got, sizeof got) != 0 || strcmp(got, id) != 0)
        return -1;
    if (puddle_json_member(json, len, "first_seen", &v) != 0 || puddle_json_int64(v, &out->first_seen) != 0)
        return -1;
    if (version == 1) { /* one profile: the one they showed us directly */
        int rc = puddle_json_member(json, len, "profile", &v);
        if (rc < 0)
            return -1;
        if (rc == 0) {
            puddle_span at;
            int64_t when;
            if (puddle_json_member(json, len, "profile_at", &at) != 0 || puddle_json_int64(at, &when) != 0 ||
                add_entry(out, PUDDLE_PEOPLE_DM, v, when, NULL) != 0)
                return -1;
        }
        return 0;
    }
    puddle_span arr, it;
    if (puddle_json_member(json, len, "profiles", &arr) != 0)
        return -1;
    size_t cur = 0;
    int r;
    while ((r = puddle_json_array_next(arr, &cur, &it)) == 1) {
        puddle_span pv, prof, at, nv;
        char place[PUDDLE_PLACE_MAX], notes[PUDDLE_PROFILE_NOTES_MAX] = "";
        int64_t when;
        if (puddle_json_member(it.p, it.len, "place", &pv) != 0 || puddle_json_string(pv, place, sizeof place) != 0 ||
            puddle_json_member(it.p, it.len, "profile", &prof) != 0 || puddle_json_member(it.p, it.len, "at", &at) != 0 ||
            puddle_json_int64(at, &when) != 0)
            return -1;
        int nrc = puddle_json_member(it.p, it.len, "notes", &nv);
        if (nrc < 0 || (nrc == 0 && puddle_json_string(nv, notes, sizeof notes) != 0))
            return -1;
        if (add_entry(out, place, prof, when, notes) != 0)
            return -1;
    }
    return r < 0 ? -1 : 0;
}

/* caller holds the lock: 0 found, 1 none, -1 */
static int read_locked(puddle_people *p, const char *id, puddle_person *out)
{
    memset(out, 0, sizeof *out);
    char *text;
    size_t len;
    int rc = puddle_record_get(p->storage, COLLECTION, id, &text, &len);
    if (rc != 0)
        return rc;
    rc = decode(text, len, id, out);
    free(text);
    if (rc != 0) {
        puddle_person_free(out);
        puddle_log(PUDDLE_LOG_WARN, "damaged person record %s", id);
        return -1;
    }
    return 0;
}

static int write_locked(puddle_people *p, const puddle_person *rec)
{
    size_t n;
    char *text = encode(rec, &n);
    if (!text)
        return -1;
    int rc = puddle_record_put(p->storage, COLLECTION, rec->id, text, n);
    free(text);
    return rc == 0 ? 0 : -1;
}

puddle_people *puddle_people_open(const char *data_dir, char *err, size_t errlen)
{
    puddle_storage *st = puddle_storage_open_files(data_dir, err, errlen);
    if (!st)
        return NULL;
    puddle_people *p = puddle_people_open_with(st, err, errlen);
    if (!p) {
        puddle_storage_close(st);
        return NULL;
    }
    p->owned = st;
    return p;
}

puddle_people *puddle_people_open_with(puddle_storage *storage, char *err, size_t errlen)
{
    puddle_people *p = new (std::nothrow) puddle_people();
    if (!p) {
        seterr(err, errlen, "out of memory");
        return NULL;
    }
    p->storage = storage;
    return p;
}

void puddle_people_close(puddle_people *p)
{
    if (!p)
        return;
    puddle_storage_close(p->owned);
    delete p;
}

static long find_place(const puddle_person *rec, const char *place)
{
    for (size_t i = 0; i < rec->nprofiles; i++)
        if (!strcmp(rec->profiles[i].place, place))
            return (long)i;
    return -1;
}

static void drop_place(puddle_person *rec, long at)
{
    entry_free(&rec->profiles[at]);
    memmove(&rec->profiles[at], &rec->profiles[at + 1], (rec->nprofiles - (size_t)at - 1) * sizeof *rec->profiles);
    rec->nprofiles--;
}

int puddle_people_set_profile(puddle_people *p, const char *id, const char *place, const char *json, size_t len, const char *notes,
                              int64_t now_ms)
{
    if (!puddle_id_valid(id) || !puddle_people_place_valid(place) || !json || len > PUDDLE_PROFILE_JSON_MAX ||
        !puddle_json_valid_object(json, len))
        return -1;
    std::lock_guard<std::mutex> hold(p->mu);
    puddle_person rec;
    if (read_locked(p, id, &rec) != 0) { /* none yet, or a damaged one, which is replaced: what they send now is all there is to know */
        memset(&rec, 0, sizeof rec);
        snprintf(rec.id, sizeof rec.id, "%s", id);
        rec.first_seen = now_ms;
    }
    long at = find_place(&rec, place);
    if (at >= 0) /* the place's old one goes, and the new one goes last */
        drop_place(&rec, at);
    int rc = add_entry(&rec, place, puddle_span{ json, len }, now_ms, notes) == 0 ? write_locked(p, &rec) : -1;
    puddle_person_free(&rec);
    return rc;
}

int puddle_people_clear_profile(puddle_people *p, const char *id, const char *place)
{
    if (!puddle_id_valid(id) || !puddle_people_place_valid(place))
        return -1;
    std::lock_guard<std::mutex> hold(p->mu);
    puddle_person rec;
    int rc = read_locked(p, id, &rec);
    if (rc != 0)
        return rc == 1 ? 0 : -1;
    long at = find_place(&rec, place);
    if (at >= 0) {
        drop_place(&rec, at);
        rc = write_locked(p, &rec);
    }
    puddle_person_free(&rec);
    return rc;
}

int puddle_people_get(puddle_people *p, const char *id, puddle_person *out)
{
    memset(out, 0, sizeof *out);
    if (!puddle_id_valid(id))
        return -1;
    std::lock_guard<std::mutex> hold(p->mu);
    return read_locked(p, id, out);
}

int puddle_people_profile(puddle_people *p, const char *id, const char *place, char **json, size_t *len)
{
    *json = NULL;
    *len = 0;
    puddle_person rec;
    int rc = puddle_people_get(p, id, &rec);
    if (rc != 0)
        return rc;
    long at = place ? find_place(&rec, place) : -1;
    if (at >= 0) {
        *json = rec.profiles[at].profile;
        *len = strlen(*json);
        rec.profiles[at].profile = NULL; /* handed over */
    }
    puddle_person_free(&rec);
    return at >= 0 ? 0 : 1;
}
