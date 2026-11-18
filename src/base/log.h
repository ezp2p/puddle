#ifndef PUDDLE_LOG_H
#define PUDDLE_LOG_H

/* The log: what the Puddle tells its owner (stderr, so journald, launchd or `docker logs`; the app's Activity in the window).
 *
 *   debug     the detail of what it does, for working out a problem: every request, connections that went nowhere
 *   info      what happened that the owner may want to know: a friend's request, a call, a phone let in
 *   warn      something didn't work, and the Puddle carries on: a record it skipped, something it will try again
 *   error     something failed that the owner should look at; the Puddle still runs
 *   critical  the Puddle can't go on, and stops
 *
 * Only what is at the chosen level or above is written: `log:` in puddle.yaml, or PUDDLE_LOG (docs/hardening.md, "The log"). */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { PUDDLE_LOG_DEBUG, PUDDLE_LOG_INFO, PUDDLE_LOG_WARN, PUDDLE_LOG_ERROR, PUDDLE_LOG_CRITICAL } puddle_log_level;

typedef void (*puddle_log_fn)(puddle_log_level level, const char *msg, void *user);

/* Replace the default stderr sink (NULL restores it). Call before starting
 * the server; the sink itself is invoked from any thread. */
void puddle_log_set_sink(puddle_log_fn fn, void *user);

/* The lowest level written (PUDDLE_LOG_INFO to begin with). Anything below it is dropped before it is even formatted. */
void puddle_log_set_level(puddle_log_level level);
puddle_log_level puddle_log_get_level(void);
/* Would a message at `level` be written? For a caller that has work to do to make one. */
int puddle_log_enabled(puddle_log_level level);

/* "debug", "info", "warn" (or "warning"), "error", "critical", any case: 0 and `*out`, or -1. And the other way. */
int puddle_log_level_parse(const char *text, puddle_log_level *out);
const char *puddle_log_level_name(puddle_log_level level);

#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
void puddle_log(puddle_log_level level, const char *fmt, ...);

#ifdef __cplusplus
}
#endif
#endif
