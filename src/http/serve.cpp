#include "http/serve.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HEADER_MAX 8192
#define DEFAULT_DEADLINE_SECONDS 15
#define RESPONSE_MAX 32768 /* a profile's longest fields alone can pass 10 KB once escaped */

static int64_t system_now(void) { return (int64_t)time(NULL); }

/* offset just past the blank line ending the headers, or 0 if it has not arrived (binary-safe) */
static size_t header_end(const char *buf, size_t len)
{
    for (size_t i = 0; i + 4 <= len; i++)
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n')
            return i + 4;
    return 0;
}

static int send_status(puddle_io *io, int status, puddle_serve_info *info)
{
    char out[512];
    size_t n = puddle_http_error(status, out, sizeof out);
    if (n)
        io->write(io->io, out, n);
    if (info)
        info->status = status;
    return status;
}

typedef enum { BODY_OK, BODY_CLOSED, BODY_TIMEOUT } body_result;

/* Read exactly `want` bytes of body into body[], starting with whatever already arrived with the headers. */
static body_result read_body(puddle_io *io, unsigned char *body, size_t want, const unsigned char *already,
                             size_t already_len, int64_t deadline, int64_t (*now)(void))
{
    size_t have = already_len < want ? already_len : want; /* anything beyond belongs to no request we serve */
    if (have)
        memcpy(body, already, have);
    while (have < want) {
        if (now() > deadline)
            return BODY_TIMEOUT;
        int n = io->read(io->io, body + have, want - have);
        if (n == 0)
            return BODY_CLOSED;
        if (n < 0)
            return BODY_TIMEOUT;
        have += (size_t)n;
    }
    return BODY_OK;
}

int puddle_http_serve(const puddle_http_handler *h, puddle_io *io, puddle_serve_info *info)
{
    static const char CONTINUE[] = "HTTP/1.1 100 Continue\r\n\r\n";
    int64_t (*now)(void) = h->now ? h->now : system_now;
    int64_t deadline = now() + (h->deadline_seconds > 0 ? h->deadline_seconds : DEFAULT_DEADLINE_SECONDS);
    char buf[HEADER_MAX + 1];
    size_t len = 0, hend = 0;
    puddle_serve_info scratch;
    if (!info)
        info = &scratch;
    memset(info, 0, sizeof *info);
    snprintf(info->method, sizeof info->method, "-");
    snprintf(info->path, sizeof info->path, "-");
    info->auth = "malformed";

    /* ---- the header block ---- */
    while (!(hend = header_end(buf, len))) {
        if (len >= HEADER_MAX)
            return send_status(io, 431, info);
        int n = io->read(io->io, buf + len, HEADER_MAX - len);
        if (n == 0)
            return 0; /* closed before a full request */
        if (n < 0)
            return len ? send_status(io, 408, info) : 0; /* timed out: only worth answering if it began */
        len += (size_t)n;
        if (now() > deadline && !header_end(buf, len))
            return send_status(io, 408, info);
    }

    puddle_request req;
    if (puddle_http_parse(buf, hend, &req) != 0)
        return send_status(io, 400, info);
    int owner_ok = h->owner_check ? h->owner_check(h->user, req.authorization) : 0;
    snprintf(info->method, sizeof info->method, "%s", req.method);
    snprintf(info->path, sizeof info->path, "%s", req.path);
    info->auth = owner_ok ? "owner token" : req.authorization[0] ? "wrong token" : "no token";

    /* ---- what to do about a body, decided before reading any of it ---- */
    int is_read = strcmp(req.method, "GET") == 0 || strcmp(req.method, "HEAD") == 0;
    if (is_read && req.content_length > 0)
        return send_status(io, 400, info); /* a body on a request that has none to give is a smuggling shape */

    size_t limit = h->body_limit ? h->body_limit(h->user, &req, owner_ok) : 0;
    if (limit > PUDDLE_BODY_HARD_MAX)
        limit = PUDDLE_BODY_HARD_MAX;
    if (req.content_length > limit) {
        /* Not read at all. The owner is told why; anyone else gets exactly what an unknown path gets, so a
         * stranger can't learn which routes take bodies from the size at which they stop. */
        return send_status(io, owner_ok ? 413 : 404, info);
    }

    unsigned char *body = NULL;
    size_t body_len = req.content_length;
    if (body_len > 0) {
        body = (unsigned char *)malloc(body_len + 1);
        if (!body)
            return send_status(io, 503, info);
        size_t already_len = len - hend;
        if (req.expect_continue && already_len < body_len)
            io->write(io->io, CONTINUE, sizeof CONTINUE - 1);
        body_result r = read_body(io, body, body_len, (const unsigned char *)buf + hend, already_len, deadline, now);
        if (r != BODY_OK) {
            free(body);
            return r == BODY_TIMEOUT ? send_status(io, 408, info) : 0;
        }
        body[body_len] = '\0';
    }
    info->body_len = body_len;

    /* ---- the answer ---- */
    char out[RESPONSE_MAX];
    int status = 0;
    puddle_big big = { NULL, 0 };
    size_t outlen = 0;
    if (h->route_big)
        outlen = h->route_big(h->user, &req, owner_ok, body, body_len, out, sizeof out, &status, &big);
    else if (h->route)
        outlen = h->route(h->user, &req, owner_ok, body, body_len, out, sizeof out, &status);
    free(body);
    if (!outlen) {
        free(big.data);
        big.data = NULL;
        status = 500;
        outlen = puddle_http_error(500, out, sizeof out);
    }
    io->write(io->io, out, outlen);
    if (big.data) {
        io->write(io->io, big.data, big.len);
        free(big.data);
    }
    memset(out, 0, sizeof out); /* an answer can hold a recovery phrase; don't leave it on the stack */
    info->status = status;
    return status;
}
