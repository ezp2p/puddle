/* A small TLS server for tests of the outbound client and outbox: it behaves as told, and remembers what it
 * was sent. Header-only so each test brings it in. */
#ifndef PUDDLE_TEST_TLSSERVER_H
#define PUDDLE_TEST_TLSSERVER_H

#include "base/hash.h"
#include "http/http.h"
#include "http/serve.h"
#include "platform/fsutil.h"
#include "http/tls.h"
#include "http/client.h"

#include <netinet/in.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef enum {
    TS_NORMAL,   /* TLS, then answer with `status` and `reply` */
    TS_CLOSE,    /* accept the connection and drop it */
    TS_PLAIN,    /* answer in plain HTTP, no TLS */
    TS_HANG,     /* accept, then say nothing */
    TS_SLOW,     /* TLS, then wait `delay_ms` before reading the request */
    TS_BIG       /* TLS, then answer with a body over the limit */
} ts_mode;

typedef struct {
    int listen_fd, stop[2];
    unsigned port;
    pthread_t thread;
    SSL_CTX *ctx;
    char fingerprint[65];
    /* behaviour (set before or between calls; read by the thread under `mu`) */
    pthread_mutex_t mu;
    ts_mode mode;
    int status;
    char reply[256];
    int delay_ms;
    int first_n, first_status; /* the first `first_n` requests are answered with `first_status` instead */
    /* what it saw */
    int connections, requests;
    char method[8], target[256], signature[1100];
    unsigned char body[200000];
    size_t body_len;
    int busy;
} testsrv;

static int ts_owner(void *u, const char *a) { (void)u; (void)a; return 0; }
static size_t ts_limit(void *u, const puddle_request *r, int o) { (void)u; (void)r; (void)o; return 150000; }
static size_t ts_route(void *u, const puddle_request *r, int o, const unsigned char *body, size_t n, char *out, size_t cap, int *status)
{
    testsrv *s = u;
    (void)o;
    pthread_mutex_lock(&s->mu);
    s->requests++;
    snprintf(s->method, sizeof s->method, "%s", r->method);
    snprintf(s->target, sizeof s->target, "%s", r->target);
    snprintf(s->signature, sizeof s->signature, "%s", r->signature);
    s->body_len = n < sizeof s->body ? n : sizeof s->body;
    if (n) memcpy(s->body, body, s->body_len);
    int st = s->requests <= s->first_n ? s->first_status : s->status;
    ts_mode m = s->mode;
    char reply[256];
    snprintf(reply, sizeof reply, "%s", s->reply);
    pthread_mutex_unlock(&s->mu);
    *status = st;
    if (m == TS_BIG) {
        static char big[PUDDLE_NET_BODY_MAX + 5000];
        (void)big;
    }
    return puddle_http_response(st, "", reply, strlen(reply), out, cap);
}

static int ts_read(void *io, void *buf, size_t cap)
{
    SSL *ssl = io;
    int n = SSL_read(ssl, buf, (int)cap);
    return n > 0 ? n : 0;
}
static int ts_write(void *io, const void *buf, size_t len)
{
    SSL *ssl = io;
    return SSL_write(ssl, buf, (int)len) > 0 ? 0 : -1;
}

static void ts_serve_one2(testsrv *s, int fd);
static void ts_serve_one(testsrv *s, int fd)
{
    ts_serve_one2(s, fd);
    pthread_mutex_lock(&s->mu);
    s->busy = 0;
    pthread_mutex_unlock(&s->mu);
}

