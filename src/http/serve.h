#ifndef PUDDLE_SERVE_H
#define PUDDLE_SERVE_H

/* Serving one HTTP request over any connection: read it, decide what to do about its body, read the body
 * under a size cap and a deadline, hand it to the router, write the answer. The connection is abstract
 * (TLS in the server, a scripted fake in tests). One request per connection; keep-alive and pipelining are
 * not supported, so anything after the first request is ignored. */

#include "http/http.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Never read more than this, whatever a route asks for. */
#define PUDDLE_BODY_HARD_MAX (4u << 20)

typedef struct {
    /* bytes read (> 0); 0 if the other side closed; < 0 on error or timeout */
    int (*read)(void *io, void *buf, size_t cap);
    /* 0 on success */
    int (*write)(void *io, const void *buf, size_t len);
    void *io;
} puddle_io;

typedef struct {
    /* Does this `Authorization` header carry the owner token? */
    int (*owner_check)(void *user, const char *authorization);

    /* How many body bytes this request may carry; 0 = none. Asked BEFORE the body is read, from the headers
     * alone, so the limit is what protects memory. Capped at PUDDLE_BODY_HARD_MAX. */
    size_t (*body_limit)(void *user, const puddle_request *req, int owner_ok);

    /* Build the response for a request whose body (NUL-terminated for convenience, binary-safe by length) has
     * been read. Returns the response length in `out`, and sets *status. */
    size_t (*route)(void *user, const puddle_request *req, int owner_ok, const unsigned char *body, size_t body_len,
                    char *out, size_t cap, int *status);

    /* Like `route`, for routes that can answer with a body too big for `out` (an image): it is handed back in `big`, and
     * sent after what is in `out`. If this is set it is used instead of `route`. */
    size_t (*route_big)(void *user, const puddle_request *req, int owner_ok, const unsigned char *body, size_t body_len,
                        char *out, size_t cap, int *status, puddle_big *big);

    void *user;
    int deadline_seconds;       /* for the whole request; 0 = 15 */
    int64_t (*now)(void);       /* seconds; NULL = time(NULL). A seam for tests. */
} puddle_http_handler;

typedef struct {
    int status;           /* what was sent; 0 if nothing was (the other side went away) */
    char method[8];       /* "-" if the request was malformed */
    char path[1024];
    const char *auth;     /* "owner token", "wrong token", "no token" or "malformed" */
    size_t body_len;
} puddle_serve_info;

/* Serve one request. Returns the status sent, or 0 if none was. `info` may be NULL. */
int puddle_http_serve(const puddle_http_handler *h, puddle_io *io, puddle_serve_info *info);

#ifdef __cplusplus
}
#endif
#endif
