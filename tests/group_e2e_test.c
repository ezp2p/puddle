/* Group chats between three real Puddles over real TLS on loopback, where two of the members are strangers to each other:
 * the invitation, the group-only connection that joins them, messages both ways, edits, deletes, a rename, a friend request over
 * a group connection, and leaving. Each Puddle is the real server with its own data directory; the owner calls are real HTTPS calls. */
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

/* the id after "id":" in the last answer */
static void id_of(char out[65], const char *after)
{
    const char *at = strstr(resp, after);
    const char *id = at ? strstr(at, "\"id\":\"") : NULL;
    out[0] = 0;
    if (id) { memcpy(out, id + 6, 64); out[64] = 0; }
}

static void befriend(node *from, node *to)
{
    char body[600];
    snprintf(body, sizeof body, "{\"target\":\"%s\",\"message\":\"hi\",\"profile\":\"main\"}", to->link); /* who they will see us as */
    CHECK(post(from, "/api/v1/streams/request", body) == 201);
    has_arg a = { to, "/api/v1/streams?state=pending_in", from->uid };
    CHECK(wait_for(has_cond, &a));
    snprintf(body, sizeof body, "/api/v1/streams/%s/accept", from->uid);
    CHECK(post(to, body, "{\"profile\":\"main\"}") == 200);
    CHECK(wait_state(from, to->uid, PUDDLE_STREAM_ACTIVE));
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    const char *base = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/puddle-groupe2e-XXXXXX", base ? base : "/tmp");
    if (!mkdtemp(root)) return 2;
    node a, b, c;
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b); memset(&c, 0, sizeof c);
    if (node_make(&a, "alice", "Alice") != 0 || node_make(&b, "bob", "Bob") != 0 || node_make(&c, "carol", "Carol") != 0) return 2;
    char body[1500], gid[65], url[300], mid[65];

    /* alice is friends with both; bob and carol have never met */
    befriend(&a, &b);
    befriend(&a, &c);
    {
        puddle_stream_state s;
        CHECK(!stream_state_of(&b, c.uid, &s) && !stream_state_of(&c, b.uid, &s));
    }

    /* ---- alice starts a group and asks them both ---- */
    snprintf(body, sizeof body, "{\"name\":\"Trio\",\"members\":[\"%s\",\"%s\"],\"profile\":\"main\"}", b.uid, c.uid);
    CHECK(post(&a, "/api/v1/groups/create", body) == 201 && json_has("\"name\":\"Trio\"") && json_has("\"not_invited\":[]"));
    id_of(gid, "{\"id\":\"");
    {
        const char *at = strstr(resp, "\"id\":\"");
        memcpy(gid, at + 6, 52);
        gid[52] = 0;
    }
    CHECK(puddle_id_valid(gid));
    CHECK(wait_has(&b, "/api/v1/groups", "\"my\":\"invited\"") && wait_has(&c, "/api/v1/groups", "\"my\":\"invited\""));
    CHECK(json_has("\"invited_by\":{\"id\":\""));

    /* ---- bob says yes: alice hears; carol is not a member yet, so nothing else happens ---- */
    char gpath[200], cpath[200];
    snprintf(gpath, sizeof gpath, "/api/v1/groups/%s", gid);
    snprintf(cpath, sizeof cpath, "/api/v1/conversations/%s/messages/send", gid);
