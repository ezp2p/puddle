#ifndef PUDDLE_CLIENT_H
#define PUDDLE_CLIENT_H

/* Calling another Puddle (or a Pond): one signed, sealed HTTPS request (the protocol's spec/02 and spec/09).
 *
 * TLS only carries it. No certificate is checked, compared or remembered, so a peer behind a reverse proxy, or on a certificate
 * that changes every day, is reached the same. The body is sealed to the peer's sealing key (`seal_pub`) and the answer comes
 * back sealed to us, so whatever carries them reads nothing; the request is signed, which proves us to them; and their answer
 * carries a signed receipt, which proves them to us: given the key we hold for them (`peer_pub`), it is checked here.
 *
 * Addresses come from strangers (a link, a request), so the ones that can only be a mistake or a probe are
 * refused before connecting: unspecified, multicast, broadcast and link-local (which includes the cloud
 * metadata address). Loopback and private ranges stay allowed, since friends on one network are normal. */

#include "core/config.h"
#include "core/identity.h"
#include "core/delivery.h"
#include "core/link.h"
#include "http/reqsig.h"
#include "platform/net.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif


typedef struct {
    int connect_timeout_s;    /* 0 = 8 */
    int io_timeout_s;         /* each read or write; 0 = 10 */
    int total_timeout_s;      /* the whole call; 0 = 30 */
    size_t max_body;          /* the largest answer to accept; 0 = PUDDLE_NET_BODY_MAX. Larger ones fail rather than fill memory. */
    const unsigned char *peer_pub; /* a Puddle's current signing key: their answer must carry their receipt. NULL = not checked. */
    /* Their sealing key (spec/09): the body is sealed to it and the answer opened, so whatever carries it reads nothing. NULL =
     * sent as it is, which only a first request to a bare address does. */
    const unsigned char *seal_pub;
    /* Told the connection once it is made, and PUDDLE_SOCK_NONE just before it is closed, so another thread can end a call
     * that is waiting (puddle_sock_cut): a Pond holds a request for events open for up to 25 seconds. NULL = not told. */
    void (*on_socket)(void *user, puddle_sock s);
    void *user;
    /* Their Levee (spec/06), as their address record named it: a custody receipt it signs for this request counts as delivered
     * (`custody` in the reply). NULL = none. */
    const char *peer_levee_id;
    const unsigned char *peer_levee_key;
    /* This Puddle's own requests to its Levee: never sent through it, signed in this header ("X-Levee-Signature") instead of
     * X-Puddle-Signature, with `extra_headers` ("" or lines each ending \r\n). Everything else goes through the Levee while
     * this Puddle is joined to one (core/levee.h), unless `direct`. */
    int direct;
    const char *sig_header;
    const char *extra_headers;
    const char *content_type; /* the body's, when it is not JSON (or sealed); NULL = as usual */
} puddle_net_opts;

#define PUDDLE_NET_BODY_MAX (64u * 1024)

/* POST or GET `target` at `addr`, signed by `me` for the Puddle `peer_id`. `body` is sent as JSON (NULL/0 for
 * none). Never fails to fill `out`; always puddle_net_reply_free() it. */
void puddle_net_request(const puddle_address *addr, const char *method, const char *target, const void *body,
                        size_t body_len, puddle_identity *me, const char *peer_id, const puddle_net_opts *opts,
                        puddle_net_reply *out);

/* Call this Puddle's own owner API (api/openapi.yaml), as `puddled` does for the owner at a terminal: on loopback, at the
 * address and port `cfg` listens on, with the owner token from its data folder. `body` is JSON or NULL. Fills `out` as
 * puddle_net_request does (result, status, body, reason), without any signature or sealing: it never leaves the computer. */
void puddle_owner_request(const puddle_config *cfg, const char *method, const char *target, const char *body, puddle_net_reply *out);

/* Would we dial this address? (resolved addresses are checked the same way.) For tests and for callers who
 * want to say no early. */
int puddle_net_host_dialable(const char *host);

/* For addresses that arrive from someone else: is this host an acceptable thing to record? A numeric address we would
 * never dial (unspecified, multicast, link-local...) is not; a name is accepted, since judging it would mean a lookup, and
 * the client checks what it resolves to when it comes to connect. */
int puddle_net_host_plausible(const char *host);

#ifdef __cplusplus
}
#endif
#endif
