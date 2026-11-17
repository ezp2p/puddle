#ifndef PUDDLE_SERVICE_ROUTES_H
#define PUDDLE_SERVICE_ROUTES_H

/* Every route the Puddle serves: the owner API's tables (api/) and the stream API's (federation/), together. */

#include "http/router.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Built once, on first use; call it before starting threads. */
const puddle_route *puddle_all_routes(size_t *count);

#ifdef __cplusplus
}
#endif
#endif
