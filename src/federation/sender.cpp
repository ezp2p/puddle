#include "federation/sender.h"

#include "http/client.h"
#include "http/reqsig.h"
#include "platform/clock.h"

#include <mutex>
#include <stdio.h>
#include <string.h>

/* Sending one outbox item (protocol spec/02 and spec/09). Every request is sealed to the peer's key and signed; the answer
 * is believed only with their receipt. TLS only carries it: no certificate is checked or remembered, so a peer behind a reverse
 * proxy, or with a certificate that changes every day, is reached like any other. */

/* How telling a failure is, so that when every address fails the owner hears the one that says the most. */
static int telling(const puddle_net_reply *r)
{
    switch (r->result) {
    case PUDDLE_NET_OK: return 4;
    case PUDDLE_NET_NOT_PROVEN: return 3;
    case PUDDLE_NET_FAILED: return 2;
    default: return 0;
    }
}

/* One address */
static void attempt_at(const puddle_outbox_hooks *hooks, const puddle_ob_item *it, const puddle_address *addr, const char *recipient,
                       const unsigned char *pub, const unsigned char *seal, puddle_net_reply *reply)
{
    puddle_net_opts opts;
    memset(&opts, 0, sizeof opts);
    opts.connect_timeout_s = hooks->net.connect_timeout_s;
    opts.io_timeout_s = hooks->net.io_timeout_s;
    opts.total_timeout_s = hooks->net.total_timeout_s;
    opts.peer_pub = pub;
    opts.seal_pub = seal;
    opts.max_body = it->max_reply ? it->max_reply : 0;
    puddle_net_request(addr, it->method, it->path, it->body_len ? it->body : NULL, it->body_len, hooks->identity, recipient, &opts, reply);
}

void puddle_federation_send(const puddle_outbox_hooks *hooks, const puddle_ob_item *it, puddle_net_reply *reply)
{
    puddle_address addrs[4];
    memset(reply, 0, sizeof *reply);
    int n = hooks->addresses ? hooks->addresses(hooks->user, it->peer, addrs, 4) : 0;
    if (n <= 0) {
        reply->result = PUDDLE_NET_FAILED;
        snprintf(reply->reason, sizeof reply->reason, "we have no address for them");
        return;
    }
    /* a first request to an address says who it is from but not who it is for: nobody has said who lives there yet */
    int first = !strcmp(it->kind, PUDDLE_OB_KIND_OPEN_ACCESS);
    const char *recipient = first ? PUDDLE_RECIPIENT_ANY : it->peer;
    /* a request for a stream (or how one stands) is answered the same to anyone, so never in their name: nothing to check */
    int anonymous = first || !strcmp(it->path, "/stream/v1/access") || !strcmp(it->path, "/stream/v1/access/status");
    unsigned char pubkey[32], sealkey[32];
    const unsigned char *pub = !anonymous && hooks->peer_key && hooks->peer_key(hooks->user, it->peer, pubkey) ? pubkey : NULL;
    /* sealed whenever there is a key to seal to: theirs, or the one their link gave; only a bare address goes without */
    const unsigned char *seal = hooks->seal_key && hooks->seal_key(hooks->user, it->peer, sealkey) ? sealkey : NULL;
    puddle_net_reply best;
    memset(&best, 0, sizeof best);
    for (int pass = 0; pass < 2; pass++) {
        best.result = PUDDLE_NET_OFFLINE;
        for (int i = 0; i < n; i++) {
            puddle_net_reply r;
            attempt_at(hooks, it, &addrs[i], recipient, pub, seal, &r);
            if (r.result == PUDDLE_NET_OK || (!pub && r.result != PUDDLE_NET_OFFLINE) || i == 0 || telling(&r) > telling(&best)) {
                puddle_net_reply_free(&best);
                best = r;
            } else {
                puddle_net_reply_free(&r);
            }
            /* an answer settles it; so does anything at all from an address when we can't check who is there */
            if (best.result == PUDDLE_NET_OK || (!pub && best.result != PUDDLE_NET_OFFLINE))
                break;
        }
        /* Signed by a key we didn't hold for them, and meanwhile their new keys have arrived (they rolled, and told us while we
         * were asking): that was them after all, so ask again now rather than waiting out the schedule. */
        unsigned char now_pub[32];
        if (pass == 0 && pub && best.result == PUDDLE_NET_NOT_PROVEN && best.proof == PUDDLE_RECEIPT_WRONG_KEY &&
            hooks->peer_key(hooks->user, it->peer, now_pub) && memcmp(now_pub, pubkey, 32) != 0) {
            memcpy(pubkey, now_pub, 32);
            if (seal)
                hooks->seal_key(hooks->user, it->peer, sealkey);
            puddle_net_reply_free(&best);
            memset(&best, 0, sizeof best);
            continue;
        }
        break;
    }
    /* sealed to a key of theirs they have rolled away from: not a refusal. Their new chain is on its way (they announce it), and
     * this waits for it on the usual schedule. */
    if (best.result == PUDDLE_NET_OK && best.status == 409 && best.body && strstr(best.body, "\"stale_key\"")) {
        best.result = PUDDLE_NET_FAILED;
        snprintf(best.reason, sizeof best.reason, "they have new keys we haven't been sent yet");
    }
    best.checked = pub != NULL;
    *reply = best;
}
