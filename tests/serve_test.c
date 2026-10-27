#include "http/serve.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

/* ---- a scripted connection ---- */
typedef struct {
    const unsigned char *data;
    size_t len, pos;
    size_t chunk;            /* at most this many bytes per read */
    long close_at;           /* once this many bytes have been read, report the other side closed (-1 never) */
    long fail_at;            /* ...or report a timeout (-1 never) */
    long tick_from;          /* the fake clock advances 1s per read once this many bytes have been read */
    int64_t clock;
    char out[300000];
    size_t outlen;
    int reads;
} fake;

static fake *g_fake;
static int64_t now_cb(void) { return g_fake->clock; }

static int fake_read(void *io, void *buf, size_t cap)
{
    fake *f = io;
    f->reads++;
    if (f->tick_from >= 0 && (long)f->pos >= f->tick_from)
        f->clock++;
    if (f->fail_at >= 0 && (long)f->pos >= f->fail_at)
        return -1;
    if (f->close_at >= 0 && (long)f->pos >= f->close_at)
        return 0;
    size_t n = f->len - f->pos;
    if (n > cap) n = cap;
    if (n > f->chunk) n = f->chunk;
    if (n == 0)
        return 0;
    memcpy(buf, f->data + f->pos, n);
    f->pos += n;
    return (int)n;
}

static int fake_write(void *io, const void *buf, size_t len)
{
    fake *f = io;
    if (f->outlen + len < sizeof f->out) {
        memcpy(f->out + f->outlen, buf, len);
        f->outlen += len;
    }
    return 0;
}

/* ---- the Puddle side ---- */
static int owner_check(void *u, const char *a) { (void)u; return strcmp(a, "Bearer ok") == 0; }

static size_t body_limit(void *u, const puddle_request *r, int owner_ok)
{
    (void)u; (void)owner_ok;
    if (strcmp(r->path, "/small") == 0) return 10;
    if (strcmp(r->path, "/big") == 0) return 100000;
    if (strcmp(r->path, "/any") == 0) return PUDDLE_BODY_HARD_MAX * 2u; /* asks for more than is ever allowed */
    return 0;
}

static struct { int calls; size_t len; unsigned char body[PUDDLE_BODY_HARD_MAX + 1]; int owner; int nul_terminated; } seen;

