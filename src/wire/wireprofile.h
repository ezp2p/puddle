#ifndef PUDDLE_WIREPROFILE_H
#define PUDDLE_WIREPROFILE_H

/* A profile as another Puddle sends it (api/federation.yaml, "Profile"), and what we make of it. Nothing a stranger sends is
 * kept as sent: it is rebuilt from the fields we understand, each checked and cleaned, so what is stored and shown is always well
 * formed. A field that is wrong is left out (or, for text that is too long, cut to its limit) and a short note says so; the
 * rest of the profile is kept. Only something that is not a JSON object at all is refused. */

#include "core/profile.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The largest a rebuilt profile can be, and room for the notes about one. */
#define PUDDLE_WIRE_PROFILE_MAX PUDDLE_PROFILE_JSON_MAX
#define PUDDLE_WIRE_NOTES_MAX PUDDLE_PROFILE_NOTES_MAX

/* Rebuild the profile object `json` into `out`: display_name (64 characters), blurb (160), about (2000, may hold newlines),
 * pronouns (32), color ({"colors":[one to five "#rrggbb"],"angle":0..360}), avatar, banner and background as
 * {"hash","mime","size"} (a SHA-256 in hex, an image type we know, a size up to the image cap), and links (up to 10 of
 * {"text": 64 characters, "url": an http or https address of up to 512 bytes, optional}). Text is cleaned (no control
 * characters, no direction overrides, bad UTF-8 replaced). Fields we do not know are skipped without a word, so a newer
 * Puddle can send more.
 *
 * What was wrong goes into `notes` as one line per field ("color: \"#12345g\" is not a colour; left out"), NUL-terminated,
 * cut short if there is too much; "" if nothing was. `notes` may be NULL.
 *
 * 0, or -1 if `json` is not a JSON object or what it comes to does not fit in `cap`. */
int puddle_profile_wire_clean(const char *json, size_t len, char *out, size_t cap, char *notes, size_t notes_cap);

typedef struct {
    char hash[65];
    char mime[16];
    uint64_t size;
} puddle_media_ref;

/* The images a (cleaned) profile refers to: up to three. Returns how many, written to `refs`. */
size_t puddle_profile_wire_media(const char *json, size_t len, puddle_media_ref refs[3]);

#ifdef __cplusplus
}
#endif
#endif
