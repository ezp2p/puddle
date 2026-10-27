#include "service/routes.h"

#include "api/api.h"
#include "federation/access.h"
#include "federation/groupapi.h"
#include "federation/inbound.h"
#include "federation/keysync.h"
#include "federation/messages.h"
#include "federation/profiles_api.h"

#include <string.h>

const puddle_route *puddle_all_routes(size_t *count)
{
    static puddle_route all[96];
    static size_t n;
    if (n == 0) {
        static const struct { const puddle_route *routes; const size_t *count; } tables[] = {
            { puddle_routes, &puddle_routes_count },
            { puddle_access_routes, &puddle_access_routes_count },
            { puddle_user_routes, &puddle_user_routes_count },
            { puddle_stream_api_routes, &puddle_stream_api_routes_count },
            { puddle_conversation_routes, &puddle_conversation_routes_count },
            { puddle_group_api_routes, &puddle_group_api_routes_count },
            { puddle_profile_api_routes, &puddle_profile_api_routes_count },
            { puddle_inbound_routes, &puddle_inbound_routes_count },
            { puddle_message_routes, &puddle_message_routes_count },
            { puddle_sync_routes, &puddle_sync_routes_count },
            { puddle_profile_routes, &puddle_profile_routes_count },
            { puddle_group_routes, &puddle_group_routes_count },
        };
        for (size_t t = 0; t < sizeof tables / sizeof tables[0]; t++) {
            memcpy(all + n, tables[t].routes, *tables[t].count * sizeof *all);
            n += *tables[t].count;
        }
    }
    *count = n;
    return all;
}
