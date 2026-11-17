#ifndef PUDDLE_INBOUND_H
#define PUDDLE_INBOUND_H

/* What other Puddles send us about a stream (api/federation.yaml): the answer to a request we made, a question
 * about one they made, and the news that a stream is closed. The request for a stream itself is access.c. */

#include "http/router.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const puddle_route puddle_inbound_routes[];
extern const size_t puddle_inbound_routes_count;

#ifdef __cplusplus
}
#endif
#endif
