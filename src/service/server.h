#ifndef PUDDLE_SERVER_H
#define PUDDLE_SERVER_H

#include "core/config.h"
#include "core/identity.h"
#include "core/profile.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct puddle_server puddle_server;

/* Open the data directory (locked: a second Puddle on the same directory fails), load/generate
 * TLS material and bind the listening socket. NULL + message in `err` on failure. */
puddle_server *puddle_server_create(const puddle_config *cfg, char *err, size_t errlen);

struct puddle_store;
/* Opened by puddle_server_create; valid until puddle_server_destroy. */
struct puddle_store *puddle_server_store(puddle_server *s);
struct puddle_profiles *puddle_server_profiles(puddle_server *s);
puddle_identity *puddle_server_identity(puddle_server *s);
puddle_assignments *puddle_server_assignments(puddle_server *s);
struct puddle_streams *puddle_server_streams(puddle_server *s);
struct puddle_people *puddle_server_people(puddle_server *s);
struct puddle_outbox *puddle_server_outbox(puddle_server *s);
struct puddle_groups *puddle_server_groups(puddle_server *s);

/* Accept loop; blocks until puddle_server_stop(), then drains in-flight
 * connections. Returns 0 on clean stop. */
int puddle_server_run(puddle_server *s);

/* Ask run() to return. Thread-safe and async-signal-safe. */
void puddle_server_stop(puddle_server *s);

/* Only after run() has returned (or was never called). */
void puddle_server_destroy(puddle_server *s);

#ifdef __cplusplus
}
#endif
#endif
