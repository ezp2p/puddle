#include "federation/sender.h"

#include "http/client.h"
#include "http/reqsig.h"
#include "platform/clock.h"

#include <mutex>
#include <stdio.h>
#include <string.h>

/* ---- proven certificates (docs/streams.md, "Proving who answers") ----
 *
 * A peer's Puddle signed for the certificate its connection showed: requests to it may go over a connection showing that
 * certificate, without a handshake each time, for ten minutes, and every good receipt renews it. What a proof says ("this
 * certificate is that peer's") is true whichever of our Puddles learned it, so there is one table for the process (tests run
 * several Puddles in one). In memory only: a restart just means a handshake. */

#define PROOF_MS (10 * 60 * 1000LL)
#define PROOFS 64

typedef struct {
    char peer[PUDDLE_USER_ID_LEN + 1];
    char fp[65];
    int64_t until_ms;
} proof;

static std::mutex proofs_mu;
static proof proofs[PROOFS];

static int64_t now_of(const puddle_outbox_hooks *hooks)
{
    return hooks->now_ms ? hooks->now_ms() : puddle_wall_ms();
}

static void proof_get(const puddle_outbox_hooks *hooks, const char *peer, char fp[65])
{
    int64_t now = now_of(hooks);
    fp[0] = '\0';
    std::lock_guard<std::mutex> hold(proofs_mu);
    for (int i = 0; i < PROOFS; i++)
        if (!strcmp(proofs[i].peer, peer) && proofs[i].until_ms > now)
            memcpy(fp, proofs[i].fp, 65);
}

/* `fp` NULL forgets what we had for them */
static void proof_set(const puddle_outbox_hooks *hooks, const char *peer, const char *fp)
{
    int64_t now = now_of(hooks);
    std::lock_guard<std::mutex> hold(proofs_mu);
    int slot = -1;
    for (int i = 0; i < PROOFS && slot < 0; i++)
        if (!strcmp(proofs[i].peer, peer))
            slot = i;
    if (fp) {
        for (int i = 0; i < PROOFS && slot < 0; i++)
            if (proofs[i].until_ms <= now)
                slot = i;
        if (slot < 0) { /* full of live ones: the one closest to running out goes */
            slot = 0;
            for (int i = 1; i < PROOFS; i++)
                if (proofs[i].until_ms < proofs[slot].until_ms)
                    slot = i;
        }
        snprintf(proofs[slot].peer, sizeof proofs[slot].peer, "%s", peer);
        snprintf(proofs[slot].fp, sizeof proofs[slot].fp, "%s", fp);
        proofs[slot].until_ms = now + PROOF_MS;
    } else if (slot >= 0) {
        memset(&proofs[slot], 0, sizeof proofs[slot]);
    }
}

/* Routes that set a stream up or repair it, so the other side may not hold our current key and could not answer a handshake
 * without answering a stranger: they are checked by their receipt alone. Every other stream route needs the handshake. */
static int sets_up(const char *path)
{
    static const char *const P[] = { "/stream/v1/access", "/stream/v1/access/result", "/stream/v1/access/status", "/stream/v1/identity" };
    for (size_t i = 0; i < sizeof P / sizeof *P; i++) {
        size_t n = strlen(P[i]);
        if (!strncmp(path, P[i], n) && (path[n] == '\0' || path[n] == '?'))
            return 1;
    }
    return 0;
}

/* How telling a failure is, so that when every address fails the owner hears the one that says the most. */
static int telling(const puddle_net_reply *r)
{
    switch (r->result) {
    case PUDDLE_NET_OK: return 4;
    case PUDDLE_NET_NOT_PROVEN: return 3;
    case PUDDLE_NET_FAILED: return 2;
    case PUDDLE_NET_OTHER_CERT: return 1;
    default: return 0;
    }
}

/* One address. For a peer we must prove first, a handshake comes before the request unless a certificate was proven lately,
 * and the request goes only over a connection showing that certificate. */
static void attempt_at(const puddle_outbox_hooks *hooks, const puddle_ob_item *it, const puddle_address *addr, const char *recipient,
                       const unsigned char *pub, int handshake, puddle_net_reply *reply)
{
    puddle_net_opts opts;
    memset(&opts, 0, sizeof opts);
    opts.connect_timeout_s = hooks->net.connect_timeout_s;
    opts.io_timeout_s = hooks->net.io_timeout_s;
    opts.total_timeout_s = hooks->net.total_timeout_s;
    opts.peer_pub = pub;
    char fp[65] = "";
    for (int round = 0; round < 2; round++) {
        if (handshake) {
            proof_get(hooks, it->peer, fp);
            if (!fp[0]) {
                puddle_net_opts h = opts;
                h.max_body = 0;
                puddle_net_request(addr, "GET", "/stream/v1/hello", NULL, 0, hooks->identity, recipient, &h, reply);
                if (reply->result != PUDDLE_NET_OK || reply->status != 204)
                    return; /* not proven, or they said something else (a signed refusal is still them saying it) */
                proof_set(hooks, it->peer, reply->fingerprint);
                snprintf(fp, sizeof fp, "%s", reply->fingerprint);
                puddle_net_reply_free(reply);
            }
            opts.expect_fp = fp;
        }
        opts.max_body = it->max_reply ? it->max_reply : 0;
        puddle_net_request(addr, it->method, it->path, it->body_len ? it->body : NULL, it->body_len, hooks->identity, recipient, &opts, reply);
        if (reply->result == PUDDLE_NET_OTHER_CERT && round == 0) {
            proof_set(hooks, it->peer, NULL); /* a new certificate since: prove it, then try once more */
            puddle_net_reply_free(reply);
            continue;
        }
        if (pub && reply->result == PUDDLE_NET_OK)
            proof_set(hooks, it->peer, reply->fingerprint); /* every receipt proves the certificate again */
        else if (pub && reply->result == PUDDLE_NET_NOT_PROVEN)
            proof_set(hooks, it->peer, NULL);
        return;
    }
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
    unsigned char pubkey[32];
    const unsigned char *pub = !anonymous && hooks->peer_key && hooks->peer_key(hooks->user, it->peer, pubkey) ? pubkey : NULL;
    int handshake = pub && !sets_up(it->path);
    puddle_net_reply best;
    memset(&best, 0, sizeof best);
    for (int pass = 0; pass < 2; pass++) {
        best.result = PUDDLE_NET_OFFLINE;
        for (int i = 0; i < n; i++) {
            puddle_net_reply r;
            attempt_at(hooks, it, &addrs[i], recipient, pub, handshake, &r);
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
            puddle_net_reply_free(&best);
            memset(&best, 0, sizeof best);
            continue;
        }
        break;
    }
    best.checked = pub != NULL;
    *reply = best;
}
