#ifndef PUDDLE_TLS_H
#define PUDDLE_TLS_H

#include <stddef.h>
#include <openssl/ssl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Build a server SSL_CTX from <dir>/cert.pem and <dir>/key.pem. If neither
 * exists, a self-signed P-256 certificate (CN=puddle, SAN localhost/127.0.0.1/::1,
 * valid 10 years) is generated first and the key is written mode 0600.
 * If only one of the two exists, that is an error (we never overwrite a key).
 * The certificate's SHA-256 fingerprint is logged so clients can pin it. */
SSL_CTX *puddle_tls_context(const char *dir, char *err, size_t errlen);

/* The certificate in `ctx` as 64 lowercase hex characters of its SHA-256: the form the outbound client records and
 * compares. 0 or -1. */
int puddle_tls_fingerprint_hex(SSL_CTX *ctx, char out[65]);

#ifdef __cplusplus
}
#endif
#endif
