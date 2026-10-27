#include "core/auth.h"

#include "core/config.h"
#include "base/encoding.h"
#include "platform/fsutil.h"
#include "storage/storage.h"
#include "base/hash.h"
#include "base/log.h"
#include "base/text.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <openssl/crypto.h>
#include <openssl/rand.h>

#define TOKEN_HEX 64

struct puddle_auth {
    puddle_storage *storage;
    puddle_storage *owned;     /* the files driver opened for a data directory; NULL when handed one */
    char path[PUDDLE_PATH_MAX]; /* where the files driver keeps it, for the app to read; "" otherwise */
    char token[TOKEN_HEX + 1];
};

static void seterr(char *err, size_t n, const char *fmt, ...)
{
    if (!err || !n)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

static int valid_token(const char *t)
{
    unsigned char raw[32];
    return strlen(t) == TOKEN_HEX && puddle_hex_decode(t, raw, 32) == 0;
}

puddle_auth *puddle_auth_open(const char *data_dir, char *err, size_t errlen)
{
    puddle_storage *st = puddle_storage_open_files(data_dir, err, errlen);
    if (!st)
        return NULL;
    puddle_auth *a = puddle_auth_open_with(st, err, errlen);
    if (!a) {
        puddle_storage_close(st);
        return NULL;
    }
    a->owned = st;
    snprintf(a->path, sizeof a->path, "%s/state/owner-token", data_dir);
    return a;
}

puddle_auth *puddle_auth_open_with(puddle_storage *st, char *err, size_t errlen)
{
    puddle_auth *a = (puddle_auth *)calloc(1, sizeof *a);
    if (!a) {
        seterr(err, errlen, "out of memory");
        return NULL;
    }
    a->storage = st;
    char *kept;
    size_t kl;
    int have = puddle_secret_get(st, "owner-token", &kept, &kl);
    if (have == 0) {
        kept[strcspn(kept, "\r\n")] = '\0';
        int ok = valid_token(kept);
        if (ok)
            memcpy(a->token, kept, TOKEN_HEX + 1);
        OPENSSL_cleanse(kept, kl);
        free(kept);
        if (ok)
            return a;
    }

    unsigned char raw[32];
    if (RAND_bytes(raw, sizeof raw) != 1) {
        seterr(err, errlen, "cannot generate a token");
        goto fail;
    }
    puddle_hex_encode(raw, sizeof raw, a->token);
    memset(raw, 0, sizeof raw);
    char text[TOKEN_HEX + 2];
    snprintf(text, sizeof text, "%s\n", a->token);
    if (puddle_secret_put(st, "owner-token", text, TOKEN_HEX + 1) != 0) {
        seterr(err, errlen, "cannot keep the owner token: %s", strerror(errno));
        goto fail;
    }
    memset(text, 0, sizeof text);
    puddle_log(have == 0 ? PUDDLE_LOG_WARN : PUDDLE_LOG_INFO, "%s owner token", have == 0 ? "replaced a damaged" : "created an");
    return a;
fail:
    puddle_auth_close(a);
    return NULL;
}

void puddle_auth_close(puddle_auth *a)
{
    if (!a)
        return;
    memset(a->token, 0, sizeof a->token);
    puddle_storage_close(a->owned);
    free(a);
}

int puddle_auth_check_bearer(const puddle_auth *a, const char *authorization)
{
    if (!authorization || puddle_ascii_ncasecmp(authorization, "Bearer ", 7) != 0)
        return 0;
    const char *t = authorization + 7;
    if (strlen(t) != TOKEN_HEX)
        return 0;
    return CRYPTO_memcmp(t, a->token, TOKEN_HEX) == 0;
}

const char *puddle_auth_token_path(const puddle_auth *a) { return a->path; }
const char *puddle_auth_token(const puddle_auth *a) { return a->token; }