#define G(verb) (snprintf(url, sizeof url, "%s/%s", gpath, verb), url)
    CHECK(post(&b, G("accept"), "{\"profile\":\"main\"}") == 200 && json_has("\"my\":\"in\""));
    {
        char path[300];
        snprintf(path, sizeof path, "/api/v1/groups");
        has_arg ha = { &a, path, "" };
        (void)ha;
        for (int i = 0; i < 200; i++) {
            puddle_group g;
            if (puddle_groups_get(puddle_server_groups(a.srv), gid, &g) == 0) {
                puddle_gmember *m = puddle_group_member(&g, b.uid);
                int ok = m && m->state == PUDDLE_GM_MEMBER;
                puddle_group_free(&g);
                if (ok) break;
            }
            usleep(50000);
        }
        puddle_group g;
        CHECK(puddle_groups_get(puddle_server_groups(a.srv), gid, &g) == 0 && puddle_group_member(&g, b.uid)->state == PUDDLE_GM_MEMBER);
        puddle_group_free(&g);
    }
    {
        puddle_stream_state s;
        CHECK(!stream_state_of(&b, c.uid, &s));                                                 /* carol has not said yes: no connection */
    }

    /* ---- carol says yes: she joins, and reaches bob, whom she has never met, through a group-only connection ---- */
    CHECK(post(&c, G("accept"), "{\"profile\":\"main\"}") == 200);
    CHECK(wait_state(&c, b.uid, PUDDLE_STREAM_SCOPED) && wait_state(&b, c.uid, PUDDLE_STREAM_SCOPED));
    {
        /* until each of the three has the other two as members: carol hears bob's yes from bob, and bob hers from her */
        for (int i = 0; i < 200; i++) {
            int all = 1;
            node *who[3] = { &a, &b, &c };
            for (int k = 0; k < 3; k++) {
                puddle_group g;
                if (puddle_groups_get(puddle_server_groups(who[k]->srv), gid, &g) != 0) { all = 0; continue; }
                all = all && puddle_group_count(&g, PUDDLE_GM_MEMBER) == 3;
                puddle_group_free(&g);
            }
            if (all) break;
            usleep(50000);
        }
        puddle_group g;
        CHECK(puddle_groups_get(puddle_server_groups(b.srv), gid, &g) == 0 && puddle_group_count(&g, PUDDLE_GM_MEMBER) == 3);
        puddle_group_free(&g);
        CHECK(puddle_groups_get(puddle_server_groups(c.srv), gid, &g) == 0 && puddle_group_count(&g, PUDDLE_GM_MEMBER) == 3 && g.my == PUDDLE_GROUP_IN);
        puddle_group_free(&g);
    }
    /* what they know of each other is the profile each shows the group, and they are not friends: directly, they are
     * shown by their ids only, since neither chose a profile for the other */
    CHECK(get(&b, "/api/v1/streams") == 200 && json_has("\"state\":\"scoped\"") && !json_has("\"display_name\":\"Carol\""));
    CHECK(get(&c, "/api/v1/streams") == 200 && json_has("\"state\":\"scoped\"") && !json_has("\"display_name\":\"Bob\""));
    CHECK(wait_has(&b, gpath, "\"display_name\":\"Carol\""));
    CHECK(wait_has(&c, gpath, "\"display_name\":\"Bob\""));
    snprintf(url, sizeof url, "/api/v1/conversations/%s/messages/send", c.uid);
    CHECK(post(&b, url, "{\"body\":\"a private word\"}") == 403);                                            /* a group connection is not a friendship */

    /* ---- talking ---- */
    CHECK(post(&b, cpath, "{\"body\":\"hello trio\"}") == 201);
    id_of(mid, "{\"id\":\"");
    snprintf(url, sizeof url, "/api/v1/conversations/%s/messages", gid);
    CHECK(wait_has(&a, url, "hello trio") && wait_has(&c, url, "hello trio"));
    CHECK(json_has("\"outgoing\":false"));
    CHECK(post(&c, cpath, "{\"body\":\"hi from carol\"}") == 201);
    CHECK(wait_has(&a, url, "hi from carol") && wait_has(&b, url, "hi from carol"));             /* and carol to bob, whom she reaches directly */
    CHECK(post(&a, cpath, "{\"body\":\"alice here\"}") == 201);
    CHECK(wait_has(&b, url, "alice here") && wait_has(&c, url, "alice here"));
    CHECK(get(&c, url) == 200 && strstr(resp, "hello trio") < strstr(resp, "alice here") && json_has("\"type\":\"group_join\""));
    CHECK(wait_has(&a, "/api/v1/conversations", "\"kind\":\"group\""));
    CHECK(json_has("\"name\":\"Trio\"") && json_has("\"member_count\":3") && json_has("\"unread\":2"));

    /* ---- an edit, a delete and a new name travel too ---- */
    snprintf(body, sizeof body, "/api/v1/conversations/%s/messages/%s/edit", gid, mid);
    CHECK(post(&b, body, "{\"body\":\"hello again, trio\"}") == 200);
    CHECK(wait_has(&a, url, "hello again, trio") && wait_has(&c, url, "hello again, trio"));
    snprintf(body, sizeof body, "%s/rename", gpath);
    CHECK(post(&c, body, "{\"name\":\"The Trio\"}") == 200);
    CHECK(wait_has(&a, "/api/v1/groups", "\"name\":\"The Trio\"") && wait_has(&b, "/api/v1/groups", "\"name\":\"The Trio\""));

    /* ---- bob asks carol to be friends over the group connection: it waits for her, and the group goes on ---- */
    {
        char req[600];
        snprintf(req, sizeof req, "{\"target\":\"%s\",\"message\":\"we should be friends\"}", c.link);
        CHECK(post(&b, "/api/v1/streams/request", req) == 201);                                  /* not "already a stream": it is only a group one */
        CHECK(wait_has(&c, "/api/v1/streams?state=pending_in", "we should be friends"));
        CHECK(post(&b, cpath, "{\"body\":\"still talking\"}") == 201);
        CHECK(wait_has(&c, url, "still talking"));                                              /* the group is not held up by the request */
        snprintf(req, sizeof req, "/api/v1/streams/%s/accept", b.uid);
        CHECK(post(&c, req, NULL) == 200);
        CHECK(wait_state(&b, c.uid, PUDDLE_STREAM_ACTIVE) && wait_state(&c, b.uid, PUDDLE_STREAM_ACTIVE));
        snprintf(req, sizeof req, "/api/v1/conversations/%s/messages/send", c.uid);
        CHECK(post(&b, req, "{\"body\":\"now it is private\"}") == 201);                                         /* and now a DM works */
        char dm[300];
        snprintf(dm, sizeof dm, "/api/v1/conversations/%s/messages", b.uid);
        CHECK(wait_has(&c, dm, "now it is private"));
        CHECK(get(&b, "/api/v1/streams?state=pending_out") == 200 && !json_has("provisional"));  /* the made-up id is gone */
    }

    /* ---- carol leaves: the others see it; friends stay friends ---- */
    snprintf(body, sizeof body, "%s/leave", gpath);
    CHECK(post(&c, body, NULL) == 200);
    for (int i = 0; i < 200; i++) {
        puddle_group g;
        int left = 0;
        if (puddle_groups_get(puddle_server_groups(a.srv), gid, &g) == 0) {
            puddle_gmember *m = puddle_group_member(&g, c.uid);
            left = m && m->state == PUDDLE_GM_LEFT;
            puddle_group_free(&g);
        }
        if (left) break;
        usleep(50000);
    }
    CHECK(get(&a, url) == 200 && json_has("\"type\":\"group_leave\""));
    CHECK(post(&a, cpath, "{\"body\":\"carol is gone\"}") == 201);
    CHECK(wait_has(&b, url, "carol is gone"));
    usleep(700000);
    CHECK(get(&c, "/api/v1/groups") == 200 && !json_has(gid));                                      /* she no longer has it */
    {
        puddle_stream_state s;
        CHECK(stream_state_of(&b, c.uid, &s) && s == PUDDLE_STREAM_ACTIVE);                     /* bob and carol are friends now, and stay so */
    }

    node_down(&a); node_down(&b); node_down(&c);
    nftw(root, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    return failures ? 1 : 0;
}
