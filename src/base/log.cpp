#include "base/log.h"

#include <stdarg.h>
#include <stdio.h>

static puddle_log_fn g_sink;
static void *g_user;

void puddle_log_set_sink(puddle_log_fn fn, void *user)
{
    g_sink = fn;
    g_user = user;
}

void puddle_log(puddle_log_level level, const char *fmt, ...)
{
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    if (g_sink) {
        g_sink(level, msg, g_user);
        return;
    }
    /* stderr is captured by journald/launchd, which add their own timestamps. */
    static const char *const names[] = { "info", "warn", "error" };
    fprintf(stderr, "%s: %s\n", names[level], msg);
}
