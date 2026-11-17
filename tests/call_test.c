/* Calls (docs/calls.md, roadmap 8.1) between real Puddles in one process: set up over the stream with signed requests, carried
 * by WebRTC over real UDP on loopback. Ringing, accepting, Opus-sized frames both ways, hanging up, declining, busy, missed,
 * nobody answering, a friend's Puddle stopping mid-call, and that only a friend can ring. Skipped (77) without calls built. */
#include "federation/calls.h"
#include "federation/access.h"
#include "base/json.h"
#include "http/client.h"
#include "storage/media.h"
#include "core/config.h"
#include "core/outbox.h"
#include "core/phrase.h"
#include "service/server.h"
#include "core/streams.h"

#include <arpa/inet.h>
#include <ftw.h>
#include <netinet/in.h>
#include <openssl/ssl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static char root[512];
static int rm_cb(const char *p, const struct stat *sb, int t, struct FTW *f) { (void)sb; (void)t; (void)f; remove(p); return 0; }

typedef struct {
    char name[16];
    puddle_config cfg;
    puddle_server *srv;
    pthread_t th;
    char token[80];
    char uid[PUDDLE_USER_ID_LEN + 1];
    char link[300];
    char phrase[PUDDLE_PHRASE_LEN + 1];
    int running;
} node;

static unsigned free_port(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(fd, (struct sockaddr *)&a, sizeof a);
    socklen_t l = sizeof a;
    getsockname(fd, (struct sockaddr *)&a, &l);
    close(fd);
    return ntohs(a.sin_port);
}

static void *run_thread(void *arg)
{
    node *n = arg;
    puddle_server_run(n->srv);
    return NULL;
}

static int node_up(node *n)
{
    char err[300];
    n->srv = puddle_server_create(&n->cfg, err, sizeof err);
    if (!n->srv) { fprintf(stderr, "%s: %s\n", n->name, err); return -1; }
    char path[900];
    snprintf(path, sizeof path, "%s/state/owner-token", n->cfg.data_dir);
    FILE *f = fopen(path, "r");
    if (!f || !fgets(n->token, sizeof n->token, f)) return -1;
    fclose(f);
    n->token[strcspn(n->token, "\r\n")] = 0;
    pthread_create(&n->th, NULL, run_thread, n);
    n->running = 1;
    return 0;
}

static void node_down(node *n)
{
    if (!n->running) return;
    puddle_server_stop(n->srv);
    pthread_join(n->th, NULL);
    puddle_server_destroy(n->srv);
    n->srv = NULL;
    n->running = 0;
}

static int node_make(node *n, const char *name, const char *display)
{
    snprintf(n->name, sizeof n->name, "%s", name);
    memset(&n->cfg, 0, sizeof n->cfg);
    snprintf(n->cfg.dir, sizeof n->cfg.dir, "%s/%s", root, name);
    snprintf(n->cfg.data_dir, sizeof n->cfg.data_dir, "%s/%s/data", root, name);
    snprintf(n->cfg.address, sizeof n->cfg.address, "127.0.0.1");
    n->cfg.port = (uint16_t)free_port();
    char path[900], err[300], *phrase = n->phrase;
    mkdir(root, 0700);
    snprintf(path, sizeof path, "%s/%s", root, name);
    mkdir(path, 0700);
    snprintf(path, sizeof path, "%s/%s/profiles.yaml", root, name);
    FILE *f = fopen(path, "w");
    fprintf(f, "profiles:\n  - id: main\n    display_name: %s\n    avatar: avatar-main.png\n"
               "  - id: work\n    display_name: %s (work)\n    avatar: avatar-work.png\n    color: \"#336699\"\n", display, display);
    fclose(f);
    for (int k = 0; k < 2; k++) { /* a picture of their own, different for everyone and for each profile */
        char ipath[900];
        snprintf(ipath, sizeof ipath, "%s/%s/avatar-%s.png", root, name, k ? "work" : "main");
        f = fopen(ipath, "wb");
        fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
        for (int i = 0; i < 2000 + k * 500; i++) fputc(name[0] + k + i % 3, f);
        fclose(f);
    }
    snprintf(n->cfg.profile_files[0], PUDDLE_PATH_MAX, "%s", path);
    n->cfg.nprofile_files = 1;
    puddle_identity *id = puddle_identity_create(n->cfg.data_dir, phrase, err, sizeof err);
    if (!id) { fprintf(stderr, "%s: %s\n", name, err); return -1; }
    puddle_identity_id(id, n->uid);
    puddle_identity_close(id);
    puddle_identity_confirm(n->cfg.data_dir);
    if (node_up(n) != 0) return -1;
    snprintf(n->link, sizeof n->link, "127.0.0.1:%u", n->cfg.port);
    return 0;
}

