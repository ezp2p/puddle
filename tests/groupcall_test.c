/* Group calls (docs/calls.md, "Group calls", roadmap 8.4) between real Puddles in one process: a call per pair of the members
 * in it, over the group's connections (Bob and Carol have never met: theirs is a group-only stream), WebRTC over real UDP on
 * loopback. Starting, ringing, joining, frames between every pair marked with whose they are, leaving and coming back, busy,
 * two starting at once ending up in one call, nobody coming, and that only a member can join. Skipped (77) without calls. */
#include "core/group.h"
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
        char *buf = malloc(70000); /* not static: two owners ask at once below */
        size_t len = 0;
        int r;
        while (len < 70000 - 1 && (r = SSL_read(ssl, buf + len, (int)(70000 - 1 - len))) > 0) len += (size_t)r;
        buf[len] = 0;
        if (sscanf(buf, "HTTP/1.1 %d", &status) != 1) status = -1;
        const char *b = strstr(buf, "\r\n\r\n");
        if (out) snprintf(out, cap, "%s", b ? b + 4 : "");
        free(buf);
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


/* ---- the media each owner's device would get, by whose it is ---- */

typedef struct {
    pthread_mutex_t mu;
    char from[4][PUDDLE_USER_ID_LEN + 1];
    int good[4], bad;
} heard;

static void on_frame(void *user, const char *call, const char *from, const unsigned char *frame, size_t len, uint32_t ts)
{
    (void)call;
    (void)ts;
    heard *h = user;
    int ok = len >= 40 && len <= 200;
    for (size_t i = 1; ok && i < len; i++)
        ok = frame[i] == (unsigned char)(frame[0] + i);
    pthread_mutex_lock(&h->mu);
    int k = 0;
    while (k < 4 && h->from[k][0] && strcmp(h->from[k], from)) k++;
    if (k < 4 && ok) {
        snprintf(h->from[k], sizeof h->from[k], "%s", from);
        h->good[k]++;
    } else {
        h->bad++;
    }
    pthread_mutex_unlock(&h->mu);
}

