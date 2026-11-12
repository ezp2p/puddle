#include "core/people.h"

#include "core/streams.h"

#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static int rm_cb(const char *p, const struct stat *sb, int t, struct FTW *f) { (void)sb; (void)t; (void)f; remove(p); return 0; }

#define T0 1760000000000LL
#define ALICE "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define BOB   "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbq"

#define GROUP "gggggggggggggggggggggggggggggggggggggggggggggggggggq"
#define GPLACE "group:" GROUP

/* what any people store does, whichever storage is behind it */
static void contract(puddle_people *p)
{
    puddle_person who;
    char *json;
    size_t len;
    CHECK(puddle_people_get(p, ALICE, &who) == 1);
    CHECK(puddle_people_profile(p, ALICE, PUDDLE_PEOPLE_DM, &json, &len) == 1 && json == NULL);
    CHECK(puddle_people_get(p, "../etc", &who) == -1);
    CHECK(puddle_people_set_profile(p, "not an id", PUDDLE_PEOPLE_DM, "{}", 2, NULL, T0) == -1);
    CHECK(puddle_people_set_profile(p, ALICE, "nowhere", "{}", 2, NULL, T0) == -1);       /* not a place */
    CHECK(puddle_people_set_profile(p, ALICE, "dm:" ALICE, "{}", 2, NULL, T0) == -1);     /* a record names its own dm "dm" */

    const char *a1 = "{\"display_name\":\"Alice\",\"blurb\":\"hi\"}";
    CHECK(puddle_people_set_profile(p, ALICE, PUDDLE_PEOPLE_DM, a1, strlen(a1), "", T0) == 0);
    CHECK(puddle_people_get(p, ALICE, &who) == 0 && !strcmp(who.id, ALICE) && who.first_seen == T0 && who.nprofiles == 1 &&
          !strcmp(who.profiles[0].place, "dm") && who.profiles[0].at == T0 && !strcmp(who.profiles[0].profile, a1) && !who.profiles[0].notes);
    puddle_person_free(&who);

    /* a group's is kept beside it, with the notes on what was left out */
    const char *g1 = "{\"display_name\":\"Ali the raider\"}";
    CHECK(puddle_people_set_profile(p, ALICE, GPLACE, g1, strlen(g1), "color: \"#12345g\" is not a colour; left out", T0 + 10) == 0);
    CHECK(puddle_people_profile(p, ALICE, GPLACE, &json, &len) == 0 && !strcmp(json, g1));
    free(json);
    CHECK(puddle_people_profile(p, ALICE, PUDDLE_PEOPLE_DM, &json, &len) == 0 && !strcmp(json, a1)); /* the direct one is untouched */
    free(json);
    CHECK(puddle_people_get(p, ALICE, &who) == 0 && who.nprofiles == 2 && who.profiles[1].notes &&
          strstr(who.profiles[1].notes, "#12345g"));
    puddle_person_free(&who);

    /* whichever comes last for a place replaces it; when we first heard of them stays */
    const char *a2 = "{\"display_name\":\"Alice at work\"}";
    CHECK(puddle_people_set_profile(p, ALICE, PUDDLE_PEOPLE_DM, a2, strlen(a2), NULL, T0 + 50) == 0);
    CHECK(puddle_people_profile(p, ALICE, PUDDLE_PEOPLE_DM, &json, &len) == 0 && len == strlen(a2) && !strcmp(json, a2));
    free(json);
    CHECK(puddle_people_get(p, ALICE, &who) == 0 && who.first_seen == T0 && who.nprofiles == 2);
    puddle_person_free(&who);

    /* not a JSON object, or too large: refused, and what was there stands */
    CHECK(puddle_people_set_profile(p, ALICE, PUDDLE_PEOPLE_DM, "[1]", 3, NULL, T0 + 60) == -1);
    CHECK(puddle_people_set_profile(p, ALICE, PUDDLE_PEOPLE_DM, "{\"a\":", 5, NULL, T0 + 60) == -1);
    char *huge = malloc(PUDDLE_PROFILE_JSON_MAX + 16);
    memset(huge, ' ', PUDDLE_PROFILE_JSON_MAX + 15);
    huge[0] = '{';
    huge[PUDDLE_PROFILE_JSON_MAX + 14] = '}';
    CHECK(puddle_people_set_profile(p, ALICE, PUDDLE_PEOPLE_DM, huge, PUDDLE_PROFILE_JSON_MAX + 15, NULL, T0 + 60) == -1);
    free(huge);
    CHECK(puddle_people_profile(p, ALICE, PUDDLE_PEOPLE_DM, &json, &len) == 0 && !strcmp(json, a2));
    free(json);

    /* taken back: forgotten for that place only */
    CHECK(puddle_people_clear_profile(p, ALICE, GPLACE) == 0);
    CHECK(puddle_people_profile(p, ALICE, GPLACE, &json, &len) == 1);
    CHECK(puddle_people_profile(p, ALICE, PUDDLE_PEOPLE_DM, &json, &len) == 0);
    free(json);
    CHECK(puddle_people_clear_profile(p, ALICE, GPLACE) == 0); /* again: nothing to do */
    CHECK(puddle_people_clear_profile(p, BOB, PUDDLE_PEOPLE_DM) == 0); /* someone we never heard of */

    CHECK(puddle_people_get(p, BOB, &who) == 1); /* one person's record is theirs alone */
}