static size_t route(void *u, const puddle_request *r, int owner_ok, const unsigned char *body, size_t n,
                    char *out, size_t cap, int *status)
{
    (void)u;
    seen.calls++;
    seen.len = n;
    seen.owner = owner_ok;
    if (n) { memcpy(seen.body, body, n); seen.nul_terminated = body[n] == '\0'; }
    if (strcmp(r->path, "/small") && strcmp(r->path, "/big") && strcmp(r->path, "/any")) {
        *status = 404;
        return puddle_http_error(404, out, cap);
    }
    *status = 200;
    return (size_t)snprintf(out, cap, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok");
}

static puddle_http_handler handler(void)
{
    puddle_http_handler h = { .owner_check = owner_check, .body_limit = body_limit, .route = route, .deadline_seconds = 15, .now = now_cb };
    return h;
}

/* a route that can answer with a file */
static size_t route_big(void *u, const puddle_request *r, int owner_ok, const unsigned char *body, size_t n, char *out, size_t cap, int *status,
                        puddle_big *big)
{
    (void)u; (void)owner_ok; (void)body; (void)n;
    if (!strcmp(r->path, "/file") || !strcmp(r->path, "/lost")) {
        big->len = 200000;
        big->data = malloc(big->len);
        for (size_t i = 0; i < big->len; i++) big->data[i] = (unsigned char)(i * 7 + 3);
        if (!strcmp(r->path, "/lost"))
            return 0; /* the route failed after making the file: it must still be freed */
        *status = 200;
        return puddle_http_head(200, "image/png", "X-Extra: 1\r\n", big->len, out, cap);
    }
    *status = 200;
    return puddle_http_response(200, "", "{\"plain\":true}", 14, out, cap);
}

/* run one request; returns the status sent */
static int run(fake *f, const void *data, size_t len, size_t chunk, puddle_serve_info *info)
{
    memset(f, 0, sizeof *f);
    f->data = data;
    f->len = len;
    f->chunk = chunk ? chunk : (size_t)-1;
    f->close_at = f->fail_at = f->tick_from = -1;
    f->clock = 1000;
    g_fake = f;
    memset(&seen, 0, offsetof(typeof(seen), body));
    puddle_http_handler h = handler();
    puddle_io io = { fake_read, fake_write, f };
    return puddle_http_serve(&h, &io, info);
}

static int run_s(fake *f, const char *s, size_t chunk) { return run(f, s, strlen(s), chunk, NULL); }

/* headers + body as one buffer */
static char *build(const char *head, const void *body, size_t n, size_t *out_len)
{
    size_t hl = strlen(head);
    char *r = malloc(hl + n + 1);
    memcpy(r, head, hl);
    if (n) memcpy(r + hl, body, n);
    *out_len = hl + n;
    return r;
}

#define POST(path, cl, extra) "POST " path " HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer ok\r\nContent-Length: " cl "\r\n" extra "\r\n"

int main(void)
{
    static fake f;
    puddle_serve_info info;
    size_t n;
    char *r;

    /* ---- plain requests ---- */
    CHECK(run(&f, "GET /small HTTP/1.1\r\nHost: x\r\n\r\n", 33, 0, &info) == 200);
    CHECK(seen.calls == 1 && seen.len == 0 && strcmp(info.method, "GET") == 0 && strcmp(info.path, "/small") == 0);
    CHECK(strcmp(info.auth, "no token") == 0 && info.status == 200 && info.body_len == 0);
    CHECK(strstr(f.out, "200 OK") != NULL);

    /* ---- a body that arrives with the headers, or after them, or a byte at a time ---- */
    r = build(POST("/small", "5", ""), "hello", 5, &n);
    CHECK(run(&f, r, n, 0, &info) == 200 && seen.len == 5 && memcmp(seen.body, "hello", 5) == 0 && seen.nul_terminated);
    CHECK(info.body_len == 5 && strcmp(info.auth, "owner token") == 0 && seen.owner);
    CHECK(run(&f, r, n, 1, &info) == 200 && seen.len == 5 && memcmp(seen.body, "hello", 5) == 0);   /* one byte per read */
    CHECK(run(&f, r, n, 7, &info) == 200 && seen.len == 5 && memcmp(seen.body, "hello", 5) == 0);   /* split mid-headers */
    CHECK(seen.calls == 1);
    free(r);

    /* binary, including NULs and a blank line, which must not be mistaken for the end of the headers */
    const unsigned char tricky[] = { 0, '\r', '\n', '\r', '\n', 0xff };
    r = build(POST("/small", "6", ""), tricky, 6, &n);
    for (size_t chunk = 0; chunk < 3; chunk++) {
        CHECK(run(&f, r, n, chunk == 2 ? 1 : 0, &info) == 200 && seen.len == 6 && memcmp(seen.body, tricky, 6) == 0);
    }
    free(r);

    /* ---- the size limit, decided before the body is read ---- */
    r = build(POST("/small", "10", ""), "0123456789", 10, &n);
    CHECK(run(&f, r, n, 0, &info) == 200 && seen.len == 10);                                         /* exactly at the limit */
    free(r);
    r = build(POST("/small", "11", ""), "0123456789A", 11, &n);
    CHECK(run(&f, r, n, 0, &info) == 413 && seen.calls == 0 && strstr(f.out, "413 Payload Too Large"));  /* the owner is told */
    free(r);
    /* a stranger is told nothing, and gets exactly what an unknown path gets */
    const char *stranger = "POST /small HTTP/1.1\r\nContent-Length: 11\r\n\r\n0123456789A";
    char stranger_out[600];
    CHECK(run_s(&f, stranger, 0) == 404 && seen.calls == 0);
    memcpy(stranger_out, f.out, f.outlen + 1);
    CHECK(run_s(&f, "GET /nowhere HTTP/1.1\r\n\r\n", 0) == 404 && strcmp(f.out, stranger_out) == 0);
    CHECK(run_s(&f, "POST /nowhere HTTP/1.1\r\nContent-Length: 1\r\n\r\nx", 0) == 404 && seen.calls == 0);   /* a body to a route with none */
    CHECK(run_s(&f, "POST /nowhere HTTP/1.1\r\nAuthorization: Bearer ok\r\nContent-Length: 1\r\n\r\nx", 0) == 413); /* the owner is told */

    /* an over-limit body is not read: its bytes are left unread */
    r = build(POST("/small", "50", ""), "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX", 50, &n);
    run(&f, r, n, 8, &info);                                            /* arriving 8 bytes at a time */
    CHECK(info.status == 413 && f.pos < n - 30);
    free(r);

    /* the limit a route asks for is itself capped; and a body of exactly the cap is read whole */
    char head[300];
    snprintf(head, sizeof head, "POST /any HTTP/1.1\r\nAuthorization: Bearer ok\r\nContent-Length: %u\r\n\r\n", PUDDLE_BODY_HARD_MAX + 1);
    CHECK(run_s(&f, head, 0) == 413 && seen.calls == 0);
    snprintf(head, sizeof head, "POST /any HTTP/1.1\r\nAuthorization: Bearer ok\r\nContent-Length: %u\r\n\r\n", PUDDLE_BODY_HARD_MAX);
    unsigned char *big = malloc(PUDDLE_BODY_HARD_MAX);
    for (size_t i = 0; i < PUDDLE_BODY_HARD_MAX; i++) big[i] = (unsigned char)(i * 31 + 7);
    r = build(head, big, PUDDLE_BODY_HARD_MAX, &n);
    CHECK(run(&f, r, n, 65536, &info) == 200 && seen.len == PUDDLE_BODY_HARD_MAX && memcmp(seen.body, big, seen.len) == 0);
    free(r);
    free(big);

    /* ---- no body, or none declared ---- */
    r = build(POST("/small", "0", ""), "", 0, &n);
    CHECK(run(&f, r, n, 0, &info) == 200 && seen.calls == 1 && seen.len == 0);
    free(r);
    CHECK(run_s(&f, "POST /small HTTP/1.1\r\nAuthorization: Bearer ok\r\n\r\n", 0) == 200 && seen.len == 0); /* no length: empty */

    /* ---- lengths that could make two parsers disagree are refused ---- */
    const char *bad[] = {
        POST("/small", "abc", ""), POST("/small", "-1", ""), POST("/small", "+5", ""), POST("/small", "1 2", ""),
        POST("/small", "0x10", ""), POST("/small", "99999999999", ""), POST("/small", "4294967296", ""),
        POST("/small", "5", "Content-Length: 5\r\n"),                    /* twice, even if the same */
        POST("/small", "5", "Transfer-Encoding: chunked\r\n"),           /* chunked is not supported */
        "POST /small HTTP/1.1\r\nAuthorization: Bearer ok\r\nTransfer-Encoding: chunked\r\n\r\n",
        POST("/small", "5", "Expect: 200-ok\r\n"),
        "POST /small HTTP/1.1\r\nAuthorization: Bearer ok\r\nContent-Length: \r\n\r\n",
        "GET /small HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello",           /* a body on a GET */
        "HEAD /small HTTP/1.1\r\nContent-Length: 1\r\n\r\nx",
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        if (run_s(&f, bad[i], 0) != 400 || seen.calls != 0) {
            fprintf(stderr, "FAIL case %zu should be 400: %.60s\n", i, bad[i]);
            failures++;
        }
    }
    CHECK(run_s(&f, "GET /small HTTP/1.1\r\nContent-Length: 0\r\n\r\n", 0) == 200); /* zero is fine */

    /* ---- the other side goes away, or stalls ---- */
    r = build(POST("/small", "10", ""), "0123456789", 10, &n);
    size_t hdr_len = n - 10;
    run(&f, r, hdr_len + 3, 0, &info);                                  /* closes after 3 of 10 body bytes */
    CHECK(info.status == 0 && f.outlen == 0 && seen.calls == 0);
    memset(&f, 0, sizeof f);
    f.data = (unsigned char *)r; f.len = n; f.chunk = hdr_len + 3; f.close_at = -1; f.fail_at = (long)hdr_len + 3; f.tick_from = -1; f.clock = 1000;
    g_fake = &f;
    memset(&seen, 0, offsetof(typeof(seen), body));
    { puddle_http_handler h = handler(); puddle_io io = { fake_read, fake_write, &f };
      CHECK(puddle_http_serve(&h, &io, &info) == 408 && seen.calls == 0 && strstr(f.out, "408 Request Timeout")); }  /* stalls: told so */
    free(r);

    /* a body dribbled in, slower than the deadline allows */
    r = build(POST("/small", "10", ""), "0123456789", 10, &n);
    hdr_len = n - 10;
    memset(&f, 0, sizeof f);
    f.data = (unsigned char *)r; f.len = n; f.chunk = 1; f.close_at = f.fail_at = -1; f.tick_from = (long)hdr_len; f.clock = 1000;
    g_fake = &f;
    memset(&seen, 0, offsetof(typeof(seen), body));
    { puddle_http_handler h = handler(); h.deadline_seconds = 4; puddle_io io = { fake_read, fake_write, &f };
      CHECK(puddle_http_serve(&h, &io, &info) == 408 && seen.calls == 0); }
    free(r);

    /* slowloris: headers trickled a byte at a time, never finishing */
    char slow[2000] = "GET /small HTTP/1.1\r\n";
    for (int i = 0; i < 60; i++) strcat(slow, "X-Slow: yes\r\n");
    memset(&f, 0, sizeof f);
    f.data = (unsigned char *)slow; f.len = strlen(slow); f.chunk = 1; f.close_at = f.fail_at = -1; f.tick_from = 0; f.clock = 1000;
    g_fake = &f;
    { puddle_http_handler h = handler(); puddle_io io = { fake_read, fake_write, &f };
      CHECK(puddle_http_serve(&h, &io, &info) == 408 && f.pos < strlen(slow)); }            /* gave up before the end */

    /* an idle connection that never sends anything costs nothing and gets nothing */
    CHECK(run_s(&f, "", 0) == 0 && f.outlen == 0);
    memset(&f, 0, sizeof f);
    f.data = (const unsigned char *)""; f.fail_at = 0; f.close_at = f.tick_from = -1; f.chunk = 1; g_fake = &f;
    { puddle_http_handler h = handler(); puddle_io io = { fake_read, fake_write, &f };
      CHECK(puddle_http_serve(&h, &io, &info) == 0 && f.outlen == 0); }

    /* ---- Expect: 100-continue ---- */
    r = build(POST("/small", "5", "Expect: 100-continue\r\n"), "hello", 5, &n);
    hdr_len = n - 5;
    CHECK(run(&f, r, hdr_len, 0, &info) == 0);                                   /* waits for the body: closed first */
    CHECK(strncmp(f.out, "HTTP/1.1 100 Continue\r\n\r\n", 25) == 0);              /* and was told to send it */
    CHECK(run(&f, r, n, 0, &info) == 200);                                        /* body came with the headers: no 100 needed */
    CHECK(strstr(f.out, "100 Continue") == NULL);
    free(r);
    /* over the limit: it is told no, and is not invited to send it */
    CHECK(run_s(&f, "POST /small HTTP/1.1\r\nAuthorization: Bearer ok\r\nContent-Length: 999\r\nExpect: 100-continue\r\n\r\n", 0) == 413);
    CHECK(strstr(f.out, "100 Continue") == NULL);
    CHECK(run_s(&f, "POST /small HTTP/1.1\r\nContent-Length: 999\r\nExpect: 100-continue\r\n\r\n", 0) == 404);
    CHECK(strstr(f.out, "100 Continue") == NULL);

    /* ---- one request per connection ---- */
    r = build(POST("/small", "5", ""), "helloGET /small HTTP/1.1\r\n\r\nGET /small HTTP/1.1\r\n\r\n", 5 + 52, &n);
    CHECK(run(&f, r, n, 0, &info) == 200 && seen.calls == 1 && seen.len == 5 && memcmp(seen.body, "hello", 5) == 0);
    CHECK(strstr(strstr(f.out, "ok") + 2, "HTTP/1.1") == NULL);                    /* only one answer */
    free(r);

    /* ---- headers: too big, malformed ---- */
    static char huge[9000];
    memset(huge, 'a', sizeof huge);
    memcpy(huge, "GET / HTTP/1.1\r\nX: ", 19);
    CHECK(run(&f, huge, sizeof huge, 0, &info) == 431);
    CHECK(run_s(&f, "garbage\r\n\r\n", 0) == 400 && strcmp(info.auth, "malformed") == 0 && strcmp(info.method, "-") == 0);

    /* ---- what gets logged ---- */
    const char *wrong = "GET /small HTTP/1.1\r\nAuthorization: Bearer nope\r\n\r\n";
    const char *right = "GET /small HTTP/1.1\r\nAuthorization: Bearer ok\r\n\r\n";
    CHECK(run(&f, wrong, strlen(wrong), 0, &info) == 200 && strcmp(info.auth, "wrong token") == 0 && !seen.owner);
    CHECK(run(&f, right, strlen(right), 0, &info) == 200 && strcmp(info.auth, "owner token") == 0 && seen.owner);

    /* ---- a file as the answer ---- */
    {
        puddle_http_handler h = handler();
        h.route_big = route_big;
        puddle_io io;
        const char *req = "GET /file HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer ok\r\n\r\n";
        memset(&f, 0, sizeof f);
        f.data = (const unsigned char *)req;
        f.len = strlen(req);
        f.chunk = (size_t)-1;
        f.close_at = f.fail_at = f.tick_from = -1;
        f.clock = 1000;
        g_fake = &f;
        io = (puddle_io){ fake_read, fake_write, &f };
        CHECK(puddle_http_serve(&h, &io, &info) == 200 && info.status == 200);
        const char *body = strstr(f.out, "\r\n\r\n");
        CHECK(body && strstr(f.out, "Content-Type: image/png\r\n") && strstr(f.out, "Content-Length: 200000\r\n") && strstr(f.out, "X-Extra: 1\r\n"));
        body += 4;
        CHECK(f.outlen == (size_t)(body - f.out) + 200000);                                     /* headers, then exactly the file */
        int same = 1;
        for (size_t i = 0; i < 200000; i++) same &= (unsigned char)body[i] == (unsigned char)(i * 7 + 3);
        CHECK(same);

        /* a route that makes a file and then fails: a plain 500, and the file is not sent */
        req = "GET /lost HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer ok\r\n\r\n";
        memset(&f, 0, sizeof f);
        f.data = (const unsigned char *)req;
        f.len = strlen(req);
        f.chunk = (size_t)-1;
        f.close_at = f.fail_at = f.tick_from = -1;
        f.clock = 1000;
        g_fake = &f;
        io = (puddle_io){ fake_read, fake_write, &f };
        CHECK(puddle_http_serve(&h, &io, &info) == 500 && f.outlen < 400 && strstr(f.out, "500"));

        /* an ordinary route through the same handler is as before */
        req = "GET /other HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer ok\r\n\r\n";
        memset(&f, 0, sizeof f);
        f.data = (const unsigned char *)req;
        f.len = strlen(req);
        f.chunk = 3; /* a byte or three at a time */
        f.close_at = f.fail_at = f.tick_from = -1;
        f.clock = 1000;
        g_fake = &f;
        io = (puddle_io){ fake_read, fake_write, &f };
        CHECK(puddle_http_serve(&h, &io, &info) == 200 && strstr(f.out, "{\"plain\":true}"));

        /* the head alone */
        char head[400];
        size_t hl = puddle_http_head(404, "text/plain", "", 12, head, sizeof head);
        CHECK(hl > 0 && strstr(head, "404 Not Found") && strstr(head, "Content-Length: 12\r\n") && !strcmp(head + hl - 4, "\r\n\r\n"));
        CHECK(puddle_http_head(200, "image/png", "", 5, head, 20) == 0);
    }

    return failures ? 1 : 0;
}
