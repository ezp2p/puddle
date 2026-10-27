#include "base/hash.h"

#include <string.h>

#include <openssl/core_names.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>

void puddle_hex_encode(const unsigned char *bytes, unsigned len, char *out)
{
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < len; i++) {
        out[2 * i] = hex[bytes[i] >> 4];
        out[2 * i + 1] = hex[bytes[i] & 15];
    }
    out[2 * len] = '\0';
}

void puddle_sha256_hex(const void *data, size_t len, char out[65])
{
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned mdlen = 0;
    EVP_Digest(data, len, md, &mdlen, EVP_sha256(), NULL);
    puddle_hex_encode(md, mdlen, out);
}

int puddle_hkdf_sha256(const void *ikm, size_t ikm_len, const char *salt, const char *info,
                       unsigned char *out, size_t outlen)
{
    EVP_KDF *kdf = EVP_KDF_fetch(NULL, "HKDF", NULL);
    EVP_KDF_CTX *ctx = kdf ? EVP_KDF_CTX_new(kdf) : NULL;
    int ok = 0;
    if (ctx) {
        OSSL_PARAM params[5];
        params[0] = OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST, (char *)"SHA256", 0);
        params[1] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_KEY, (void *)ikm, ikm_len);
        params[2] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_SALT, (void *)salt, strlen(salt));
        params[3] = OSSL_PARAM_construct_octet_string(OSSL_KDF_PARAM_INFO, (void *)info, strlen(info));
        params[4] = OSSL_PARAM_construct_end();
        ok = EVP_KDF_derive(ctx, out, outlen, params) == 1;
    }
    EVP_KDF_CTX_free(ctx);
    EVP_KDF_free(kdf);
    return ok ? 0 : -1;
}