/* a stream record as one was written before profiles had records of their own */
static void put_old_stream(puddle_storage *st, const char *id, const char *profile)
{
    char rec[1024];
    int n = snprintf(rec, sizeof rec,
                     "{\"v\":1,\"id\":\"%s\",\"state\":\"active\",\"created\":%lld,\"updated\":%lld,\"note\":\"\","
                     "\"addresses\":\"203.0.113.7:8443\",\"addr_seq\":0%s%s}",
                     id, T0, T0 + 7, profile ? ",\"profile\":" : "", profile ? profile : "");
    CHECK(puddle_record_put(st, "streams", id, rec, (size_t)n) == 0);
}

static int keep(void *user, const char *id, const char *json, size_t len, int64_t updated_ms)
{
    return puddle_people_set_profile((puddle_people *)user, id, PUDDLE_PEOPLE_DM, json, len, NULL, updated_ms);
}

int main(void)
{
    char err[300];

    /* memory */
    {
        puddle_storage *mem = puddle_storage_open_memory();
        puddle_people *p = puddle_people_open_with(mem, err, sizeof err);
        CHECK(p != NULL);
        contract(p);
        puddle_people_close(p);
        p = puddle_people_open_with(mem, err, sizeof err); /* kept by the storage, not the store */
        char *json;
        size_t len;
        CHECK(puddle_people_profile(p, ALICE, PUDDLE_PEOPLE_DM, &json, &len) == 0 && strstr(json, "Alice at work"));
        free(json);
        puddle_people_close(p);
        puddle_storage_close(mem);
    }

    /* files: <data>/state/people/<id>.json */
    char root[] = "/tmp/puddle-people-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    {
        puddle_people *p = puddle_people_open(root, err, sizeof err);
        CHECK(p != NULL);
        contract(p);
        puddle_people_close(p);
        char path[600];
        snprintf(path, sizeof path, "%s/state/people/%s.json", root, ALICE);
        CHECK(access(path, F_OK) == 0);
        /* a damaged record is reported, and the next profile they send replaces it */
        FILE *f = fopen(path, "w");
        fputs("{\"v\":1,", f);
        fclose(f);
        p = puddle_people_open(root, err, sizeof err);
        puddle_person who;
        CHECK(puddle_people_get(p, ALICE, &who) == -1);
        CHECK(puddle_people_set_profile(p, ALICE, PUDDLE_PEOPLE_DM, "{}", 2, NULL, T0 + 90) == 0);
        CHECK(puddle_people_get(p, ALICE, &who) == 0 && who.first_seen == T0 + 90 && who.nprofiles == 1 && !strcmp(who.profiles[0].profile, "{}"));
        puddle_person_free(&who);
        puddle_people_close(p);
    }

    /* moving profiles out of stream records written before people had their own */
    {
        puddle_storage *mem = puddle_storage_open_memory();
        put_old_stream(mem, ALICE, "{\"display_name\":\"Alice\"}");
        put_old_stream(mem, BOB, NULL);
        puddle_streams *ss = puddle_streams_open_with(mem, err, sizeof err);
        puddle_people *p = puddle_people_open_with(mem, err, sizeof err);
        CHECK(ss && p);
        CHECK(puddle_streams_take_profiles(ss, keep, p) == 1);
        puddle_person who;
        CHECK(puddle_people_get(p, ALICE, &who) == 0 && who.nprofiles == 1 && !strcmp(who.profiles[0].place, "dm") &&
              !strcmp(who.profiles[0].profile, "{\"display_name\":\"Alice\"}") && who.profiles[0].at == T0 + 7);
        puddle_person_free(&who);
        CHECK(puddle_people_get(p, BOB, &who) == 1);
        char *rec;
        size_t len;
        CHECK(puddle_record_get(mem, "streams", ALICE, &rec, &len) == 0 && !strstr(rec, "profile")); /* gone from the stream */
        free(rec);
        puddle_stream st;
        CHECK(puddle_streams_get(ss, ALICE, &st) == 0 && st.state == PUDDLE_STREAM_ACTIVE && st.updated_ms == T0 + 7 && st.naddr == 1);
        puddle_stream_free(&st);
        CHECK(puddle_streams_take_profiles(ss, keep, p) == 0); /* once only */
        puddle_people_close(p);
        puddle_streams_close(ss);
        puddle_storage_close(mem);
    }

    /* a record from before profiles had places: its one profile is the direct one */
    {
        puddle_storage *mem = puddle_storage_open_memory();
        const char *v1 = "{\"v\":1,\"id\":\"" ALICE "\",\"first_seen\":5,\"profile\":{\"display_name\":\"Old\"},\"profile_at\":6}";
        CHECK(puddle_record_put(mem, "people", ALICE, v1, strlen(v1)) == 0);
        puddle_people *p = puddle_people_open_with(mem, err, sizeof err);
        puddle_person who;
        CHECK(puddle_people_get(p, ALICE, &who) == 0 && who.first_seen == 5 && who.nprofiles == 1 && !strcmp(who.profiles[0].place, "dm") &&
              who.profiles[0].at == 6 && strstr(who.profiles[0].profile, "Old"));
        puddle_person_free(&who);
        CHECK(puddle_people_set_profile(p, ALICE, GPLACE, "{}", 2, NULL, 9) == 0); /* written back in today's form, keeping it */
        char *rec;
        size_t len;
        CHECK(puddle_record_get(mem, "people", ALICE, &rec, &len) == 0 && strstr(rec, "\"v\":2") && strstr(rec, "Old"));
        free(rec);
        puddle_people_close(p);
        puddle_storage_close(mem);
    }

    nftw(root, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    if (failures)
        fprintf(stderr, "%d failure%s\n", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
