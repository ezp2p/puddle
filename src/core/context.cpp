#include "core/context.h"

#include "platform/clock.h"

int64_t puddle_ctx_now_ms(const puddle_ctx *ctx)
{
    if (ctx->now_ms)
        return ctx->now_ms();
    return puddle_wall_ms();
}
