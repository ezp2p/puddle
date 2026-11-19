#include "base/log.h"

#include <atomic>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "base/text.h"

static puddle_log_fn g_sink;
static void *g_user;
static std::atomic<int> g_level{ PUDDLE_LOG_INFO };

static const char *const names[] = { "debug", "info", "warn", "error", "critical" };

static bool same_word(const char *a, const char *b)
{
    return strlen(a) == strlen(b) && puddle_ascii_ncasecmp(a, b, strlen(b)) == 0;
}

void puddle_log_set_sink(puddle_log_fn fn, void *user)
{
    g_sink = fn;
    g_user = user;
}

void puddle_log_set_level(puddle_log_level level)
{
    g_level = level;
}

puddle_log_level puddle_log_get_level(void)
{
    return (puddle_log_level)g_level.load();
}

int puddle_log_enabled(puddle_log_level level)
{
    return level >= g_level.load();
}

int puddle_log_level_parse(const char *text, puddle_log_level *out)
{
    if (!text)
        return -1;
    for (int i = 0; i <= PUDDLE_LOG_CRITICAL; i++)
        if (same_word(text, names[i])) {
            *out = (puddle_log_level)i;
            return 0;
        }
    if (same_word(text, "warning")) {
        *out = PUDDLE_LOG_WARN;
        return 0;
    }
    return -1;
}

const char *puddle_log_level_name(puddle_log_level level)
{
    return level >= PUDDLE_LOG_DEBUG && level <= PUDDLE_LOG_CRITICAL ? names[level] : "info";
}

void puddle_log(puddle_log_level level, const char *fmt, ...)
{
    if (!puddle_log_enabled(level))
        return;
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
    fprintf(stderr, "%s: %s\n", names[level], msg);
}
