#ifndef PUDDLE_COURIER_H
#define PUDDLE_COURIER_H

/* The courier this layer provides: what puts the owner's news on its way to other Puddles. service/server.cpp sets
 * it in the context (`ctx.courier = &puddle_federation_courier`), and so do tests that need it. */

#include "core/context.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const puddle_courier puddle_federation_courier;

#ifdef __cplusplus
}
#endif
#endif
