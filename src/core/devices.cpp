#include "core/devices.h"

#include "base/encoding.h"
#include "base/hash.h"
#include "base/json.h"
#include "core/identity.h"
#include "platform/fsutil.h"

#include <errno.h>
#include <mutex>
#include <new>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

static void seterr(char *err, size_t n, const char *fmt, ...)
{
    if (!err || !n)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

/* ---- keys ---- */

int puddle_devkey_from_seed(puddle_devkey *k, const unsigned char seed[32])
{
    memset(k, 0, sizeof *k);
    EVP_PKEY *pk = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, seed, 32);
    size_t pl = 32;
    int ok = pk && EVP_PKEY_get_raw_public_key(pk, k->pub, &pl) == 1 && pl == 32;
    EVP_PKEY_free(pk);
    if (!ok)
        return -1;
    memcpy(k->priv, seed, 32);
    puddle_device_id(k->pub, k->id);
    return 0;
}

int puddle_devkey_generate(puddle_devkey *k)
{
    unsigned char seed[32];
    if (RAND_bytes(seed, sizeof seed) != 1)
        return -1;
    int rc = puddle_devkey_from_seed(k, seed);
    OPENSSL_cleanse(seed, sizeof seed);
    return rc;
}

int puddle_devkey_sign(void *user, const void *data, size_t len, unsigned char sig[64])
{
    const puddle_devkey *k = (const puddle_devkey *)user;
    EVP_PKEY *pk = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL, k->priv, 32);
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    size_t sl = 64;
    int ok = pk && ctx && EVP_DigestSignInit(ctx, NULL, NULL, NULL, pk) == 1 && EVP_DigestSign(ctx, sig, &sl, (const unsigned char *)data, len) == 1 && sl == 64;
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pk);
    return ok ? 0 : -1;
}

static void device_hash(const unsigned char pub[32], unsigned char out[32])
{
    static const char label[] = "puddle-device-v1\n";
    unsigned char buf[sizeof label - 1 + 32];
    memcpy(buf, label, sizeof label - 1);
    memcpy(buf + sizeof label - 1, pub, 32);
    SHA256(buf, sizeof buf, out);
}

void puddle_device_id(const unsigned char pub[32], char out[PUDDLE_DEVICE_ID_LEN + 1])
{
    unsigned char h[32];
    device_hash(pub, h);
    puddle_base32_encode(h, 32, out, PUDDLE_DEVICE_ID_LEN + 1);
}

