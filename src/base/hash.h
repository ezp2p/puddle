#ifndef PUDDLE_HASH_H
#define PUDDLE_HASH_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Lowercase hex of `len` bytes into `out` (needs 2*len+1 bytes). */
void puddle_hex_encode(const unsigned char *bytes, unsigned len, char *out);

/* SHA-256 of `data` as 64 lowercase hex characters (+ NUL). */
void puddle_sha256_hex(const void *data, size_t len, char out[65]);

/* HKDF-SHA256 (RFC 5869): `outlen` bytes from `ikm`, with a salt and a context label. 0 or -1. */
int puddle_hkdf_sha256(const void *ikm, size_t ikm_len, const char *salt, const char *info,
                       unsigned char *out, size_t outlen);

#ifdef __cplusplus
}
#endif
#endif