/* ---- the owner's HTTPS client ---- */

static int api(node *n, const char *method, const char *path, const char *body, char *out, size_t cap)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(n->cfg.port);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) { close(fd); return -1; }
    struct timeval tv = { 10, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);
    SSL *ssl = SSL_new(ctx);
    SSL_set_fd(ssl, fd);
    int status = -1;
    if (SSL_connect(ssl) == 1) {
        char req[4096];
        size_t bl = body ? strlen(body) : 0;
        int rn = snprintf(req, sizeof req, "%s %s HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
                          method, path, n->token, bl, body ? body : "");
        SSL_write(ssl, req, rn);
        static char buf[70000];
        size_t len = 0;
        int r;
        while (len < sizeof buf - 1 && (r = SSL_read(ssl, buf + len, (int)(sizeof buf - 1 - len))) > 0) len += (size_t)r;
        buf[len] = 0;
        if (sscanf(buf, "HTTP/1.1 %d", &status) != 1) status = -1;
        const char *b = strstr(buf, "\r\n\r\n");
        if (out) snprintf(out, cap, "%s", b ? b + 4 : "");
    }
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    close(fd);
    return status;
}

static char resp[70000];

static int post(node *n, const char *path, const char *body) { return api(n, "POST", path, body, resp, sizeof resp); }
static int get(node *n, const char *path) { return api(n, "GET", path, NULL, resp, sizeof resp); }


static int json_has(const char *needle) { return strstr(resp, needle) != NULL; }

static int wait_for(int (*cond)(void *), void *arg)
{
    for (int i = 0; i < 300; i++) {
        if (cond(arg)) return 1;
        usleep(50000);
    }
    return 0;
}

typedef struct { node *n; const char *path; const char *needle; } has_arg;
static int has_cond(void *p)
{
    has_arg *a = p;
    return get(a->n, a->path) == 200 && json_has(a->needle);
}
/* wait until a GET on `n` has `needle` in its answer */
static int wait_has(node *n, const char *path, const char *needle)
{
    has_arg a = { n, path, needle };
    return wait_for(has_cond, &a);
}

static int stream_state_of(node *n, const char *peer, puddle_stream_state *out)
{
    puddle_stream st;
    if (puddle_streams_get(puddle_server_streams(n->srv), peer, &st) != 0) return 0;
    *out = st.state;
    puddle_stream_free(&st);
    return 1;
}
typedef struct { node *n; const char *peer; puddle_stream_state want; } state_arg;
static int state_cond(void *p)
{
    state_arg *a = p;
    puddle_stream_state s;
    return stream_state_of(a->n, a->peer, &s) && s == a->want;
}
static int wait_state(node *n, const char *peer, puddle_stream_state want)
{
    state_arg a = { n, peer, want };
    return wait_for(state_cond, &a);
}

static void befriend(node *from, node *to)
{
    char body[600];
    snprintf(body, sizeof body, "{\"target\":\"%s\",\"message\":\"hi\",\"profile\":\"main\"}", to->link);
    CHECK(post(from, "/api/v1/streams/request", body) == 201);
    has_arg a = { to, "/api/v1/streams?state=pending_in", from->uid };
    CHECK(wait_for(has_cond, &a));
    snprintf(body, sizeof body, "/api/v1/streams/%s/accept", from->uid);
    CHECK(post(to, body, "{\"profile\":\"main\"}") == 200);
    CHECK(wait_state(from, to->uid, PUDDLE_STREAM_ACTIVE));
}

/* ---- the media each owner's device would get ---- */

typedef struct {
    pthread_mutex_t mu;
    int frames, good;
    char call[32];
} heard;

static void on_frame(void *user, const char *call, const char *from, const unsigned char *frame, size_t len, uint32_t ts)
{
    (void)ts;
    (void)from;
    heard *h = user;
    pthread_mutex_lock(&h->mu);
    h->frames++;
    snprintf(h->call, sizeof h->call, "%s", call);
    /* what send_frames sent: its length and every byte from its number */
    int ok = len >= 40 && len <= 200;
    for (size_t i = 1; ok && i < len; i++)
        ok = frame[i] == (unsigned char)(frame[0] + i);
    h->good += ok;
    pthread_mutex_unlock(&h->mu);
}