void puddle_device_check(const unsigned char pub[32], char out[7])
{
    unsigned char h[32];
    device_hash(pub, h);
    uint32_t v = ((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) | ((uint32_t)h[2] << 8) | h[3];
    snprintf(out, 7, "%06u", (unsigned)(v % 1000000u));
}

/* ---- the list ---- */

struct puddle_devices {
    std::mutex mu;
    char dir[1100], path[1200];
    puddle_device items[PUDDLE_DEVICES_MAX];
    size_t n;
    char secret[PUDDLE_PAIR_SECRET_LEN + 1]; /* "" when none */
    int64_t secret_until;
};

static const char *state_name(puddle_device_state s) { return s == PUDDLE_DEVICE_ACTIVE ? "active" : "pending"; }

/* caller holds the lock */
static int save(puddle_devices *d, char *err, size_t errlen)
{
    size_t cap = 512 + d->n * 400;
    char *buf = (char *)malloc(cap);
    if (!buf) {
        seterr(err, errlen, "out of memory");
        return -1;
    }
    puddle_jw w;
    puddle_jw_init(&w, buf, cap);
    puddle_jw_object(&w);
    puddle_jw_num(&w, "v", 1);
    puddle_jw_key(&w, "devices");
    puddle_jw_array(&w);
    for (size_t i = 0; i < d->n; i++) {
        const puddle_device *x = &d->items[i];
        char hex[65];
        puddle_hex_encode(x->pub, 32, hex);
        puddle_jw_object(&w);
        puddle_jw_str(&w, "name", x->name);
        puddle_jw_str(&w, "key", hex);
        puddle_jw_str(&w, "state", state_name(x->state));
        puddle_jw_num(&w, "created", x->created_ms);
        puddle_jw_end_object(&w);
    }
    puddle_jw_end_array(&w);
    puddle_jw_end_object(&w);
    size_t len = puddle_jw_finish(&w);
    int rc = len && puddle_write_file_atomic(d->dir, d->path, buf, len) == 0 ? 0 : -1;
    free(buf);
    if (rc != 0)
        seterr(err, errlen, "cannot save the device list: %s", strerror(errno));
    return rc;
}

static int load(puddle_devices *d, char *err, size_t errlen)
{
    FILE *f = fopen(d->path, "rb");
    if (!f)
        return errno == ENOENT ? 0 : (seterr(err, errlen, "cannot read %s: %s", d->path, strerror(errno)), -1);
    char *text = (char *)malloc(64 * 1024);
    size_t len = text ? fread(text, 1, 64 * 1024 - 1, f) : 0;
    fclose(f);
    if (!text) {
        seterr(err, errlen, "out of memory");
        return -1;
    }
    text[len] = '\0';
    puddle_span arr, item, v;
    int rc = -1;
    size_t cur = 0;
    int r = -1;
    if (!puddle_json_valid_object(text, len) || puddle_json_member(text, len, "devices", &arr) != 0)
        goto out;
    while ((r = puddle_json_array_next(arr, &cur, &item)) == 1) {
        if (d->n == PUDDLE_DEVICES_MAX)
            goto out;
        puddle_device *x = &d->items[d->n];
        memset(x, 0, sizeof *x);
        char hex[80], st[16];
        if (puddle_json_member(item.p, item.len, "name", &v) != 0 || puddle_json_string(v, x->name, sizeof x->name) != 0 ||
            puddle_json_member(item.p, item.len, "key", &v) != 0 || puddle_json_string(v, hex, sizeof hex) != 0 ||
            puddle_hex_decode(hex, x->pub, 32) != 0 || strlen(hex) != 64 || puddle_json_member(item.p, item.len, "state", &v) != 0 ||
            puddle_json_string(v, st, sizeof st) != 0 || puddle_json_member(item.p, item.len, "created", &v) != 0 ||
            puddle_json_int64(v, &x->created_ms) != 0)
            goto out;
        if (!strcmp(st, "active"))
            x->state = PUDDLE_DEVICE_ACTIVE;
        else if (!strcmp(st, "pending"))
            x->state = PUDDLE_DEVICE_PENDING;
        else
            goto out;
        puddle_device_id(x->pub, x->id);
        d->n++;
    }
    rc = r < 0 ? -1 : 0;
out:
    if (rc != 0)
        seterr(err, errlen, "%s is damaged", d->path);
    free(text);
    return rc;
}

puddle_devices *puddle_devices_open(const char *data_dir, char *err, size_t errlen)
{
    puddle_devices *d = new (std::nothrow) puddle_devices();
    if (!d) {
        seterr(err, errlen, "out of memory");
        return NULL;
    }
    snprintf(d->dir, sizeof d->dir, "%s/state", data_dir);
    snprintf(d->path, sizeof d->path, "%s/devices.json", d->dir);
    if (puddle_mkdir_p(d->dir) != 0) {
        seterr(err, errlen, "cannot make %s: %s", d->dir, strerror(errno));
        puddle_devices_close(d);
        return NULL;
    }
    if (load(d, err, errlen) != 0) {
        puddle_devices_close(d);
        return NULL;
    }
    return d;
}

void puddle_devices_close(puddle_devices *d)
{
    if (!d)
        return;
    OPENSSL_cleanse(d->secret, sizeof d->secret);
    delete d;
}

int puddle_devices_new_secret(puddle_devices *d, int64_t now_ms, char out[PUDDLE_PAIR_SECRET_LEN + 1], int64_t *expires_ms)
{
    unsigned char raw[PUDDLE_PAIR_SECRET_LEN / 2];
    if (RAND_bytes(raw, sizeof raw) != 1)
        return -1;
    d->mu.lock();
    puddle_hex_encode(raw, sizeof raw, d->secret);
    d->secret_until = now_ms + PUDDLE_PAIR_MS;
    memcpy(out, d->secret, PUDDLE_PAIR_SECRET_LEN + 1);
    if (expires_ms)
        *expires_ms = d->secret_until;
    d->mu.unlock();
    OPENSSL_cleanse(raw, sizeof raw);
    return 0;
}

/* caller holds the lock */
static int secret_ok_locked(const puddle_devices *d, const char *secret, int64_t now_ms)
{
    if (!secret || strlen(secret) != PUDDLE_PAIR_SECRET_LEN || !d->secret[0] || now_ms >= d->secret_until)
        return 0;
    return CRYPTO_memcmp(secret, d->secret, PUDDLE_PAIR_SECRET_LEN) == 0;
}

int puddle_devices_secret_ok(puddle_devices *d, const char *secret, int64_t now_ms)
{
    d->mu.lock();
    int ok = secret_ok_locked(d, secret, now_ms);
    d->mu.unlock();
    return ok;
}

static puddle_device *find(puddle_devices *d, const char *id)
{
    for (size_t i = 0; id && i < d->n; i++)
        if (!strcmp(d->items[i].id, id))
            return &d->items[i];
    return NULL;
}

int puddle_devices_pair(puddle_devices *d, const char *secret, const unsigned char pub[32], const char *name, int64_t now_ms,
                        puddle_device *out, char *err, size_t errlen)
{
    char id[PUDDLE_DEVICE_ID_LEN + 1];
    puddle_device_id(pub, id);
    d->mu.lock();
    int rc;
    puddle_device *had = find(d, id);
    if (!secret_ok_locked(d, secret, now_ms)) {
        rc = 1;
    } else if (had) {
        if (out)
            *out = *had;
        rc = 2;
    } else if (d->n == PUDDLE_DEVICES_MAX) {
        seterr(err, errlen, "there are %d devices already", PUDDLE_DEVICES_MAX);
        rc = 3;
    } else {
        puddle_device *x = &d->items[d->n++];
        memset(x, 0, sizeof *x);
        memcpy(x->id, id, sizeof x->id);
        snprintf(x->name, sizeof x->name, "%s", name);
        memcpy(x->pub, pub, 32);
        x->state = PUDDLE_DEVICE_PENDING;
        x->created_ms = now_ms;
        if (save(d, err, errlen) != 0) {
            d->n--;
            rc = -1;
        } else {
            OPENSSL_cleanse(d->secret, sizeof d->secret); /* one use */
            d->secret_until = 0;
            if (out)
                *out = *x;
            rc = 0;
        }
    }
    d->mu.unlock();
    return rc;
}

int puddle_devices_get(puddle_devices *d, const char *id, puddle_device *out)
{
    d->mu.lock();
    puddle_device *x = find(d, id);
    if (x && out)
        *out = *x;
    d->mu.unlock();
    return x ? 0 : 1;
}

static int newest_first(const void *a, const void *b)
{
    int64_t x = ((const puddle_device *)a)->created_ms, y = ((const puddle_device *)b)->created_ms;
    return x < y ? 1 : x > y ? -1 : 0;
}

int puddle_devices_list(puddle_devices *d, puddle_device **out, size_t *count)
{
    d->mu.lock();
    size_t n = d->n;
    puddle_device *copy = (puddle_device *)malloc((n ? n : 1) * sizeof *copy);
    if (copy && n)
        memcpy(copy, d->items, n * sizeof *copy);
    d->mu.unlock();
    if (!copy)
        return -1;
    qsort(copy, n, sizeof *copy, newest_first);
    *out = copy;
    *count = n;
    return 0;
}

int puddle_devices_accept(puddle_devices *d, const char *id, char *err, size_t errlen)
{
    d->mu.lock();
    puddle_device *x = find(d, id);
    int rc = 0;
    if (!x) {
        rc = 1;
    } else if (x->state != PUDDLE_DEVICE_PENDING) {
        rc = 2;
    } else {
        x->state = PUDDLE_DEVICE_ACTIVE;
        if (save(d, err, errlen) != 0) {
            x->state = PUDDLE_DEVICE_PENDING;
            rc = -1;
        }
    }
    d->mu.unlock();
    return rc;
}

int puddle_devices_remove(puddle_devices *d, const char *id, int only_pending, char *err, size_t errlen)
{
    d->mu.lock();
    puddle_device *x = find(d, id);
    int rc = 0;
    if (!x) {
        rc = 1;
    } else if (only_pending && x->state != PUDDLE_DEVICE_PENDING) {
        rc = 2;
    } else {
        size_t i = (size_t)(x - d->items);
        puddle_device gone = *x;
        memmove(&d->items[i], &d->items[i + 1], (d->n - i - 1) * sizeof *x);
        d->n--;
        if (save(d, err, errlen) != 0) { /* put it back: the file still says it is there */
            memmove(&d->items[i + 1], &d->items[i], (d->n - i) * sizeof *x);
            d->items[i] = gone;
            d->n++;
            rc = -1;
        }
    }
    d->mu.unlock();
    return rc;
}

void puddle_devices_seen(puddle_devices *d, const char *id, int64_t now_ms)
{
    d->mu.lock();
    puddle_device *x = find(d, id);
    if (x)
        x->last_seen_ms = now_ms;
    d->mu.unlock();
}

static int key_of(puddle_devices *d, const char *id, const char *kid, unsigned char pub[32], int pending_too)
{
    if (!d || !id || !kid)
        return 0;
    d->mu.lock();
    puddle_device *x = find(d, id);
    int ok = 0;
    if (x && (pending_too || x->state == PUDDLE_DEVICE_ACTIVE)) {
        char k[65];
        puddle_key_id(x->pub, k);
        if (!strcmp(k, kid)) {
            memcpy(pub, x->pub, 32);
            ok = 1;
        }
    }
    d->mu.unlock();
    return ok;
}

int puddle_devices_key(void *devices, const char *id, const char *kid, unsigned char sign_pub[32])
{
    return key_of((puddle_devices *)devices, id, kid, sign_pub, 0);
}

int puddle_devices_key_any(void *devices, const char *id, const char *kid, unsigned char sign_pub[32])
{
    return key_of((puddle_devices *)devices, id, kid, sign_pub, 1);
}

int puddle_devices_is_active(puddle_devices *d, const char *id)
{
    if (!d)
        return 0;
    d->mu.lock();
    puddle_device *x = find(d, id);
    int ok = x && x->state == PUDDLE_DEVICE_ACTIVE;
    d->mu.unlock();
    return ok;
}
