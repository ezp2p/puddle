#ifndef PUDDLE_ENCODING_H
#define PUDDLE_ENCODING_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* RFC 4648 base32, lowercase, no padding. Writes a NUL-terminated string; returns its length, or 0 if
 * `cap` is too small. 32 bytes give 52 characters (a user id). */
size_t puddle_base32_encode(const unsigned char *in, size_t len, char *out, size_t cap);

/* Standard base64 with padding. Same return convention. */
size_t puddle_base64_encode(const unsigned char *in, size_t len, char *out, size_t cap);

/* Strict base64: padding required, no whitespace, no stray bits. 0 and the byte count in *outlen, or -1. */
int puddle_base64_decode(const char *in, unsigned char *out, size_t cap, size_t *outlen);

/* Exactly `n` bytes from 2n lowercase hex characters. 0 or -1. */
int puddle_hex_decode(const char *hex, unsigned char *out, size_t n);

#ifdef __cplusplus
}
#endif
#endif
