#ifndef PUDDLE_DELIVERY_H
#define PUDDLE_DELIVERY_H

/* What came of sending something to another Puddle: whatever does the sending (http/client.h) fills this in, and
 * the outbox (core/outbox.h) decides from it what to do next. */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PUDDLE_NET_OK,           /* an HTTP answer arrived (its status says what the other side thought); with a peer key, signed by them */
    PUDDLE_NET_OFFLINE,      /* nobody there: refused, timed out, unreachable, no such host. Worth retrying. */
    PUDDLE_NET_FAILED,       /* reached something, but not a Puddle conversation: TLS or HTTP went wrong */
    PUDDLE_NET_CERT_CHANGED, /* a Pond's `fingerprint_ok` refused the certificate; nothing was sent */
    PUDDLE_NET_NOT_PROVEN,   /* with a peer key: an answer came, but not one they signed for (`proof` says how); `status` is filled */
    PUDDLE_NET_OTHER_CERT    /* with a certificate proven to be theirs: the connection showed another, so nothing was sent */
} puddle_net_result;

/* What a Puddle's receipt said about an answer (docs/streams.md, "Proving who answers"; http/reqsig.h checks it) */
typedef enum {
    PUDDLE_RECEIPT_OK = 0,
    PUDDLE_RECEIPT_MISSING,     /* no receipt at all (what a stranger's 404, or something that is not a Puddle, gives) */
    PUDDLE_RECEIPT_WRONG_KEY,   /* signed by another id, or by a key that is not the one we hold for them */
    PUDDLE_RECEIPT_BAD,         /* malformed, or the signature does not cover this request, status and body */
    PUDDLE_RECEIPT_OTHER_CERT   /* a good signature, for a certificate other than the one on this connection */
} puddle_receipt_check;

typedef struct {
    puddle_net_result result;
    int status;               /* HTTP status, for OK and NOT_PROVEN */
    char reason[200];         /* in words, for anything but OK */
    char fingerprint[65];     /* hex SHA-256 of the certificate this connection showed; "" if TLS was not reached */
    puddle_receipt_check proof; /* with a peer key and an answer: what the receipt said */
    int checked;              /* the answer was checked against a key we hold for them, so only their signature counts */
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
