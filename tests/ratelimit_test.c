#include "core/ratelimit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

int main(void)
{
    /* a bucket: a burst, then the steady rate */
    CHECK(puddle_bucket_new(0, 5) == NULL && puddle_bucket_new(1, 0) == NULL);
    puddle_bucket *b = puddle_bucket_new(2, 3); /* 2 a second, 3 at once */
    int64_t t = 1000000;
    CHECK(puddle_bucket_take(b, t) && puddle_bucket_take(b, t) && puddle_bucket_take(b, t));
    CHECK(!puddle_bucket_take(b, t));
    CHECK(!puddle_bucket_take(b, t + 499));      /* 0.998 of a token */
    CHECK(puddle_bucket_take(b, t + 500));       /* one token after half a second */
    CHECK(!puddle_bucket_take(b, t + 500));
    CHECK(puddle_bucket_take(b, t + 1000));
    CHECK(!puddle_bucket_take(b, t + 1000));
    t += 3600 * 1000;                            /* an hour later it is full, not overflowing */
    int n = 0;
    while (puddle_bucket_take(b, t) && n < 100) n++;
    CHECK(n == 3);
    CHECK(!puddle_bucket_take(b, t - 5000));     /* the clock stepping back gives nothing, and takes nothing away */
    CHECK(!puddle_bucket_take(b, t));            /* ...and stepping back then forward again does not refill it */
    CHECK(puddle_bucket_take(b, t + 500));
    puddle_bucket_free(b);

    /* per key */
    CHECK(puddle_keyed_new(0, 1, 1) == NULL && puddle_keyed_new(1, 0, 1) == NULL && puddle_keyed_new(1, 1, 0) == NULL);
    puddle_keyed *k = puddle_keyed_new(4, 2, 1000);
    t = 5000000;
    CHECK(puddle_keyed_hit(k, "a", t) && puddle_keyed_hit(k, "a", t + 1));
    CHECK(!puddle_keyed_hit(k, "a", t + 2) && !puddle_keyed_hit(k, "a", t + 999)); /* refusals aren't counted or extended */
    CHECK(puddle_keyed_hit(k, "b", t + 5));       /* someone else is unaffected */
    CHECK(puddle_keyed_hit(k, "a", t + 1000));    /* the window ended */
    CHECK(puddle_keyed_hit(k, "a", t + 1000) && !puddle_keyed_hit(k, "a", t + 1001));
    CHECK(!puddle_keyed_hit(k, NULL, t));
    char longkey[100];
    memset(longkey, 'x', 99); longkey[99] = 0;
    CHECK(!puddle_keyed_hit(k, longkey, t));      /* a key that doesn't fit is refused, not truncated into someone else's */
    /* a full table forgets the oldest, never refuses a newcomer */
    char key[8];
    for (int i = 0; i < 4; i++) { snprintf(key, sizeof key, "k%d", i); CHECK(puddle_keyed_hit(k, key, t + 2000 + i)); }
    CHECK(puddle_keyed_hit(k, "new", t + 2010));
    CHECK(puddle_keyed_hit(k, "k0", t + 2011) && puddle_keyed_hit(k, "k0", t + 2012)); /* k0 was the oldest: forgotten, fresh count */
    puddle_keyed_free(k);
    puddle_bucket_free(NULL);
    puddle_keyed_free(NULL);
    return failures ? 1 : 0;
}
