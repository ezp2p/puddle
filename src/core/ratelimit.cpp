#include "core/ratelimit.h"

#include <mutex>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct puddle_bucket {
    std::mutex mu;
    double per_ms, burst, tokens;
    int64_t last_ms;
    int started;
};

puddle_bucket *puddle_bucket_new(double per_second, int burst)
{
    if (per_second <= 0 || burst < 1)
        return NULL;
    puddle_bucket *b = new (std::nothrow) puddle_bucket();
    if (!b)
        return NULL;
    b->per_ms = per_second / 1000.0;
    b->burst = b->tokens = burst;
    return b;
}

void puddle_bucket_free(puddle_bucket *b)
{
    if (!b)
        return;
    delete b;
}

int puddle_bucket_take(puddle_bucket *b, int64_t now_ms)
{
    b->mu.lock();
    if (b->started && now_ms > b->last_ms) {
        b->tokens += (double)(now_ms - b->last_ms) * b->per_ms;
        if (b->tokens > b->burst)
            b->tokens = b->burst;
    }
    if (!b->started || now_ms > b->last_ms) /* a clock that steps back changes nothing */
        b->last_ms = now_ms;
    b->started = 1;
    int ok = b->tokens >= 1.0;
    if (ok)
        b->tokens -= 1.0;
    b->mu.unlock();
    return ok;
}

typedef struct {
    char key[64];
    int64_t start_ms;
    int hits;
    int used;
} slot;

struct puddle_keyed {
    std::mutex mu;
    slot *slots;
    size_t n;
    int max_hits;
    int64_t window_ms;
};

puddle_keyed *puddle_keyed_new(size_t slots, int max_hits, int64_t window_ms)
{
    if (slots < 1 || max_hits < 1 || window_ms < 1)
        return NULL;
    puddle_keyed *k = new (std::nothrow) puddle_keyed();
    if (!k)
        return NULL;
    k->slots = (slot *)calloc(slots, sizeof *k->slots);
    if (!k->slots) {
        delete k;
        return NULL;
    }
    k->n = slots;
    k->max_hits = max_hits;
    k->window_ms = window_ms;
    return k;
}

void puddle_keyed_free(puddle_keyed *k)
{
    if (!k)
        return;
    free(k->slots);
    delete k;
}

int puddle_keyed_hit(puddle_keyed *k, const char *key, int64_t now_ms)
{
    if (!key || strlen(key) >= sizeof k->slots[0].key)
        return 0;
    k->mu.lock();
    slot *found = NULL, *free_slot = NULL, *oldest = &k->slots[0];
    for (size_t i = 0; i < k->n; i++) {
        slot *s = &k->slots[i];
        if (s->used && now_ms - s->start_ms >= k->window_ms)
            s->used = 0; /* that window is over */
        if (s->used && !strcmp(s->key, key))
            found = s;
        if (!s->used && !free_slot)
            free_slot = s;
        if (s->used && (!oldest->used || s->start_ms < oldest->start_ms))
            oldest = s;
    }
    int ok;
    if (found) {
        ok = found->hits < k->max_hits;
        if (ok)
            found->hits++;
    } else {
        slot *s = free_slot ? free_slot : oldest;
        memset(s, 0, sizeof *s);
        snprintf(s->key, sizeof s->key, "%s", key);
        s->start_ms = now_ms;
        s->hits = 1;
        s->used = 1;
        ok = 1;
    }
    k->mu.unlock();
    return ok;
}
