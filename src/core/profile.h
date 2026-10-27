#ifndef PUDDLE_PROFILE_H
#define PUDDLE_PROFILE_H

/* Profiles: how the owner appears to others (docs/profiles.md). The owner has several (work, friends, family...), each a
 * YAML file in <data_dir>/profiles/<id>.yaml, and chooses which one each place sees: a direct conversation with someone,
 * a group, later a Pond. Nothing is shown anywhere the owner has not chosen a profile for: that choice is their consent
 * for it to be sent there. Images live in <data_dir>/media/<sha256> and a profile names them by hash. */

#include "core/config.h"
#include "storage/storage.h"
#include "storage/media.h" /* the image size limit, and recognising an image */
#include "base/json.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PUDDLE_PROFILE_ID_MAX 32
#define PUDDLE_MAX_PROFILES 32
#define PUDDLE_MAX_GRADIENT_STOPS 5
#define PUDDLE_MAX_PROFILE_LINKS 10

/* Limits, in characters (not bytes). */
#define PUDDLE_PROFILE_NAME_MAX 64
#define PUDDLE_PROFILE_BLURB_MAX 160
#define PUDDLE_PROFILE_ABOUT_MAX 2000
#define PUDDLE_PROFILE_PRONOUNS_MAX 32
#define PUDDLE_PROFILE_LINK_TEXT_MAX 64
#define PUDDLE_PROFILE_LINK_URL_MAX 512 /* bytes: an address is ASCII */

/* A profile as JSON (what is sent, and what we keep of what others send) is never larger than this: an `about` of 2000
 * four-byte characters, ten links, and the rest. And the notes about one received profile fit in this. */
#define PUDDLE_PROFILE_JSON_MAX 24576
#define PUDDLE_PROFILE_NOTES_MAX 1024

typedef struct {
    char hash[65]; /* SHA-256 of the file, hex; empty string = no image */
    char mime[16]; /* image/png, image/jpeg, image/gif or image/webp */
    uint64_t size;
} puddle_media;

typedef enum { PUDDLE_COLOR_NONE, PUDDLE_COLOR_SOLID, PUDDLE_COLOR_GRADIENT } puddle_color_kind;

typedef struct {
    puddle_color_kind kind;
    char stops[PUDDLE_MAX_GRADIENT_STOPS][8]; /* "#rrggbb"; one stop for SOLID */
    int nstops;
    int angle; /* degrees, GRADIENT only */
} puddle_color;

/* Somewhere else the owner can be found: a game tag, an account, a page. `url` may be NULL ("Battle.net: Ali#1234"). */
typedef struct {
    char *text;
    char *url;
} puddle_profile_link;

typedef struct {
    char id[PUDDLE_PROFILE_ID_MAX + 1]; /* private to the owner; never sent to anyone */
    char *display_name;                 /* the name shown; required */
    char *blurb;    /* short bio, may be NULL */
    char *about;    /* long bio, may span lines, may be NULL */
    char *pronouns; /* may be NULL */
    puddle_color color; /* the colour of their name */
    puddle_media avatar, banner, background;
    puddle_profile_link links[PUDDLE_MAX_PROFILE_LINKS];
    int nlinks;
} puddle_profile;

void puddle_profile_free(puddle_profile *p);              /* what it owns; the struct is the caller's */
int puddle_profile_copy(puddle_profile *dst, const puddle_profile *src); /* 0, or -1 (out of memory; dst left empty) */

/* Is `id` a profile id: 1 to 32 of a-z, 0-9, '-' and '_'? */
int puddle_profile_id_valid(const char *id);

/* Does `p` keep every rule (a display name; text within its limits, valid UTF-8, no control characters except line breaks
 * in `about`; colours "#rrggbb"; 2 to 5 stops and an angle of 0 to 360 for a gradient; links with text and, if any, an
 * http or https address)? 0, or -1 with what is wrong in `err` ("blurb is 170 characters; the limit is 160"). */
int puddle_profile_check(const puddle_profile *p, char *err, size_t errlen);

/* Write a profile's fields as members of the JSON object `w` has open, in the form they take on the wire and in the owner
 * API: display_name, then blurb, about, pronouns, color ({"colors":[..],"angle":n}), avatar, banner, background as
 * {"hash","mime","size"} and links ([{"text","url"}]), each only if there is one. Never the id. */
void puddle_profile_write_fields(puddle_jw *w, const puddle_profile *p);

/* ---- the owner's profiles ---- */

typedef struct puddle_profiles puddle_profiles;

