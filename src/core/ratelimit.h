#ifndef PUDDLE_RATELIMIT_H
#define PUDDLE_RATELIMIT_H

/* Two small limiters for requests from people we don't know yet. Both take the time as an argument, so they
 * are testable, and both are safe to call from several threads. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A token bucket: `burst` at once, then `per_second` on average. Used for work done before we know who is
 * asking (checking a key chain costs signatures), so a flood can't pin the CPU. */
typedef struct puddle_bucket puddle_bucket;
puddle_bucket *puddle_bucket_new(double per_second, int burst);
void puddle_bucket_free(puddle_bucket *b);
int puddle_bucket_take(puddle_bucket *b, int64_t now_ms); /* 1 allowed, 0 not */

/* At most `max_hits` per `window_ms` for each key (a user id), across a table of `slots` keys; when the table
 * is full the oldest window is forgotten first. */
typedef struct puddle_keyed puddle_keyed;
puddle_keyed *puddle_keyed_new(size_t slots, int max_hits, int64_t window_ms);
void puddle_keyed_free(puddle_keyed *k);
int puddle_keyed_hit(puddle_keyed *k, const char *key, int64_t now_ms); /* 1 allowed (and counted), 0 over */

#ifdef __cplusplus
}
#endif
#endif
