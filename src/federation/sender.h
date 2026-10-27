#ifndef PUDDLE_SENDER_H
#define PUDDLE_SENDER_H

/* How the outbox's items travel: a signed HTTPS request to each address known for the peer, until one answers.
 * Set as the outbox's `send` hook (service/server.cpp, and tests that deliver). */

#include "core/outbox.h"

#ifdef __cplusplus
extern "C" {
#endif

void puddle_federation_send(const puddle_outbox_hooks *hooks, const puddle_ob_item *item, puddle_net_reply *reply);

#ifdef __cplusplus
}
#endif
#endif
