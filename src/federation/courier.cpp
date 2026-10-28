#include "federation/courier.h"

#include "federation/files.h"
#include "federation/flow.h"
#include "federation/groupflow.h"
#include "federation/keysync.h"
#include "federation/ponds.h"
#include "federation/profiles_api.h"

/* Keys that rolled reach the Ponds too, which are not peers. */
static int keys_rolled(puddle_ctx *ctx)
{
    int rc = puddle_sync_after_roll(ctx);
    puddle_ponds_keys_rolled(ctx);
    return rc;
}

const puddle_courier puddle_federation_courier = {
    puddle_flow_request,
    puddle_flow_owe,
    puddle_profile_push,
    puddle_profile_changed,
    puddle_media_want,
    keys_rolled,
    puddle_group_create,
    puddle_group_invite,
    puddle_group_accept,
    puddle_group_decline,
    puddle_group_leave,
    puddle_group_rename,
    puddle_group_send_event,
    puddle_file_fetch,
    puddle_file_request,
    puddle_file_answer,
    puddle_pond_join,
    puddle_pond_send,
    puddle_pond_history,
    puddle_pond_leave,
};