static int frames_of(heard *h)
{
    pthread_mutex_lock(&h->mu);
    int n = h->good;
    pthread_mutex_unlock(&h->mu);
    return n;
}

static void send_frames(node *n, const char *call, int count, unsigned char seed)
{
    unsigned char f[200];
    for (int k = 0; k < count; k++) {
        size_t len = 40 + (size_t)(k * 7) % 150;
        f[0] = (unsigned char)(seed + k);
        for (size_t i = 1; i < len; i++) f[i] = (unsigned char)(f[0] + i);
        CHECK(puddle_call_send_audio(puddle_server_calls(n->srv), call, f, len) == 0);
        usleep(20000);
    }
}

/* ---- video: H.264-shaped access units (a parameter set, then a picture), big enough to be cut into many packets ---- */

typedef struct {
    pthread_mutex_t mu;
    int good, bad;
    char from[PUDDLE_USER_ID_LEN + 16]; /* an id, or "<id>/screen" */
} seen_video;

/* access unit `k`: SPS, PPS, then an IDR slice of 6000 bytes, none of them zero (so no start code inside) */
static size_t video_au(int k, unsigned char *out)
{
    static const unsigned char sc[4] = { 0, 0, 0, 1 };
    size_t n = 0;
    memcpy(out + n, sc, 4); n += 4; out[n++] = 0x67; out[n++] = 0x42; out[n++] = 0xc0; out[n++] = 0x1f;
    memcpy(out + n, sc, 4); n += 4; out[n++] = 0x68; out[n++] = 0xce; out[n++] = 0x3c; out[n++] = 0x80;
    memcpy(out + n, sc, 4); n += 4; out[n++] = 0x65; out[n++] = (unsigned char)(k + 1);
    for (int i = 0; i < 6000; i++) out[n++] = (unsigned char)(1 + (k * 31 + i) % 255);
    return n;
}

static void on_video(void *user, const char *call, const char *from, const unsigned char *frame, size_t len, uint32_t ts)
{
    (void)call;
    (void)ts;
    seen_video *v = user;
    unsigned char want[7000];
    int k = len > 21 ? frame[21] - 1 : -1;
    size_t n = k >= 0 ? video_au(k, want) : 0;
    pthread_mutex_lock(&v->mu);
    if (n == len && !memcmp(want, frame, len)) {
        v->good++;
        snprintf(v->from, sizeof v->from, "%s", from);
    } else {
        v->bad++;
    }
    pthread_mutex_unlock(&v->mu);
}

static int video_good(seen_video *v)
{
    pthread_mutex_lock(&v->mu);
    int n = v->good;
    pthread_mutex_unlock(&v->mu);
    return n;
}

static void send_video(node *n, const char *call, int count)
{
    unsigned char au[7000];
    for (int k = 0; k < count; k++) {
        size_t len = video_au(k, au);
        CHECK(puddle_call_send_video(puddle_server_calls(n->srv), call, au, len) == 0);
        usleep(66000);
    }
}

static void call_id(char out[32])
{
    const char *at = strstr(resp, "\"id\":\"");
    out[0] = 0;
    if (at) { memcpy(out, at + 6, 26); out[26] = 0; }
}

