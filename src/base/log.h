#ifndef PUDDLE_LOG_H
#define PUDDLE_LOG_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { PUDDLE_LOG_INFO, PUDDLE_LOG_WARN, PUDDLE_LOG_ERROR } puddle_log_level;

typedef void (*puddle_log_fn)(puddle_log_level level, const char *msg, void *user);

/* Replace the default stderr sink (NULL restores it). Call before starting
 * the server; the sink itself is invoked from any thread. */
void puddle_log_set_sink(puddle_log_fn fn, void *user);

#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
void puddle_log(puddle_log_level level, const char *fmt, ...);

#ifdef __cplusplus
}
#endif
#endif
