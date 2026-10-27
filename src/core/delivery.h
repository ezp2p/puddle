#ifndef PUDDLE_DELIVERY_H
#define PUDDLE_DELIVERY_H

/* What came of sending something to another Puddle: whatever does the sending (http/client.h) fills this in, and
 * the outbox (core/outbox.h) decides from it what to do next. */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PUDDLE_NET_OK,           /* an HTTP answer arrived (its status says what the other side thought) */
    PUDDLE_NET_OFFLINE,      /* nobody there: refused, timed out, unreachable, no such host. Worth retrying. */
    PUDDLE_NET_FAILED,       /* reached something, but not a Puddle conversation: TLS or HTTP went wrong. Not retried. */
    PUDDLE_NET_CERT_CHANGED  /* `fingerprint_ok` refused the certificate; nothing was sent */
} puddle_net_result;

typedef struct {
    puddle_net_result result;
    int status;               /* HTTP status, for OK */
    char reason[200];         /* in words, for anything but OK */
    char fingerprint[65];     /* hex SHA-256 of the certificate; "" if TLS was not reached */
    char content_type[48];    /* of the answer, as it said (lower case, without parameters); "" if it did not */
    char *body;               /* the response body, NUL-terminated, or NULL. Free with puddle_net_reply_free. */
    size_t body_len;
} puddle_net_reply;

/* Free the body of a reply. */
void puddle_net_reply_free(puddle_net_reply *r);

#ifdef __cplusplus
}
#endif
#endif
