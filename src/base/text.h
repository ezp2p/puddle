#ifndef PUDDLE_TEXT_H
#define PUDDLE_TEXT_H

/* Text from other people, made safe to keep and show. */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Clean up text a stranger sent: valid UTF-8 (bad bytes become U+FFFD), no control characters (a newline is kept
 * if `multiline`), no bidirectional overrides (they let a name read backwards to hide what it says), trimmed.
 * 0 if it fits in `max_chars` characters and `cap` bytes, else -1; `*chars` gets the length. */
int puddle_clean_text(const char *in, size_t len, size_t max_chars, int multiline, char *out, size_t cap, size_t *chars);

/* Is `u` an address a person can be sent to: http:// or https://, a host, at most `max` bytes of printable ASCII, and none
 * of the characters that could break out of where it is shown (quotes, angle brackets, backslash, spaces)? 1 or 0. */
int puddle_url_ok(const char *u, size_t max);

/* Cut UTF-8 text `s` in place to at most `max_chars` characters, then drop spaces and line breaks left at the end. */
void puddle_text_cut(char *s, size_t max_chars);

/* Compare the first `n` bytes of two strings ignoring ASCII case (header names, "Bearer"): 0 if equal, as strncasecmp. */
int puddle_ascii_ncasecmp(const char *a, const char *b, size_t n);

#ifdef __cplusplus
}
#endif
#endif
