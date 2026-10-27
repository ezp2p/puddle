#include "core/delivery.h"

#include <stdlib.h>

void puddle_net_reply_free(puddle_net_reply *r)
{
    free(r->body);
    r->body = NULL;
    r->body_len = 0;
}