/* the call `id` on `n` says `needle` */
static int wait_call(node *n, const char *id, const char *needle)
{
    char path[200];
    snprintf(path, sizeof path, "/api/v1/calls/%s", id);
    return wait_has(n, path, needle);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    if (!puddle_calls_built()) {
        printf("call_test: skipped, built without calls (libdatachannel)\n");
        return 77;
    }
    const char *base = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/puddle-calls-XXXXXX", base ? base : "/tmp");
    if (!mkdtemp(root)) return 2;
    node a, b, c;
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b); memset(&c, 0, sizeof c);
    if (node_make(&a, "alice", "Alice") != 0 || node_make(&b, "bob", "Bob") != 0 || node_make(&c, "carol", "Carol") != 0) return 2;
    for (int i = 0; i < 100 && !(puddle_server_calls(a.srv) && puddle_server_calls(b.srv) && puddle_server_calls(c.srv)); i++)
        usleep(20000);
    CHECK(puddle_server_calls(a.srv) && puddle_server_calls(b.srv) && puddle_server_calls(c.srv));
    heard ha = { PTHREAD_MUTEX_INITIALIZER, 0, 0, "" }, hb = { PTHREAD_MUTEX_INITIALIZER, 0, 0, "" };
    puddle_calls_on_audio(puddle_server_calls(a.srv), on_frame, &ha);
    puddle_calls_on_audio(puddle_server_calls(b.srv), on_frame, &hb);
    char body[300], path[200], id[32];

    befriend(&a, &b);

    /* ---- only a friend: Carol has no stream with Bob ---- */
    snprintf(body, sizeof body, "{\"user\":\"%s\"}", b.uid);
    CHECK(post(&c, "/api/v1/calls/start", body) == 409 && json_has("no_stream"));
    CHECK(get(&b, "/api/v1/calls") == 200 && json_has("\"items\":[]"));
    CHECK(post(&a, "/api/v1/calls/start", "{\"user\":\"nobody\"}") == 400);

    /* ---- ringing, answering, talking, hanging up ---- */
    CHECK(post(&a, "/api/v1/calls/start", body) == 201 && json_has("\"state\":\"calling\"") && json_has("\"direction\":\"outgoing\""));
    call_id(id);
    CHECK(strlen(id) == 26);
    CHECK(wait_call(&b, id, "\"state\":\"ringing\""));
    CHECK(get(&b, "/api/v1/calls") == 200 && json_has(a.uid) && json_has("\"direction\":\"incoming\""));
    snprintf(path, sizeof path, "/api/v1/calls/%s/accept", id);
    CHECK(post(&a, path, NULL) == 404);                                  /* not Alice's to answer */
    CHECK(post(&b, path, "{}") == 200);                                /* with an empty body, as the window sends it */
    CHECK(wait_call(&a, id, "\"state\":\"active\""));
    CHECK(wait_call(&b, id, "\"state\":\"active\""));
    CHECK(wait_call(&a, id, "\"media\":true") && wait_call(&b, id, "\"media\":true"));
    CHECK(post(&b, path, NULL) == 404);                                  /* answered already */
    snprintf(body, sizeof body, "{\"user\":\"%s\"}", a.uid);
    CHECK(post(&b, "/api/v1/calls/start", body) == 409 && json_has("busy")); /* one call at a time */

    send_frames(&a, id, 50, 1);
    send_frames(&b, id, 50, 101);
    for (int i = 0; i < 100 && (frames_of(&hb) < 45 || frames_of(&ha) < 45); i++) usleep(20000);
    CHECK(frames_of(&hb) >= 45 && frames_of(&ha) >= 45);                /* over UDP: a few may go missing, none come wrong */
    CHECK(ha.frames == ha.good && hb.frames == hb.good && !strcmp(hb.call, id));

    /* ---- video beside it: whole access units, cut into packets and put back together on the way ---- */
    seen_video va = { PTHREAD_MUTEX_INITIALIZER, 0, 0, "" }, vb = va;
    puddle_calls_on_video(puddle_server_calls(a.srv), on_video, &va);
    puddle_calls_on_video(puddle_server_calls(b.srv), on_video, &vb);
    send_video(&a, id, 15);
    send_video(&b, id, 15);
    for (int i = 0; i < 100 && (video_good(&va) < 12 || video_good(&vb) < 12); i++) usleep(20000);
    CHECK(video_good(&vb) >= 12 && video_good(&va) >= 12 && va.bad == 0 && vb.bad == 0); /* over UDP: a few may go missing, none come wrong */
    CHECK(!strcmp(vb.from, a.uid) && !strcmp(va.from, b.uid));
    {
        unsigned char big[16];
        memset(big, 1, sizeof big);
        CHECK(puddle_call_send_video(puddle_server_calls(a.srv), "nosuchcallnosuchcallnosuch", big, sizeof big) == -1);
    }

    /* ---- a shared screen: its own track, and it comes as "<them>/screen" ---- */
    {
        seen_video vs = { PTHREAD_MUTEX_INITIALIZER, 0, 0, "" };
        puddle_calls_on_video(puddle_server_calls(b.srv), on_video, &vs);
        unsigned char au[7000];
        for (int k = 0; k < 12; k++) {
            size_t len = video_au(k, au);
            CHECK(puddle_call_send_screen(puddle_server_calls(a.srv), id, au, len) == 0);
            usleep(66000);
        }
        for (int i = 0; i < 100 && video_good(&vs) < 9; i++) usleep(20000);
        char want[PUDDLE_USER_ID_LEN + 16];
        snprintf(want, sizeof want, "%s/screen", a.uid);
        CHECK(video_good(&vs) >= 9 && vs.bad == 0 && !strcmp(vs.from, want));
        puddle_calls_on_video(puddle_server_calls(b.srv), on_video, &vb);
    }

    snprintf(path, sizeof path, "/api/v1/calls/%s/end", id);
    CHECK(post(&a, path, NULL) == 200 && json_has("\"state\":\"ended\"") && json_has("\"reason\":\"hung_up\""));
    CHECK(wait_call(&b, id, "\"reason\":\"hung_up\""));
    CHECK(puddle_call_send_audio(puddle_server_calls(a.srv), id, (const unsigned char *)"x", 1) == -1);
    CHECK(post(&a, path, NULL) == 404);                                  /* it is over */

    /* ---- declined ---- */
    snprintf(body, sizeof body, "{\"user\":\"%s\"}", b.uid);
    CHECK(post(&a, "/api/v1/calls/start", body) == 201);
    call_id(id);
    CHECK(wait_call(&b, id, "\"state\":\"ringing\""));
    snprintf(path, sizeof path, "/api/v1/calls/%s/decline", id);
    CHECK(post(&b, path, NULL) == 200 && json_has("\"reason\":\"declined\""));
    CHECK(wait_call(&a, id, "\"reason\":\"declined\""));

    /* ---- busy, and missed: Bob is being rung by Alice when Carol, a friend now, rings him too ---- */
    befriend(&c, &b);
    CHECK(post(&a, "/api/v1/calls/start", body) == 201);
    call_id(id);
    CHECK(wait_call(&b, id, "\"state\":\"ringing\""));
    CHECK(post(&c, "/api/v1/calls/start", body) == 201 && json_has("\"reason\":\"busy\""));
    snprintf(path, sizeof path, "/api/v1/calls/%s/end", id);
    CHECK(post(&a, path, NULL) == 200 && json_has("\"reason\":\"cancelled\""));
    CHECK(wait_call(&b, id, "\"reason\":\"missed\""));

    /* ---- nobody answers ---- */
    puddle_calls_set_timeouts(puddle_server_calls(a.srv), 1500, 0, 0);
    CHECK(post(&a, "/api/v1/calls/start", body) == 201);
    call_id(id);
    CHECK(wait_call(&a, id, "\"reason\":\"no_answer\""));
    CHECK(wait_call(&b, id, "\"reason\":\"missed\""));

    /* ---- a friend's Puddle stops mid-call ---- */
    CHECK(post(&a, "/api/v1/calls/start", body) == 201);
    call_id(id);
    CHECK(wait_call(&b, id, "\"state\":\"ringing\""));
    snprintf(path, sizeof path, "/api/v1/calls/%s/accept", id);
    CHECK(post(&b, path, NULL) == 200);
    CHECK(wait_call(&a, id, "\"state\":\"active\""));
    node_down(&b);
    CHECK(wait_call(&a, id, "\"state\":\"ended\""));
    CHECK(get(&a, "/api/v1/calls") == 200 && !json_has("\"state\":\"active\""));

    /* ---- the history: per friend, and kept across a restart ---- */
    snprintf(path, sizeof path, "/api/v1/calls?user=%s", b.uid);
    CHECK(get(&a, path) == 200 && json_has("\"version\":") && json_has("\"reason\":\"declined\"") && json_has("\"reason\":\"no_answer\"") &&
          json_has("\"reason\":\"hung_up\"") && json_has("\"reason\":\"cancelled\""));
    snprintf(path, sizeof path, "/api/v1/calls?user=%s", c.uid);
    CHECK(get(&a, path) == 200 && json_has("\"items\":[]"));                 /* Alice never called Carol */
    CHECK(get(&c, "/api/v1/calls") == 200 && json_has("\"reason\":\"busy\""));
    {
        CHECK(get(&a, "/api/v1/calls?limit=1") == 200);
        int n = 0;
        for (const char *p = resp; (p = strstr(p, "\"direction\":")); p++) n++;
        CHECK(n == 1);
    }
    CHECK(get(&a, "/api/v1/calls?user=nobody") == 400);
    node_down(&a);
    CHECK(node_up(&a) == 0);
    for (int i = 0; i < 100 && !puddle_server_calls(a.srv); i++) usleep(20000);
    snprintf(path, sizeof path, "/api/v1/calls?user=%s", b.uid);
    CHECK(get(&a, path) == 200 && json_has("\"reason\":\"declined\"") && json_has("\"reason\":\"no_answer\""));

    node_down(&a);
    node_down(&c);
    nftw(root, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    if (failures) {
        fprintf(stderr, "call_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("call_test: ok\n");
    return 0;
}