static int heard_from(heard *h, const char *from)
{
    pthread_mutex_lock(&h->mu);
    int n = 0;
    for (int k = 0; k < 4; k++)
        if (!strcmp(h->from[k], from)) n = h->good[k];
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

static void wait_heard(heard *h, const char *from, int at_least)
{
    for (int i = 0; i < 150 && heard_from(h, from) < at_least; i++) usleep(20000);
    CHECK(heard_from(h, from) >= at_least);
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

static int wait_call(node *n, const char *id, const char *needle)
{
    char path[200];
    snprintf(path, sizeof path, "/api/v1/calls/%s", id);
    return wait_has(n, path, needle);
}

/* how many of the others in the call `n` is connected with */
static int connected_count(node *n, const char *id)
{
    char path[200];
    snprintf(path, sizeof path, "/api/v1/calls/%s", id);
    if (get(n, path) != 200) return -1;
    int k = 0;
    for (const char *p = resp; (p = strstr(p, "\"media\":true,\"slot\"")); p++) k++;
    return k;
}

static int wait_connected(node *n, const char *id, int want)
{
    for (int i = 0; i < 300; i++) {
        if (connected_count(n, id) == want) return 1;
        usleep(50000);
    }
    return 0;
}

static int wait_without(node *n, const char *id, const char *needle)
{
    char path[200];
    snprintf(path, sizeof path, "/api/v1/calls/%s", id);
    for (int i = 0; i < 300; i++) {
        if (get(n, path) == 200 && !json_has(needle)) return 1;
        usleep(50000);
    }
    return 0;
}

/* the group call `n` is in: the first in its list for the group */
static void current_of(node *n, const char *gid, char out[32], char state[16])
{
    char path[200];
    snprintf(path, sizeof path, "/api/v1/calls?group=%s&limit=1", gid);
    out[0] = state[0] = 0;
    if (get(n, path) != 200) return;
    const char *at = strstr(resp, "\"items\":[{\"id\":\"");
    if (!at) return;
    memcpy(out, at + 16, 26);
    out[26] = 0;
    const char *st = strstr(at, "\"state\":\"");
    if (st) sscanf(st + 9, "%15[a-z]", state);
}

typedef struct { node *n; char body[200]; int status; char out[4096]; } starter;
static void *start_thread(void *p)
{
    starter *s = p;
    s->status = api(s->n, "POST", "/api/v1/calls/start", s->body, s->out, sizeof s->out);
    return NULL;
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    if (!puddle_calls_built()) {
        printf("groupcall_test: skipped, built without calls (libdatachannel)\n");
        return 77;
    }
    const char *base = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/puddle-groupcalls-XXXXXX", base ? base : "/tmp");
    if (!mkdtemp(root)) return 2;
    node a, b, c, d;
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b); memset(&c, 0, sizeof c); memset(&d, 0, sizeof d);
    if (node_make(&a, "alice", "Alice") != 0 || node_make(&b, "bob", "Bob") != 0 || node_make(&c, "carol", "Carol") != 0 ||
        node_make(&d, "dave", "Dave") != 0)
        return 2;
    node *all[4] = { &a, &b, &c, &d };
    for (int k = 0; k < 4; k++) {
        for (int i = 0; i < 100 && !puddle_server_calls(all[k]->srv); i++) usleep(20000);
        CHECK(puddle_server_calls(all[k]->srv));
        puddle_calls_set_timeouts(puddle_server_calls(all[k]->srv), 4000, 0, 0); /* ringing, and alone in a call, 4 s */
        puddle_calls_set_beat(puddle_server_calls(all[k]->srv), 1000);
    }
    heard ha = { PTHREAD_MUTEX_INITIALIZER, {""}, {0}, 0 }, hb = ha, hc = ha;
    puddle_calls_on_audio(puddle_server_calls(a.srv), on_frame, &ha);
    puddle_calls_on_audio(puddle_server_calls(b.srv), on_frame, &hb);
    puddle_calls_on_audio(puddle_server_calls(c.srv), on_frame, &hc);
    char body[300], path[200], id[32], url[300];

    /* alice is friends with all three; bob and carol have never met; dave is in no group */
    befriend(&a, &b);
    befriend(&a, &c);
    befriend(&a, &d);
    char gid[64];
    snprintf(body, sizeof body, "{\"name\":\"Trio\",\"members\":[\"%s\",\"%s\"],\"profile\":\"main\"}", b.uid, c.uid);
    CHECK(post(&a, "/api/v1/groups/create", body) == 201);
    {
        const char *at = strstr(resp, "\"id\":\"");
        memcpy(gid, at + 6, 52);
        gid[52] = 0;
    }
    CHECK(wait_has(&b, "/api/v1/groups", "\"my\":\"invited\"") && wait_has(&c, "/api/v1/groups", "\"my\":\"invited\""));
    snprintf(url, sizeof url, "/api/v1/groups/%s/accept", gid);
    CHECK(post(&b, url, "{\"profile\":\"main\"}") == 200);
    CHECK(post(&c, url, "{\"profile\":\"main\"}") == 200);
    CHECK(wait_state(&c, b.uid, PUDDLE_STREAM_SCOPED) && wait_state(&b, c.uid, PUDDLE_STREAM_SCOPED));
    for (int i = 0; i < 200; i++) { /* until each has the other two as members */
        int ok = 1;
        for (int k = 0; k < 3; k++) {
            puddle_group g;
            if (puddle_groups_get(puddle_server_groups(all[k]->srv), gid, &g) != 0) { ok = 0; continue; }
            ok = ok && puddle_group_count(&g, PUDDLE_GM_MEMBER) == 3;
            puddle_group_free(&g);
        }
        if (ok) break;
        usleep(50000);
    }

    /* ---- only a member can start or join one ---- */
    snprintf(body, sizeof body, "{\"group\":\"%s\"}", gid);
    CHECK(post(&d, "/api/v1/calls/start", body) == 404 && json_has("no_group"));
    CHECK(post(&a, "/api/v1/calls/start", "{\"group\":\"nope\"}") == 400);
    snprintf(path, sizeof path, "{\"group\":\"%s\",\"user\":\"%s\"}", gid, b.uid);
    CHECK(post(&a, "/api/v1/calls/start", path) == 400);                  /* one or the other */

    /* ---- alice starts one: bob and carol are rung ---- */
    CHECK(post(&a, "/api/v1/calls/start", body) == 201 && json_has("\"state\":\"calling\"") && json_has(gid) &&
          json_has("\"members\":[]"));
    call_id(id);
    CHECK(strlen(id) == 26);
    CHECK(wait_call(&b, id, "\"state\":\"ringing\"") && json_has(a.uid) && json_has(gid));
    CHECK(wait_call(&c, id, "\"state\":\"ringing\""));
    snprintf(path, sizeof path, "/api/v1/calls?group=%s", gid);
    CHECK(get(&b, path) == 200 && json_has(id));
    snprintf(path, sizeof path, "/api/v1/calls?user=%s", a.uid);
    CHECK(get(&b, path) == 200 && !json_has(id));                          /* a friend's calls are only theirs */

    /* ---- bob joins: alice and bob talk ---- */
    snprintf(path, sizeof path, "/api/v1/calls/%s/accept", id);
    CHECK(post(&b, path, "{}") == 200);
    CHECK(wait_call(&a, id, "\"state\":\"active\"") && wait_call(&b, id, "\"state\":\"active\""));
    CHECK(wait_connected(&a, id, 1) && wait_connected(&b, id, 1));
    CHECK(wait_call(&c, id, b.uid));                                        /* carol sees who is in it */
    send_frames(&a, id, 30, 1);
    send_frames(&b, id, 30, 51);
    wait_heard(&hb, a.uid, 25);
    wait_heard(&ha, b.uid, 25);

    /* ---- carol joins too, by starting one in the group: it is the same call; everyone connects with everyone ---- */
    snprintf(body, sizeof body, "{\"group\":\"%s\"}", gid);
    CHECK(post(&c, "/api/v1/calls/start", body) == 201 && json_has(id));
    CHECK(wait_connected(&a, id, 2) && wait_connected(&b, id, 2) && wait_connected(&c, id, 2));
    send_frames(&c, id, 30, 101);
    wait_heard(&ha, c.uid, 25);
    wait_heard(&hb, c.uid, 25);                                            /* over bob and carol's group-only connection */
    send_frames(&b, id, 30, 151);
    wait_heard(&hc, b.uid, 25);
    wait_heard(&hc, a.uid, 0);
    send_frames(&a, id, 30, 201);
    wait_heard(&hc, a.uid, 25);
    CHECK(ha.bad == 0 && hb.bad == 0 && hc.bad == 0);

    /* ---- video from each to the other two ---- */
    {
        seen_video va = { PTHREAD_MUTEX_INITIALIZER, 0, 0, "" }, vb = va, vc = va;
        puddle_calls_on_video(puddle_server_calls(a.srv), on_video, &va);
        puddle_calls_on_video(puddle_server_calls(b.srv), on_video, &vb);
        puddle_calls_on_video(puddle_server_calls(c.srv), on_video, &vc);
        send_video(&c, id, 12);
        for (int i = 0; i < 100 && (video_good(&va) < 9 || video_good(&vb) < 9); i++) usleep(20000);
        CHECK(video_good(&va) >= 9 && video_good(&vb) >= 9 && !strcmp(va.from, c.uid) && !strcmp(vb.from, c.uid)); /* bob's over the group-only stream */
        /* a shared screen, to every member, as "<them>/screen" */
        seen_video sa = { PTHREAD_MUTEX_INITIALIZER, 0, 0, "" }, sb = sa;
        puddle_calls_on_video(puddle_server_calls(a.srv), on_video, &sa);
        puddle_calls_on_video(puddle_server_calls(b.srv), on_video, &sb);
        {
            unsigned char au[7000];
            for (int k = 0; k < 12; k++) {
                size_t len = video_au(k, au);
                CHECK(puddle_call_send_screen(puddle_server_calls(c.srv), id, au, len) == 0);
                usleep(66000);
            }
        }
        for (int i = 0; i < 100 && (video_good(&sa) < 9 || video_good(&sb) < 9); i++) usleep(20000);
        {
            char want[PUDDLE_USER_ID_LEN + 16];
            snprintf(want, sizeof want, "%s/screen", c.uid);
            CHECK(video_good(&sa) >= 9 && video_good(&sb) >= 9 && sa.bad == 0 && !strcmp(sa.from, want) && !strcmp(sb.from, want));
        }
        puddle_calls_on_video(puddle_server_calls(a.srv), on_video, &va);
        puddle_calls_on_video(puddle_server_calls(b.srv), on_video, &vb);
        send_video(&b, id, 12);
        for (int i = 0; i < 100 && video_good(&vc) < 9; i++) usleep(20000);
        CHECK(video_good(&vc) >= 9 && !strcmp(vc.from, b.uid));
        CHECK(va.bad == 0 && vb.bad == 0 && vc.bad == 0);
        for (int k = 0; k < 3; k++)
            puddle_calls_on_video(puddle_server_calls(all[k]->srv), NULL, NULL);
    }

    /* ---- busy: one call at a time ---- */
    snprintf(body, sizeof body, "{\"user\":\"%s\"}", a.uid);
    CHECK(post(&d, "/api/v1/calls/start", body) == 201 && json_has("\"reason\":\"busy\""));
    snprintf(body, sizeof body, "{\"user\":\"%s\"}", b.uid);
    CHECK(post(&a, "/api/v1/calls/start", body) == 409 && json_has("busy"));

    /* ---- carol leaves: it goes on without her, and she can come back ---- */
    snprintf(path, sizeof path, "/api/v1/calls/%s/end", id);
    CHECK(post(&c, path, NULL) == 200 && json_has("\"state\":\"open\""));
    CHECK(wait_without(&a, id, c.uid) && wait_without(&b, id, c.uid));
    CHECK(wait_connected(&a, id, 1));
    snprintf(body, sizeof body, "{\"group\":\"%s\"}", gid);
    CHECK(post(&c, "/api/v1/calls/start", body) == 201 && json_has(id));
    CHECK(wait_connected(&a, id, 2) && wait_connected(&c, id, 2));
    CHECK(post(&c, path, NULL) == 200);

    /* ---- bob leaves; alice, alone, is let go after a while ---- */
    CHECK(post(&b, path, NULL) == 200);
    CHECK(wait_call(&a, id, "\"reason\":\"hung_up\""));
    CHECK(puddle_call_send_audio(puddle_server_calls(a.srv), id, (const unsigned char *)"x", 1) == -1);
    snprintf(path, sizeof path, "/api/v1/calls?group=%s", gid);
    CHECK(wait_has(&b, path, "\"reason\":\"hung_up\""));
    for (int i = 0; i < 100 && get(&b, path) == 200 && json_has("\"state\":\"open\""); i++) usleep(20000);
    CHECK(!json_has("\"state\":\"open\""));                                 /* alice said she left: over, at once */
    CHECK(get(&c, path) == 200 && json_has(id) && json_has("\"reason\":\"hung_up\""));

    /* ---- nobody comes: bob says no, carol lets it ring ---- */
    snprintf(body, sizeof body, "{\"group\":\"%s\"}", gid);
    char id2[32];
    CHECK(post(&a, "/api/v1/calls/start", body) == 201);
    call_id(id2);
    CHECK(wait_call(&b, id2, "\"state\":\"ringing\"") && wait_call(&c, id2, "\"state\":\"ringing\""));
    snprintf(path, sizeof path, "/api/v1/calls/%s/decline", id2);
    CHECK(post(&b, path, NULL) == 200 && json_has("\"state\":\"open\""));
    CHECK(wait_call(&a, id2, "\"reason\":\"no_answer\""));
    CHECK(wait_call(&b, id2, "\"reason\":\"declined\""));
    CHECK(wait_call(&c, id2, "\"reason\":\"missed\""));

    /* ---- alice and bob start one at the same moment: they end up in one call ---- */
    starter sa = { &a, "", 0, "" }, sb = { &b, "", 0, "" };
    snprintf(sa.body, sizeof sa.body, "{\"group\":\"%s\"}", gid);
    snprintf(sb.body, sizeof sb.body, "{\"group\":\"%s\"}", gid);
    pthread_t ta, tb;
    pthread_create(&ta, NULL, start_thread, &sa);
    pthread_create(&tb, NULL, start_thread, &sb);
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);
    CHECK(sa.status == 201 && sb.status == 201);
    char ca[32] = "", cb[32] = "", sta[16] = "", stb[16] = "";
    for (int i = 0; i < 300; i++) {
        current_of(&a, gid, ca, sta);
        current_of(&b, gid, cb, stb);
        if (ca[0] && !strcmp(ca, cb) && !strcmp(sta, "active") && !strcmp(stb, "active")) break;
        usleep(50000);
    }
    CHECK(ca[0] && !strcmp(ca, cb) && !strcmp(sta, "active") && !strcmp(stb, "active"));
    snprintf(path, sizeof path, "/api/v1/calls/%s/end", ca);
    CHECK(post(&a, path, NULL) == 200);
    CHECK(post(&b, path, NULL) == 200);

    for (int k = 0; k < 4; k++)
        node_down(all[k]);
    nftw(root, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    if (failures) {
        fprintf(stderr, "groupcall_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("groupcall_test: ok\n");
    return 0;
}
