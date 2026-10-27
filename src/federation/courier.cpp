#include "federation/courier.h"

#include "federation/flow.h"
#include "federation/groupflow.h"
#include "federation/keysync.h"
#include "federation/profiles_api.h"

const puddle_courier puddle_federation_courier = {
    puddle_flow_request,
    puddle_flow_owe,
    puddle_profile_push,
    puddle_profile_changed,
    puddle_media_want,
    puddle_sync_after_roll,
    puddle_group_create,
    puddle_group_invite,
    puddle_group_accept,
    puddle_group_decline,
    puddle_group_leave,
    puddle_group_rename,
    puddle_group_send_event,
};
