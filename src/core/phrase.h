#ifndef PUDDLE_PHRASE_H
#define PUDDLE_PHRASE_H

/* The recovery phrase: eight groups of four characters, e.g. 7QK4-M2XB-9TDW-HC5R-ZP3E-A8VN-4YJF-X2K7.
 *
 * Seven groups (28 characters, 140 bits) are random; the eighth is a checksum. The alphabet is
 * Crockford's base32: digits and letters without I, L, O, U, so nothing looks like anything else.
 * Typing is forgiving (any case, spaces or hyphens or none, O read as 0, I and L as 1), but a mistake
 * is caught: a single wrong character, or two neighbours swapped, is always detected, and any other
 * slip is missed one time in a million.
 *
 * All the keys of one generation are derived from the phrase (docs/roadmap.md, phase 1). A phrase is
 * shown once and never stored. */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PUDDLE_PHRASE_LEN 39 /* "XXXX-XXXX-XXXX-XXXX-XXXX-XXXX-XXXX-XXXX" */

enum {
    PUDDLE_PHRASE_OK = 0,
    PUDDLE_PHRASE_BAD_LENGTH = -1,   /* not 32 characters once separators are ignored */
    PUDDLE_PHRASE_BAD_CHARACTER = -2,
    PUDDLE_PHRASE_BAD_CHECKSUM = -3  /* a typo somewhere */
};

/* A new random phrase, in canonical form (upper case, hyphens, NUL-terminated). 0 or -1. */
int puddle_phrase_generate(char out[PUDDLE_PHRASE_LEN + 1]);

/* Read a phrase as a person typed it and give its canonical form, or say what is wrong with it.
 * `canonical` may be NULL to only check. */
int puddle_phrase_parse(const char *text, char canonical[PUDDLE_PHRASE_LEN + 1]);

/* The 32-byte secret all of this generation's keys come from. Takes any form that parses. The same
 * phrase always gives the same seed. 0, or the parse error. */
int puddle_phrase_seed(const char *text, unsigned char seed[32]);

#ifdef __cplusplus
}
#endif
#endif