/* Read every <data_dir>/profiles/<id>.yaml. A file with a mistake in it is left out with a warning naming the line, so one bad
 * edit cannot stop the Puddle; an image that is missing is dropped from its profile the same way.
 *
 * The first time (no profiles folder yet), the profiles in the files `cfg->profile_files` names (the `profiles:` setting
 * Puddles used before) are imported: each becomes a file here. The first of them was then shown to everyone the owner had
 * not chosen one for, so its id is copied to `old_default` for the caller to keep showing it where it was already seen;
 * otherwise `old_default` is "". With nothing to import, one profile named after the OS account is made, shown to no one.
 * NULL with a message in `err` if the folder cannot be made or an imported file cannot be read. */
puddle_profiles *puddle_profiles_open(const puddle_config *cfg, char old_default[PUDDLE_PROFILE_ID_MAX + 1], char *err, size_t errlen);
void puddle_profiles_close(puddle_profiles *p);

/* The ids, sorted, into `ids` (up to `cap`). Returns how many there are. */
size_t puddle_profiles_ids(puddle_profiles *p, char (*ids)[PUDDLE_PROFILE_ID_MAX + 1], size_t cap);

/* 0 and a copy in `out` (free it with puddle_profile_free), 1 no such profile. */
int puddle_profiles_get(puddle_profiles *p, const char *id, puddle_profile *out);
int puddle_profiles_has(puddle_profiles *p, const char *id);

/* Keep `pr` as its file, creating it (`create`, which fails if the id is taken) or replacing it (which fails if there is no
 * such profile). Its images must already be in <data_dir>/media (puddle_profiles_add_image). 0, or -1 with a short code in
 * `*code` ("bad_request", "profile_exists", "no_such_profile", "too_many_profiles", "no_such_image", "internal") and what
 * is wrong in `err`. */
int puddle_profiles_save(puddle_profiles *p, const puddle_profile *pr, int create, const char **code, char *err, size_t errlen);

/* Delete the profile and its file. 0, 1 no such profile, -1. */
int puddle_profiles_remove(puddle_profiles *p, const char *id);

/* Keep an image the owner gave us (its bytes) in <data_dir>/media, and describe it in `out`. 0, or -1 with the reason in
 * `err` (not a png, jpg, gif or webp; empty; over the size limit; cannot be written). */
int puddle_profiles_add_image(puddle_profiles *p, const unsigned char *data, size_t len, puddle_media *out, char *err, size_t errlen);

/* Fill in `m` (its mime and size) from the image with hash m->hash that we hold. 0, or 1 if we do not hold it. */
int puddle_profiles_image(puddle_profiles *p, puddle_media *m);

/* ---- where each profile is shown ---- */

/* A place is where a profile can be shown: "dm:<user id>" (a direct conversation, and everything else between two Puddles
 * outside groups) or "group:<group id>". Ponds will add "pond:<id>". */
#define PUDDLE_PLACE_MAX 64
int puddle_place_dm(const char *user_id, char out[PUDDLE_PLACE_MAX]);   /* 0, or -1 if the id is not valid */
int puddle_place_group(const char *group_id, char out[PUDDLE_PLACE_MAX]);
int puddle_place_valid(const char *place);
/* For "group:<id>", the group id (else NULL); for "dm:<id>", the user id (else NULL). Pointers into `place`. */
const char *puddle_place_group_id(const char *place);
const char *puddle_place_dm_id(const char *place);

/* Which profile each place is shown: the owner's choices. Kept in <data_dir>/state/profile-places.tsv (the choices made
 * for streams before groups had their own, in stream-profiles.tsv, are read from there the first time). */
typedef struct puddle_assignments puddle_assignments;
puddle_assignments *puddle_assignments_open(const char *data_dir, char *err, size_t errlen);
puddle_assignments *puddle_assignments_open_with(puddle_storage *storage, char *err, size_t errlen); /* storage outlives it */
void puddle_assignments_close(puddle_assignments *a);
/* 0 found (id copied to `out`), 1 nothing chosen */
int puddle_assignments_get(puddle_assignments *a, const char *place, char out[PUDDLE_PROFILE_ID_MAX + 1]);
/* Show `profile_id` at `place`; NULL takes the choice back (nothing is shown there). 0 or -1. */
int puddle_assignments_set(puddle_assignments *a, const char *place, const char *profile_id, char *err, size_t errlen);

typedef struct {
    char place[PUDDLE_PLACE_MAX];
    char profile[PUDDLE_PROFILE_ID_MAX + 1];
} puddle_assignment;
/* Every choice (malloc'd; free() it), and how many. 0 or -1. */
int puddle_assignments_list(puddle_assignments *a, puddle_assignment **out, size_t *n);

/* The profile shown at `place`: 0 and a copy in `out` (free with puddle_profile_free), 1 none (nothing chosen there, or the
 * chosen profile is gone). */
int puddle_profile_for_place(puddle_profiles *p, puddle_assignments *a, const char *place, puddle_profile *out);

#ifdef __cplusplus
}
#endif
#endif
