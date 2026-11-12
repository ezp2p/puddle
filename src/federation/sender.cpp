#include "federation/sender.h"

#include "http/client.h"
#include "http/reqsig.h"

#include <stdio.h>
#include <string.h>

/* ---- one delivery: try each address we know, until one answers ---- */

typedef struct {
    const puddle_outbox_hooks *hooks;
    const char *peer;
} sender_fp;

static int sender_fingerprint_ok(void *u, const char *fp)
{
    sender_fp *c = (sender_fp *)u;
    return c->hooks->fingerprint ? c->hooks->fingerprint(c->hooks->user, c->peer, fp) : 1;
}

void puddle_federation_send(const puddle_outbox_hooks *hooks, const puddle_ob_item *it, puddle_net_reply *reply)
{
    puddle_address addrs[4];
    int n = hooks->addresses ? hooks->addresses(hooks->user, it->peer, addrs, 4) : 0;
    if (n <= 0) {
        memset(reply, 0, sizeof *reply);
        reply->result = PUDDLE_NET_FAILED;
        snprintf(reply->reason, sizeof reply->reason, "we have no address for them");
        return;
    }
    sender_fp fc = { hooks, it->peer };
    puddle_net_opts opts;
    memset(&opts, 0, sizeof opts);
    opts.connect_timeout_s = hooks->net.connect_timeout_s;
    opts.io_timeout_s = hooks->net.io_timeout_s;
    opts.total_timeout_s = hooks->net.total_timeout_s;
    opts.fingerprint_ok = sender_fingerprint_ok;
    opts.user = &fc;
    if (it->max_reply)
        opts.max_body = it->max_reply;
    for (int i = 0; i < n; i++) {
        /* a first request to an address says who it is from but not who it is for: nobody has said who lives there yet */
        const char *recipient = !strcmp(it->kind, PUDDLE_OB_KIND_OPEN_ACCESS) ? PUDDLE_RECIPIENT_ANY : it->peer;
        puddle_net_request(&addrs[i], it->method, it->path, it->body_len ? it->body : NULL, it->body_len, hooks->identity, recipient, &opts, reply);
        if (reply->result != PUDDLE_NET_OFFLINE)
            return; /* reached something: that is the answer */
        if (i + 1 < n)
            puddle_net_reply_free(reply);
    }
}
