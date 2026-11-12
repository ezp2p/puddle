#include "storage/media.h"

#include "platform/fsutil.h"
#include "base/hash.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *puddle_image_sniff(const unsigned char *b, size_t n)
{
    if (n >= 8 && memcmp(b, "\x89PNG\r\n\x1a\n", 8) == 0)
        return "image/png";
    if (n >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF)
        return "image/jpeg";
    if (n >= 6 && (memcmp(b, "GIF87a", 6) == 0 || memcmp(b, "GIF89a", 6) == 0))
        return "image/gif";
    if (n >= 12 && memcmp(b, "RIFF", 4) == 0 && memcmp(b + 8, "WEBP", 4) == 0)
        return "image/webp";
    return NULL;
}


static void seterr(char *err, size_t n, const char *msg)
{
    if (err && n)
        snprintf(err, n, "%s", msg);
}

int puddle_media_hash_valid(const char *hash)
{
    if (!hash || strlen(hash) != 64)
        return 0;
    for (int i = 0; i < 64; i++)
        if (!((hash[i] >= '0' && hash[i] <= '9') || (hash[i] >= 'a' && hash[i] <= 'f')))
            return 0;
    return 1;
}

static int path_of(const char *data_dir, const char *hash, char *out, size_t cap)
{
    if (!puddle_media_hash_valid(hash))
        return -1;
    int n = snprintf(out, cap, "%s/media/%s", data_dir, hash);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}

int puddle_media_have(const char *data_dir, const char *hash)
{
    char path[1200];
    return path_of(data_dir, hash, path, sizeof path) == 0 && puddle_path_is_file(path);
}

int puddle_media_read(const char *data_dir, const char *hash, size_t max, unsigned char **data, size_t *len, char mime[40])
{
    char path[1200];
    if (path_of(data_dir, hash, path, sizeof path) != 0)
        return 1;
    FILE *f = fopen(path, "rb");
    if (!f)
        return errno == ENOENT ? 1 : -1;
    int64_t fsize = puddle_path_size(path); /* named by its hash: nobody replaces it under us */
    if (fsize <= 0 || (uint64_t)fsize > max) {
        fclose(f);
        return -1;
    }
    size_t n = (size_t)fsize;
    unsigned char *buf = (unsigned char *)malloc(n);
    if (!buf || fread(buf, 1, n, f) != n) {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);
    const char *m = puddle_image_sniff(buf, n);
    if (!m) { /* not an image: it is not served as one */
        free(buf);
        return -1;
    }
    snprintf(mime, 40, "%s", m);
    *data = buf;
    *len = n;
    return 0;
}

int puddle_media_store(const char *data_dir, const char *hash, const unsigned char *data, size_t len, uint64_t expected_size,
                       const char *expected_mime, char *err, size_t errlen)
{
    char path[1200], dir[1100], got[65];
    if (path_of(data_dir, hash, path, sizeof path) != 0) {
        seterr(err, errlen, "not a hash");
        return -1;
    }
    if (len == 0 || len != expected_size || len > PUDDLE_IMAGE_MAX_BYTES) {
        seterr(err, errlen, "it is not the size it was said to be");
        return -1;
    }
    puddle_sha256_hex(data, len, got);
    if (strcmp(got, hash) != 0) {
        seterr(err, errlen, "it is not the picture it was said to be (the hash differs)");
        return -1;
    }
    const char *mime = puddle_image_sniff(data, len);
    if (!mime || (expected_mime && strcmp(mime, expected_mime) != 0)) {
        seterr(err, errlen, "it is not an image of the kind it was said to be");
        return -1;
    }
    if (puddle_media_have(data_dir, hash))
        return 0;
    snprintf(dir, sizeof dir, "%s/media", data_dir);
    if (puddle_mkdir_p(dir) != 0 || puddle_write_file_atomic(dir, path, data, len) != 0) {
        seterr(err, errlen, "cannot save it");
        return -1;
    }
    return 0;
}
