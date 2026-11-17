#ifndef PUDDLE_MEDIA_H
#define PUDDLE_MEDIA_H

/* Images kept by what they are: <data_dir>/media/<sha256>. Our own profile images are put there when profiles load; a
 * friend's are fetched into it. A file is named by its hash, so it is only ever the one thing, and what is stored is
 * checked to be it. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PLACEHOLDER limit so a mistaken path can't copy something huge into the data directory.
 * The real policy is an open question (docs/discuss-later.md). */
#define PUDDLE_IMAGE_MAX_BYTES (25u * 1024 * 1024)

/* The kind of image these bytes are (image/png, image/jpeg, image/gif or image/webp), judged by what the file starts with, never
 * by what it is called or claims; NULL for anything else. */
const char *puddle_image_sniff(const unsigned char *bytes, size_t len);

/* Is `hash` a SHA-256 in the one spelling we keep (64 lower-case hex characters)? */
int puddle_media_hash_valid(const char *hash);

/* 1 if we hold that image. */
int puddle_media_have(const char *data_dir, const char *hash);

/* Read an image: malloc'd bytes in *data, its type in `mime` (judged from its bytes). 0, 1 if we do not hold it, -1 on error.
 * Never more than `max` bytes. */
int puddle_media_read(const char *data_dir, const char *hash, size_t max, unsigned char **data, size_t *len, char mime[40]);

/* Keep an image fetched from a friend, after checking it: its SHA-256 is `hash`, its length is `expected_size`, and its bytes
 * are an image of the type `expected_mime` said. 0 stored (or already held), -1 with a reason in `err`. */
int puddle_media_store(const char *data_dir, const char *hash, const unsigned char *data, size_t len, uint64_t expected_size,
                       const char *expected_mime, char *err, size_t errlen);

#ifdef __cplusplus
}
#endif
#endif
