#ifndef PUDDLE_ROUTER_H
#define PUDDLE_ROUTER_H

/* The route table: method + path -> handler, and who may call it. Every route declares one of three ways a
 * caller proves itself, and the router checks it before the handler runs, so a handler never sees a request
 * that was not authenticated for it. A failed check is answered exactly like a path that does not exist.
 *
 *   OWNER  the request carries the owner token (the app, curl, a paired phone client)
 *   PEER   a signed request from a Puddle we already have a stream with (its current key is looked up)
 *   CHAIN  a signed request proven by a key chain carried in the request itself: a stranger asking for a
 *          stream, or a peer pushing its new keys after a roll. The route says how to find the key. */

#include "core/config.h"
#include "core/context.h"
#include "core/group.h"
#include "http/http.h"
#include "core/identity.h"
#include "base/json.h"
#include "core/profile.h"
#include "core/outbox.h"
#include "core/ratelimit.h"
#include "http/reqsig.h"
#include "storage/store.h"
#include "core/streams.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { PUDDLE_AUTH_OWNER, PUDDLE_AUTH_PEER, PUDDLE_AUTH_CHAIN, PUDDLE_AUTH_CHAIN_OPEN } puddle_auth_kind;

/* An answer that is a file, not JSON: a handler fills this (data is malloc'd; the router sends and frees it). */
typedef struct {
    unsigned char *data;
    size_t len;
    char mime[40];
} puddle_blob;

#define PUDDLE_PATH_PARAMS 3
#define PUDDLE_PARAM_MAX 72 /* an event id (64 hex) is the longest value a path segment carries */

typedef struct {
    puddle_ctx *ctx;
    puddle_blob *blob;                     /* set to answer with a file instead of JSON */
    const puddle_request *req;
    const unsigned char *body;             /* NUL-terminated past body_len, binary-safe by length */
    size_t body_len;
    puddle_auth_kind auth;
    const char *sender;                    /* PEER/CHAIN: the authenticated sender's user id; NULL for OWNER */
    /* the `{name:type}` segments of the route's path, in order, already checked against their type */
    char param_name[PUDDLE_PATH_PARAMS][16];
    char param[PUDDLE_PATH_PARAMS][PUDDLE_PARAM_MAX];
} puddle_call;

/* The value of the path segment `{name:...}`; "" if the route has none by that name. */
const char *puddle_param(const puddle_call *call, const char *name);

/* Write the JSON answer into `w` and return the HTTP status; write nothing for an empty body. */
typedef int (*puddle_handler)(const puddle_call *call, puddle_jw *w);

/* CHAIN routes: find the key a request names in the chain the body carries, verifying that chain. Return 1 and
 * fill `sign_pub` only if it checks out. */
typedef int (*puddle_chain_key_fn)(puddle_ctx *ctx, const unsigned char *body, size_t body_len,
                                   const char *sender_id, const char *kid, unsigned char sign_pub[32]);

/* A route's path is literal segments and typed `{name:type}` ones. A segment only matches a value of its type, so a
 * malformed id is answered like a path that does not exist:
 *   id     a user id or a group id (52 characters of base32)
 *   event  an event id (64 hex)
 *   token  an invite token (letters, digits, '-' and '_', up to 64)
 *   profile  one of the owner's profile ids (a-z, 0-9, '-' and '_', up to 32) */
typedef struct {
    const char *method;                    /* "GET" or "POST" only */
    const char *path;
    puddle_auth_kind auth;
    size_t body_max;                       /* 0 = takes no body */
    puddle_handler fn;
    puddle_chain_key_fn chain_key;         /* CHAIN only */
} puddle_route;

typedef struct {
    const puddle_route *routes;
    size_t count;
    puddle_ctx *ctx;
} puddle_router;

/* As puddle_router_handle, for the server: a file answer comes back in `big` (headers in `out`) instead of having to fit in it. */
size_t puddle_router_handle_big(const puddle_router *r, const puddle_request *req, int owner_ok, const unsigned char *body,
                                size_t body_len, char *out, size_t cap, int *status, puddle_big *big);

/* How many body bytes a request may carry, from its headers alone (asked before the body is read). A route the
 * caller could not use gets 0, so a body to it is refused the way an unknown path is. */
size_t puddle_router_body_limit(const puddle_router *r, const puddle_request *req, int owner_ok);

/* Authenticate, run the handler, build the full HTTP response. `owner_ok`: the request carried the owner token. */
size_t puddle_router_handle(const puddle_router *r, const puddle_request *req, int owner_ok,
                            const unsigned char *body, size_t body_len, char *out, size_t cap, int *status);

/* The time in milliseconds, from the context's clock when there is one (tests set it). */
int64_t puddle_call_now_ms(const puddle_call *call);

#define PUDDLE_QUERY_VALUE_MAX 80 /* room for a user id (52 characters) */

/* Read the query string of a request target. `keys` lists the parameters allowed; each found one is copied
 * (letters, digits, `_` and `-` only, up to 79 characters) to `values[i]`, and left "" if absent. A parameter not
 * in the list, repeated, empty, or with any other character is an error. 0 or -1. */
int puddle_query_parse(const char *target, const char *const *keys, size_t nkeys, char (*values)[PUDDLE_QUERY_VALUE_MAX]);

/* Helper for handlers: write {"error":"<code>"} and return `status`. */
int puddle_fail(puddle_jw *w, int status, const char *code);

/* An image from the media store, by the `?hash=<64 hex>` of the request: the hash (0), or -1 if the query is not exactly
 * that. Then answer with the file: 1, or 0 if it is not held (the handler says 404). */
int puddle_query_media_hash(const puddle_call *call, char out[PUDDLE_QUERY_VALUE_MAX]);
int puddle_answer_media(const puddle_call *call, const char *hash);

#ifdef __cplusplus
}
#endif
#endif
