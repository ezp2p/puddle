#ifndef PUDDLE_AUTH_H
#define PUDDLE_AUTH_H

/* The owner token: what the owner's own tools (the app, curl, later phone clients once paired) present
 * to use the owner API. Made on first run, stored in <data_dir>/state/owner-token (mode 0600).
 * Anything without it is answered as if the path did not exist. */

#include "storage/storage.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct puddle_auth puddle_auth;

/* Load the owner token, making one on first run (or replacing a damaged one). NULL + `err` on failure. The first keeps
 * it in `data_dir` (the files driver); the second in any storage, which must outlive it. */
puddle_auth *puddle_auth_open(const char *data_dir, char *err, size_t errlen);
puddle_auth *puddle_auth_open_with(puddle_storage *storage, char *err, size_t errlen);
void puddle_auth_close(puddle_auth *a);

/* Does an `Authorization` header value ("Bearer <token>") carry the owner token? 1 or 0. Constant time
 * in the token. NULL or empty is 0. */
int puddle_auth_check_bearer(const puddle_auth *a, const char *authorization);

/* Where the token file is ("" when the storage is not files), and the token itself (64 hex characters). Treat the
 * second as secret. */
const char *puddle_auth_token_path(const puddle_auth *a);
const char *puddle_auth_token(const puddle_auth *a);

#ifdef __cplusplus
}
#endif
#endif