static void ts_serve_one2(testsrv *s, int fd)
{
    pthread_mutex_lock(&s->mu);
    s->connections++;
    s->busy = 1;
    ts_mode m = s->mode;
    int delay = s->delay_ms;
    pthread_mutex_unlock(&s->mu);
    if (m == TS_CLOSE) { close(fd); return; }
    if (m == TS_HANG) {
        struct pollfd p = { s->stop[0], POLLIN, 0 };
        poll(&p, 1, 3000);
        close(fd);
        return;
    }
    if (m == TS_PLAIN) {
        char buf[2048];
        ssize_t r = read(fd, buf, sizeof buf);
        (void)r;
        const char *a = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
        ssize_t w = write(fd, a, strlen(a));
        (void)w;
        close(fd);
        return;
    }
    SSL *ssl = SSL_new(s->ctx);
    SSL_set_fd(ssl, fd);
    if (SSL_accept(ssl) == 1) {
        if (m == TS_SLOW) usleep((useconds_t)delay * 1000);
        if (m == TS_BIG) {
            char *big = malloc(PUDDLE_NET_BODY_MAX + 20000);
            memset(big, 'x', PUDDLE_NET_BODY_MAX + 20000);
            char head[200];
            int hn = snprintf(head, sizeof head, "HTTP/1.1 200 OK\r\nContent-Length: %u\r\nConnection: close\r\n\r\n", PUDDLE_NET_BODY_MAX + 20000);
            char tmp[4096];
            SSL_read(ssl, tmp, sizeof tmp);
            SSL_write(ssl, head, hn);
            SSL_write(ssl, big, PUDDLE_NET_BODY_MAX + 20000);
            free(big);
        } else {
            puddle_http_handler h = { .owner_check = ts_owner, .body_limit = ts_limit, .route = ts_route, .user = s, .deadline_seconds = 5 };
            puddle_io io = { ts_read, ts_write, ssl };
            puddle_http_serve(&h, &io, NULL);
        }
        SSL_shutdown(ssl);
    }
    SSL_free(ssl);
    close(fd);
}

static void *ts_thread(void *arg)
{
    testsrv *s = arg;
    for (;;) {
        struct pollfd p[2] = { { s->listen_fd, POLLIN, 0 }, { s->stop[0], POLLIN, 0 } };
        if (poll(p, 2, -1) < 0) continue;
        if (p[1].revents) break;
        if (p[0].revents & POLLIN) {
            int fd = accept(s->listen_fd, NULL, NULL);
            if (fd >= 0) ts_serve_one(s, fd);
        }
    }
    return NULL;
}

/* `dir` holds the certificate (made on first use); `port` 0 = any free one */
static int ts_start(testsrv *s, const char *dir, unsigned port)
{
    signal(SIGPIPE, SIG_IGN);
    memset(s, 0, sizeof *s);
    pthread_mutex_init(&s->mu, NULL);
    s->status = 200;
    snprintf(s->reply, sizeof s->reply, "{\"ok\":true}");
    char err[200];
    puddle_mkdir_p(dir);
    s->ctx = puddle_tls_context(dir, err, sizeof err);
    if (!s->ctx) { fprintf(stderr, "tls: %s\n", err); return -1; }
    char path[900];
    snprintf(path, sizeof path, "%s/cert.pem", dir);
    FILE *f = fopen(path, "r");
    X509 *x = f ? PEM_read_X509(f, NULL, NULL, NULL) : NULL;
    if (f) fclose(f);
    unsigned char md[32];
    unsigned ml = 0;
    if (!x || X509_digest(x, EVP_sha256(), md, &ml) != 1) return -1;
    X509_free(x);
    puddle_hex_encode(md, 32, s->fingerprint);
    s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;
    setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((uint16_t)port);
    if (bind(s->listen_fd, (struct sockaddr *)&a, sizeof a) != 0 || listen(s->listen_fd, 16) != 0) return -1;
    socklen_t al = sizeof a;
    getsockname(s->listen_fd, (struct sockaddr *)&a, &al);
    s->port = ntohs(a.sin_port);
    if (pipe(s->stop) != 0) return -1;
    return pthread_create(&s->thread, NULL, ts_thread, s);
}

static void ts_stop(testsrv *s)
{
    if (s->listen_fd <= 0) return;
    ssize_t w = write(s->stop[1], "x", 1);
    (void)w;
    pthread_join(s->thread, NULL);
    close(s->listen_fd);
    close(s->stop[0]);
    close(s->stop[1]);
    SSL_CTX_free(s->ctx);
    s->listen_fd = 0;
}

/* a connection that ended in a client giving up may still be being "served"; let it finish */
static void ts_wait_idle(testsrv *s)
{
    for (int i = 0; i < 1000; i++) {
        pthread_mutex_lock(&s->mu);
        int b = s->busy;
        pthread_mutex_unlock(&s->mu);
        if (!b) return;
        usleep(10000);
    }
}

static void ts_set(testsrv *s, ts_mode mode, int status, const char *reply, int delay_ms)
{
    ts_wait_idle(s);
    pthread_mutex_lock(&s->mu);
    s->mode = mode;
    s->status = status;
    snprintf(s->reply, sizeof s->reply, "%s", reply ? reply : "");
    s->delay_ms = delay_ms;
    pthread_mutex_unlock(&s->mu);
}

#endif
