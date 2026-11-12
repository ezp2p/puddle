#ifndef PUDDLE_GROUPAPI_H
#define PUDDLE_GROUPAPI_H

#include "http/router.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Group chats: the routes other Puddles use (groupapi.cpp). The owner's are in api/groups.cpp and api/conversations.cpp. */
extern const puddle_route puddle_group_routes[];
extern const size_t puddle_group_routes_count;

#ifdef __cplusplus
}
#endif
#endif
