#ifndef PUDDLE_JSON_H
#define PUDDLE_JSON_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A slice of the input. Strings keep their quotes; nothing is copied. */
typedef struct {
    const char *p;
    size_t len;
} puddle_span;

/* 1 if [json, json+len) is exactly one valid JSON object (nesting limited to 64), else 0. */
int puddle_json_valid_object(const char *json, size_t len);

/* Look up a top-level member of a JSON object and return the raw bytes of its value, exactly as
 * written (this is what event ids hash). Validates the whole object while scanning.
 *   0  found
 *   1  not found
 *  -1  malformed JSON, or the key appears more than once (ambiguous, so rejected) */
int puddle_json_member(const char *json, size_t len, const char *key, puddle_span *val);

/* Walk the elements of a JSON array. `array` is the array's own text (as puddle_json_member returns it), `*cursor`
 * starts at 0. Each call gives the next element, as its exact text, in *item. 1 an element, 0 the end, -1 malformed
 * (or not an array). Elements are not checked beyond being well-formed values: look inside with puddle_json_member
 * (objects) or puddle_json_string and puddle_json_int64. */
int puddle_json_array_next(puddle_span array, size_t *cursor, puddle_span *item);

/* Value helpers. 0 on success, -1 if the value is the wrong kind, out of range, or too long. */
int puddle_json_int64(puddle_span v, int64_t *out);
int puddle_json_string(puddle_span v, char *out, size_t cap); /* unescaped UTF-8, NUL-terminated */

/* Writer. Builds one JSON value into a caller's buffer, with no allocation. Commas, quotes and escaping are
 * handled here, so a handler only says what goes in. Mistakes (a value with no key in an object, a key in an
 * array, unbalanced ends, nesting past 64, a full buffer) are remembered, and puddle_jw_finish reports them
 * once, so a handler checks one result rather than every call.
 *
 * Strings are written as valid UTF-8 whatever they hold: a byte that is not valid UTF-8 (a peer's display
 * name can contain anything) becomes U+FFFD, and control characters are escaped. */
typedef struct {
    char *buf;
    size_t cap, len;
    int err;
    int depth;
    int key_pending, done;
    char kind[64];  /* 'o' or 'a' per level */
    char count[64]; /* 0 = empty so far, 1 = has members */
} puddle_jw;

void puddle_jw_init(puddle_jw *w, char *buf, size_t cap);
void puddle_jw_object(puddle_jw *w); /* open {  (as a value) */
void puddle_jw_end_object(puddle_jw *w);
void puddle_jw_array(puddle_jw *w);  /* open [  (as a value) */
void puddle_jw_end_array(puddle_jw *w);
void puddle_jw_key(puddle_jw *w, const char *key); /* inside an object; the next call is its value */
void puddle_jw_string(puddle_jw *w, const char *s);
void puddle_jw_string_n(puddle_jw *w, const char *s, size_t n);
void puddle_jw_int(puddle_jw *w, int64_t v);
void puddle_jw_uint(puddle_jw *w, uint64_t v);
void puddle_jw_bool(puddle_jw *w, int v);
void puddle_jw_null(puddle_jw *w);
void puddle_jw_raw(puddle_jw *w, const char *json, size_t n); /* already-valid JSON, e.g. a stored event: the caller vouches for it */
/* Shorthand for key + value. */
void puddle_jw_str(puddle_jw *w, const char *key, const char *s);
void puddle_jw_num(puddle_jw *w, const char *key, int64_t v);
void puddle_jw_flag(puddle_jw *w, const char *key, int v);
/* NUL-terminates and returns the length, or 0 if anything went wrong or the value is not complete. */
size_t puddle_jw_finish(puddle_jw *w);

#ifdef __cplusplus
}
#endif
#endif
