#ifndef PUDDLE_PEOPLE_H
#define PUDDLE_PEOPLE_H

/* Someone this Puddle knows of, and the profiles their Puddle sent us (test_structure/model/records/person.yaml). One
 * record per person, collection "people", keyed by their user id.
 *
 * A person can show us a different profile in each place we share: one for our direct conversation ("dm") and one for
 * each group ("group:<group id>"; Ponds will add "pond:<id>"). Each arrives from their Puddle (with their request for a
 * stream, their answer to ours, or POST /stream/v1/profile whenever they change it), and whichever comes last for a place
 * replaces what we held for it. They may also take one back, and then we forget it. What they sent was cleaned on arrival
 * (wire/wireprofile.h); the notes say what had to be left out. */

#include "core/profile.h"
#include "core/link.h" /* the user id length, and checking one */
#include "storage/storage.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PUDDLE_PEOPLE_DM "dm" /* the place of the direct conversation, as a person's record names it */
#define PUDDLE_PEOPLE_MAX_PLACES 64

/* Is `place` one a person's record can hold: "dm" or "group:<group id>"? */
int puddle_people_place_valid(const char *place);

typedef struct {
    char place[PUDDLE_PLACE_MAX];
    char *profile; /* the JSON object, cleaned */
    int64_t at;    /* when it arrived */
    char *notes;   /* what was wrong with what they sent, one line each; NULL if nothing */
} puddle_person_profile;

typedef struct {
    char id[PUDDLE_USER_ID_LEN + 1];
    int64_t first_seen; /* when we first heard of them */
    puddle_person_profile *profiles;
    size_t nprofiles;
} puddle_person;

void puddle_person_free(puddle_person *p); /* frees what the record owns; the struct itself is the caller's */

typedef struct puddle_people puddle_people;

/* The first form keeps them as files under `data_dir` (<data_dir>/state/people/<id>.json); the second uses `storage`,
 * which must outlive the store. NULL with a message in `err`. */
puddle_people *puddle_people_open(const char *data_dir, char *err, size_t errlen);
puddle_people *puddle_people_open_with(puddle_storage *storage, char *err, size_t errlen);
void puddle_people_close(puddle_people *p);

/* Keep `json` (a JSON object, at most PUDDLE_WIRE_PROFILE_MAX) as the profile they show us at `place`, arrived at `now_ms`,
 * with `notes` (NULL or "" for none), creating their record if there is none. 0, or -1 if the id, the place or the profile
 * is not valid, or it cannot be written. */
int puddle_people_set_profile(puddle_people *p, const char *id, const char *place, const char *json, size_t len, const char *notes,
                              int64_t now_ms);

/* Forget the profile they showed us at `place` (they took it back, or we left the place). 0 (also if there was none) or -1. */
int puddle_people_clear_profile(puddle_people *p, const char *id, const char *place);

/* 0 found (free it with puddle_person_free), 1 no record, -1 `id` is not valid or the record cannot be read. */
int puddle_people_get(puddle_people *p, const char *id, puddle_person *out);

/* The profile they show us at `place` alone: 0 and a malloc'd NUL-terminated copy, 1 none, -1 on error. */
int puddle_people_profile(puddle_people *p, const char *id, const char *place, char **json, size_t *len);

#ifdef __cplusplus
}
#endif
#endif
