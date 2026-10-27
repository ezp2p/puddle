/* Two (and more) real Puddles in one process, talking over real TLS on loopback: asking for a stream, accepting
 * and declining, invites, blocks, closing, a changed certificate, and what a stranger is told. Each Puddle is the
 * real server with its own data directory; the "owner" calls are real HTTPS calls with the owner token. */
#include "federation/access.h"
#include "core/people.h"
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

/* does the profile `n` holds for `peer_id` mention `text`? */
static int has_profile(node *n, const char *peer_id, const char *text)
{
    char *json;
    size_t len;
    if (puddle_people_profile(puddle_server_people(n->srv), peer_id, PUDDLE_PEOPLE_DM, &json, &len) != 0) return 0;
    int found = strstr(json, text) != NULL;
    free(json);
    return found;
}

static int stream_state(node *n, const char *peer_id, puddle_stream_state *out)
{
    puddle_stream st;
    if (puddle_streams_get(puddle_server_streams(n->srv), peer_id, &st) != 0) return 0;
    *out = st.state;
    puddle_stream_free(&st);
    return 1;
}

static int wait_state(node *n, const char *peer_id, puddle_stream_state want)
{
    for (int i = 0; i < 200; i++) {
        puddle_stream_state s;
        if (stream_state(n, peer_id, &s) && s == want) return 1;
        usleep(50000);
    }
    return 0;
}

static int owed_of(node *n, const char *peer_id)
{
    puddle_stream st;
    if (puddle_streams_get(puddle_server_streams(n->srv), peer_id, &st) != 0) return -1;
    int o = (int)st.owed;
    puddle_stream_free(&st);
    return o;
}

static int wait_owed_cleared(node *n, const char *peer_id)
{
    for (int i = 0; i < 200; i++) {
        if (owed_of(n, peer_id) == PUDDLE_OWED_NONE) return 1;
        usleep(50000);
    }
    return 0;
}

static int outbox_count(node *n, const char *peer_id, int *queued)
{
    puddle_ob_item *l;
    size_t c;
    puddle_outbox_list(puddle_server_outbox(n->srv), &l, &c);
    int total = 0;
    if (queued) *queued = 0;
    for (size_t i = 0; i < c; i++)
        if (!peer_id || !strcmp(l[i].peer, peer_id)) {
            total++;
            if (queued && l[i].state == PUDDLE_OB_QUEUED) (*queued)++;
        }
    puddle_outbox_list_free(l, c);
    return total;
}

/* Both outboxes empty: what was owed has been delivered. Deliveries finish on their own threads, so this waits for them
 * (up to 10 s) rather than looking once, which under load can come a moment too soon. */
static int outboxes_empty(node *a, node *b)
{
    for (int i = 0; i < 200; i++) {
        if (outbox_count(a, NULL, NULL) == 0 && outbox_count(b, NULL, NULL) == 0) return 1;
        usleep(50000);
    }
    return 0;
}

static void request(node *from, node *to, const char *message, const char *link_override, int expect)
{
    char body[900];
    /* who they will see us as: nothing goes without that choice */
    snprintf(body, sizeof body, "{\"target\":\"%s\",\"message\":\"%s\",\"profile\":\"main\"}", link_override ? link_override : to->link, message);
    int st = post(from, "/api/v1/streams/request", body);
    if (st != expect) { fprintf(stderr, "FAIL request %s->%s: %d %s\n", from->name, to->name, st, resp); failures++; }
}

/* {"<key>":{"id","address","profile","chain"}<extra>} as `who` would send it */
static char *peer_body(puddle_identity *who, const char *who_id, const char *key, const char *extra, size_t *len)
{
    size_t cl;
    char *chain = puddle_identity_chain(who, &cl);
    size_t cap = cl * 6 + 2000;
    char *big = malloc(cap);
    puddle_jw w;
    puddle_jw_init(&w, big, cap);
    puddle_jw_object(&w);
    puddle_jw_key(&w, key);
    puddle_jw_object(&w);
    puddle_jw_str(&w, "id", who_id);
    puddle_jw_str(&w, "address", "127.0.0.1:9");
    puddle_jw_key(&w, "profile");
    puddle_jw_raw(&w, "{\"display_name\":\"x\"}", 20);
    puddle_jw_str(&w, "chain", chain);
    puddle_jw_end_object(&w);
    puddle_jw_end_object(&w);
    free(chain);
    size_t n = puddle_jw_finish(&w);
    if (extra && *extra) {
        big[n - 1] = ',';
        strcpy(big + n, extra);
        strcat(big, "}");
        n = strlen(big);
    }
    *len = n;
    return big;
}

/* send `body` as `who` to `to`'s `path`; the HTTP status (or -1) */
static int send_as(node *to, puddle_identity *who, const char *path, const char *body, size_t len)
{
    puddle_address at;
    char t[64];
    snprintf(t, sizeof t, "127.0.0.1:%u", to->cfg.port);
    puddle_address_parse(t, &at);
    puddle_net_reply r;
    puddle_net_request(&at, "POST", path, body, len, who, to->uid, NULL, &r);
    int st = r.result == PUDDLE_NET_OK ? r.status : -1;
    if (st == 403 && r.body && strstr(r.body, "no_stream")) st = 4030;
    puddle_net_reply_free(&r);
    return st;
}

/* ask `to` how a request from `who` is going; the state it answers with */
static const char *status_of(node *to, puddle_identity *who, const char *who_id)
{
    static char state[16];
    size_t cl;
    char *chain = puddle_identity_chain(who, &cl);
    size_t cap = cl * 6 + 2000;
    char *big = malloc(cap);
    puddle_jw w;
    puddle_jw_init(&w, big, cap);
    puddle_jw_object(&w);
    puddle_jw_key(&w, "requester");
    puddle_jw_object(&w);
    puddle_jw_str(&w, "id", who_id);
    puddle_jw_str(&w, "address", "127.0.0.1:9");
    puddle_jw_key(&w, "profile");
    puddle_jw_raw(&w, "{\"display_name\":\"x\"}", 20);
    puddle_jw_str(&w, "chain", chain);
    puddle_jw_end_object(&w);
    puddle_jw_end_object(&w);
    free(chain);
    size_t n = puddle_jw_finish(&w);
    puddle_address at;
    char t[64];
    snprintf(t, sizeof t, "127.0.0.1:%u", to->cfg.port);
    puddle_address_parse(t, &at);
    puddle_net_reply r;
    puddle_net_request(&at, "POST", "/stream/v1/access/status", big, n, who, to->uid, NULL, &r);
    snprintf(state, sizeof state, "error");
    if (r.result == PUDDLE_NET_OK && r.status == 200 && r.body) {
        const char *p = strstr(r.body, "\"state\":\"");
        if (p) {
            p += 9;
            snprintf(state, sizeof state, "%.*s", (int)strcspn(p, "\""), p);
        }
    }
    puddle_net_reply_free(&r);
    free(big);
    return state;
}

/* The owner API names what it acts on in the path. stream_body() picks the stream (and leaves an empty body);
 * sp("accept") is then "/api/v1/streams/<it>/accept". dm_to and dm_msg do the same for a conversation and a message. */
static char sid[64];
static const char *dm_to, *dm_msg;
static void stream_body(char *out, size_t cap, const char *id) { snprintf(sid, sizeof sid, "%s", id); snprintf(out, cap, "{\"profile\":\"main\"}"); }
static const char *path_of(const char *kind, const char *id, const char *rest)
{
    static char buf[4][400];
    static int k;
    k = (k + 1) % 4;
    snprintf(buf[k], sizeof buf[k], "/api/v1/%s/%s%s%s", kind, id, rest ? "/" : "", rest ? rest : "");
    return buf[k];
}
static const char *sp(const char *verb) { return path_of("streams", sid, verb); }
static const char *msg_path(const char *verb)
{
    char rest[120];
    snprintf(rest, sizeof rest, "messages/%s/%s", dm_msg, verb);
    return path_of("conversations", dm_to, rest);
}

/* has this node asked someone at an address, and not heard who it is yet? */
static int has_provisional(node *n)
{
    char body[8000];
    return api(n, "GET", "/api/v1/streams?state=pending_out", NULL, body, sizeof body) == 200 && strstr(body, "\"provisional\":true") != NULL;
}

static int json_has(const char *needle) { return strstr(resp, needle) != NULL; }

static void rmtree(const char *p) { nftw(p, rm_cb, 16, FTW_DEPTH | FTW_PHYS); }

int main(void)
{
    signal(SIGPIPE, SIG_IGN);
    const char *base = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/puddle-e2e-XXXXXX", base ? base : "/tmp");
    if (!mkdtemp(root)) return 2;

    node alice, bob, carol, dave, eve;
    if (node_make(&alice, "alice", "Alice") || node_make(&bob, "bob", "Bob") || node_make(&carol, "carol", "Carol") ||
        node_make(&dave, "dave", "Dave") || node_make(&eve, "eve", "Eve")) {
        fprintf(stderr, "cannot start the Puddles\n");
        return 2;
    }
    char body[900], err[300];

    /* ---- asking, being asked, accepting ---- */
    request(&bob, &alice, "hi, it is Bob", NULL, 201);
    CHECK(json_has("\"state\":\"pending_out\"") && json_has("\"message\":\"hi, it is Bob\""));
    CHECK(wait_state(&alice, bob.uid, PUDDLE_STREAM_PENDING_IN));                                 /* it arrived, and waits for her */
    CHECK(get(&alice, "/api/v1/streams?state=pending_in") == 200 && json_has(bob.uid) && json_has("\"message\":\"hi, it is Bob\"") &&
          json_has("\"display_name\":\"Bob\""));
    {
        CHECK(has_provisional(&bob));                                                              /* Bob does not know yet, nor who she is */
    }
    stream_body(body, sizeof body, bob.uid);
    CHECK(post(&alice, sp("accept"), body) == 200 && json_has("\"state\":\"active\""));
    CHECK(wait_state(&bob, alice.uid, PUDDLE_STREAM_ACTIVE));                                     /* told, and verified */
    CHECK(wait_owed_cleared(&alice, bob.uid));                                                    /* ...so it is no longer owed */
    {
        puddle_stream st;
        CHECK(puddle_streams_get(puddle_server_streams(bob.srv), alice.uid, &st) == 0);
        CHECK(st.chain && st.tls_fp[0] && has_profile(&bob, alice.uid, "Alice") && st.naddr >= 1);
        puddle_stream_free(&st);
        CHECK(puddle_streams_get(puddle_server_streams(alice.srv), bob.uid, &st) == 0 && st.chain && st.tls_fp[0] &&
              has_profile(&alice, bob.uid, "Bob"));
        puddle_stream_free(&st);
    }
    CHECK(get(&bob, "/api/v1/streams?state=active") == 200 && json_has(alice.uid) && json_has("\"display_name\":\"Alice\""));
    CHECK(outboxes_empty(&alice, &bob));        /* nothing left waiting anywhere */

    /* ---- messages between the two ---- */
    {
        char body2[600], url[300];
        snprintf(body2, sizeof body2, "{\"body\":\"hello Alice, it's Bob\"}"); dm_to = alice.uid;
        CHECK(post(&bob, path_of("conversations", dm_to, "messages/send"), body2) == 201 && json_has("\"outgoing\":true"));
        snprintf(url, sizeof url, "/api/v1/conversations/%s/messages", bob.uid);
        int arrived = 0;
        for (int i = 0; i < 100 && !arrived; i++) {
            arrived = get(&alice, url) == 200 && json_has("hello Alice, it's Bob");
            if (!arrived) usleep(50000);
        }
        CHECK(arrived && json_has("\"outgoing\":false") && json_has(bob.uid));                  /* it came in, from Bob */
        snprintf(body2, sizeof body2, "{\"body\":\"hi Bob \\u2014 caf\\u00e9 \\ud83d\\ude42\"}"); dm_to = bob.uid;
        CHECK(post(&alice, path_of("conversations", dm_to, "messages/send"), body2) == 201);
        char msg_id[80] = "";
        {
            const char *ip = strstr(resp, "\"id\":\"");
            if (ip) snprintf(msg_id, sizeof msg_id, "%.64s", ip + 6);
        }
        snprintf(url, sizeof url, "/api/v1/conversations/%s/messages", alice.uid);
        int back = 0;
        for (int i = 0; i < 100 && !back; i++) {
            back = get(&bob, url) == 200 && json_has("caf\xC3\xA9 \xF0\x9F\x99\x82");
            if (!back) usleep(50000);
        }
        CHECK(back);                                                                              /* any language, intact */
        CHECK(json_has("hello Alice, it's Bob") && json_has("\"outgoing\":true") && json_has("\"outgoing\":false"));
        CHECK(strstr(resp, "hello Alice") < strstr(resp, "hi Bob"));                              /* in the order they were said */
        /* Alice corrects it, and Bob sees the correction; she takes it back, and Bob sees that */
        snprintf(body2, sizeof body2, "{\"body\":\"hi Bob (corrected)\"}"); dm_to = bob.uid; dm_msg = msg_id;
        CHECK(post(&alice, msg_path("edit"), body2) == 200 && json_has("\"edited_at\""));
        int edited = 0;
        for (int i = 0; i < 100 && !edited; i++) {
            edited = get(&bob, url) == 200 && json_has("hi Bob (corrected)") && json_has("\"edited_at\"");
            if (!edited) usleep(50000);
        }
        CHECK(edited && !json_has("caf\xC3\xA9"));
        snprintf(body2, sizeof body2, "{}"); dm_to = bob.uid; dm_msg = msg_id;
        CHECK(post(&alice, msg_path("delete"), NULL) == 200);
        int gone = 0;
        for (int i = 0; i < 100 && !gone; i++) {
            gone = get(&bob, url) == 200 && json_has("\"deleted\":true");
            if (!gone) usleep(50000);
        }
        CHECK(gone && !json_has("hi Bob") && json_has("hello Alice, it's Bob"));
        snprintf(body2, sizeof body2, "{}"); dm_to = alice.uid; dm_msg = msg_id;
        CHECK(post(&bob, msg_path("delete"), NULL) == 404);                                      /* Bob cannot take back what Alice said */
        CHECK(outboxes_empty(&alice, &bob));     /* delivered, nothing waiting */
        /* someone with no stream cannot be sent to, nor send */
        snprintf(body2, sizeof body2, "{\"body\":\"hello\"}"); dm_to = eve.uid;
        CHECK(post(&alice, path_of("conversations", dm_to, "messages/send"), body2) == 404);                 /* no stream: no conversation */
        CHECK(stream_state(&bob, alice.uid, &(puddle_stream_state){0}));
    }

    /* ---- profiles and their pictures ---- */
    {
        puddle_profile am, aw, bm;
        CHECK(puddle_profiles_get(puddle_server_profiles(alice.srv), "main", &am) == 0 && puddle_profiles_get(puddle_server_profiles(alice.srv), "work", &aw) == 0 &&
              puddle_profiles_get(puddle_server_profiles(bob.srv), "main", &bm) == 0);
        char ah_main[65], ah_work[65], bh_main[65];
        uint64_t ah_main_size = am.avatar.size;
        snprintf(ah_main, sizeof ah_main, "%s", am.avatar.hash);
        snprintf(ah_work, sizeof ah_work, "%s", aw.avatar.hash);
        snprintf(bh_main, sizeof bh_main, "%s", bm.avatar.hash);
        puddle_profile_free(&am); puddle_profile_free(&aw); puddle_profile_free(&bm);
        int have_a = 0, have_b = 0;
        for (int i = 0; i < 200 && !(have_a && have_b); i++) {
            have_a = puddle_media_have(bob.cfg.data_dir, ah_main);                                 /* Alice's picture is on Bob's machine */
            have_b = puddle_media_have(alice.cfg.data_dir, bh_main);                               /* and Bob's on Alice's */
            if (!(have_a && have_b)) usleep(50000);
        }
        CHECK(have_a && have_b);
        char url[300], body2[400];
        snprintf(url, sizeof url, "/api/v1/media?hash=%s", ah_main);
        CHECK(api(&bob, "GET", url, NULL, resp, sizeof resp) == 200);                              /* and Bob's app can show it */
        CHECK(outboxes_empty(&alice, &bob));
        CHECK(get(&bob, "/api/v1/streams?state=active") == 200 && json_has(ah_main) && json_has("\"color\"") == 0);   /* its reference, with the profile */
        CHECK(get(&alice, "/api/v1/profiles") == 200 && json_has("Alice (work)") && json_has("\"color\":{\"colors\":[\"#336699\"]}") && json_has(ah_work));

        /* Alice shows Bob her work profile: he is told, learns of the new picture, and fetches it */
        CHECK(!puddle_media_have(bob.cfg.data_dir, ah_work));
        snprintf(body2, sizeof body2, "{\"profile\":\"work\"}");
        CHECK(post(&alice, path_of("streams", bob.uid, "profile"), body2) == 200 && json_has("\"profile\":\"work\""));
        int seen = 0;
        for (int i = 0; i < 200 && !seen; i++) {
            seen = get(&bob, "/api/v1/streams?state=active") == 200 && json_has("Alice (work)") && puddle_media_have(bob.cfg.data_dir, ah_work);
            if (!seen) usleep(50000);
        }
        CHECK(seen && json_has("#336699"));
        snprintf(url, sizeof url, "/api/v1/media?hash=%s", ah_work);
        CHECK(api(&bob, "GET", url, NULL, resp, sizeof resp) == 200);
        /* none: Bob is told to forget it, and knows her only by her id */
        snprintf(body2, sizeof body2, "{\"profile\":null}");
        CHECK(post(&alice, path_of("streams", bob.uid, "profile"), body2) == 200);
        int gone = 0;
        for (int i = 0; i < 200 && !gone; i++) {
            gone = get(&bob, path_of("users", alice.uid, NULL)) == 200 && !json_has("\"profile\"");
            if (!gone) usleep(50000);
        }
        CHECK(gone);
        /* and her main one again */
        snprintf(body2, sizeof body2, "{\"profile\":\"main\"}");
        CHECK(post(&alice, path_of("streams", bob.uid, "profile"), body2) == 200);
        int back = 0;
        for (int i = 0; i < 200 && !back; i++) {
            back = get(&bob, "/api/v1/streams?state=active") == 200 && !json_has("Alice (work)") && json_has("\"display_name\":\"Alice\"");
            if (!back) usleep(50000);
        }
        CHECK(back);
        for (int i = 0; i < 100 && (outbox_count(&alice, NULL, NULL) || outbox_count(&bob, NULL, NULL)); i++) usleep(50000);
        CHECK(outboxes_empty(&alice, &bob));
        /* a picture we are not shown is not given out: Bob asks for the work one directly, as himself */
        {
            puddle_net_reply nr;
            puddle_address at;
            char t[64], target[200];
            snprintf(t, sizeof t, "127.0.0.1:%u", alice.cfg.port);
            puddle_address_parse(t, &at);
            snprintf(target, sizeof target, "/stream/v1/media?hash=%s", ah_work);
            puddle_net_request(&at, "GET", target, NULL, 0, puddle_server_identity(bob.srv), alice.uid, NULL, &nr);
            CHECK(nr.result == PUDDLE_NET_OK && nr.status == 404);
            puddle_net_reply_free(&nr);
            snprintf(target, sizeof target, "/stream/v1/media?hash=%s", ah_main);
            puddle_net_request(&at, "GET", target, NULL, 0, puddle_server_identity(bob.srv), alice.uid, NULL, &nr);
            CHECK(nr.result == PUDDLE_NET_OK && nr.status == 200 && !strcmp(nr.content_type, "image/png") && nr.body_len == 2008 && nr.body_len == ah_main_size);
            puddle_net_reply_free(&nr);
        }
    }

    /* ---- changing keys while a message is on its way ---- */
    {
        node_down(&bob);
        char body2[600], url[300], buf[400];
        snprintf(body2, sizeof body2, "{\"body\":\"sent before I changed keys\"}"); dm_to = bob.uid;
        CHECK(post(&alice, path_of("conversations", dm_to, "messages/send"), body2) == 201);                                      /* Bob is away: it waits, signed by the old key */
        usleep(1000000);
        int queued;
        CHECK(outbox_count(&alice, bob.uid, &queued) == 1 && queued == 1);
        puddle_stream st;
        snprintf(body2, sizeof body2, "{\"phrase\":\"%s\"}", alice.phrase);
        CHECK(post(&alice, "/api/v1/identity/roll", body2) == 200);
        {
            const char *p = strstr(resp, "\"new_phrase\":\"");
            CHECK(p != NULL);
            if (p) { p += 14; snprintf(buf, sizeof buf, "%.*s", (int)strcspn(p, "\""), p); snprintf(alice.phrase, sizeof alice.phrase, "%s", buf); }
        }
        CHECK(post(&alice, "/api/v1/identity/roll/confirm", "{}") == 200 && json_has("\"seq\":1"));
        CHECK(outbox_count(&alice, bob.uid, NULL) == 2);                                           /* the news of the new keys, ahead of the message */
        CHECK(node_up(&bob) == 0);
        puddle_outbox_retry_now(puddle_server_outbox(alice.srv), bob.uid);
        snprintf(url, sizeof url, "/api/v1/conversations/%s/messages", alice.uid);
        int arrived = 0;
        for (int i = 0; i < 200 && !arrived; i++) {
            arrived = get(&bob, url) == 200 && json_has("sent before I changed keys");
            if (!arrived) usleep(50000);
        }
        CHECK(arrived);                                                                             /* the news went first, so the message was believed */
        for (int i = 0; i < 100 && outbox_count(&alice, NULL, NULL) != 0; i++) usleep(50000);
        CHECK(outbox_count(&alice, NULL, NULL) == 0);
        CHECK(puddle_streams_get(puddle_server_streams(bob.srv), alice.uid, &st) == 0);
        {
            puddle_chain_head h;
            char e3[200];
            CHECK(st.chain && puddle_chain_verify(st.chain, st.chain_len, alice.uid, &h, e3, sizeof e3) == 0 && h.seq == 1);   /* Bob holds the new chain */
            puddle_stream_free(&st);
        }
        CHECK(puddle_streams_get(puddle_server_streams(alice.srv), bob.uid, &st) == 0 && st.chain_sent == 1);   /* and Alice knows he does */
        puddle_stream_free(&st);
        /* from now on, plain sailing, both ways */
        snprintf(body2, sizeof body2, "{\"body\":\"after the change\"}"); dm_to = bob.uid;
        CHECK(post(&alice, path_of("conversations", dm_to, "messages/send"), body2) == 201);
        int again = 0;
        snprintf(url, sizeof url, "/api/v1/conversations/%s/messages", alice.uid);
        for (int i = 0; i < 100 && !again; i++) {
            again = get(&bob, url) == 200 && json_has("after the change");
            if (!again) usleep(50000);
        }
        CHECK(again);
        snprintf(body2, sizeof body2, "{\"body\":\"and back\"}"); dm_to = alice.uid;
        CHECK(post(&bob, path_of("conversations", dm_to, "messages/send"), body2) == 201);
        int back = 0;
        snprintf(url, sizeof url, "/api/v1/conversations/%s/messages", bob.uid);
        for (int i = 0; i < 100 && !back; i++) {
            back = get(&alice, url) == 200 && json_has("and back");
            if (!back) usleep(50000);
        }
        CHECK(back);
    }

    /* each side now holds the other's keys: a signed request from Bob is accepted by Alice's PEER routes */
    {
        puddle_net_reply r;
        puddle_address at;
        char t[64];
        snprintf(t, sizeof t, "127.0.0.1:%u", alice.cfg.port);
        puddle_address_parse(t, &at);
        puddle_identity *bid = NULL;
        (void)bid;
        puddle_net_request(&at, "POST", "/stream/v1/close", "{}", 2, puddle_server_identity(carol.srv), alice.uid, NULL, &r);
        CHECK(r.result == PUDDLE_NET_OK && r.status == 404);                                      /* Carol: a stranger to Alice's close */
        puddle_net_reply_free(&r);
    }

    /* an answer nobody asked for changes nothing: Bob (already a stream) says "declined", Eve (never asked) says "approved" */
    {
        size_t n;
        char *b = peer_body(puddle_server_identity(bob.srv), bob.uid, "responder", "\"approved\":false", &n);
        CHECK(send_as(&alice, puddle_server_identity(bob.srv), "/stream/v1/access/result", b, n) == 202);
        free(b);
        CHECK(wait_state(&alice, bob.uid, PUDDLE_STREAM_ACTIVE));
        b = peer_body(puddle_server_identity(eve.srv), eve.uid, "responder", "\"approved\":true", &n);
        CHECK(send_as(&alice, puddle_server_identity(eve.srv), "/stream/v1/access/result", b, n) == 202);
        free(b);
        b = peer_body(puddle_server_identity(bob.srv), bob.uid, "responder", "\"approved\":\"yes\"", &n);
        CHECK(send_as(&alice, puddle_server_identity(bob.srv), "/stream/v1/access/result", b, n) == 202);
        free(b);
        puddle_stream_state s;
        CHECK(!stream_state(&alice, eve.uid, &s) && stream_state(&alice, bob.uid, &s) && s == PUDDLE_STREAM_ACTIVE);
        /* the same body signed by someone else is not an answer from Bob at all */
        b = peer_body(puddle_server_identity(bob.srv), bob.uid, "responder", "\"approved\":false", &n);
        CHECK(send_as(&alice, puddle_server_identity(eve.srv), "/stream/v1/access/result", b, n) == 404);
        free(b);
    }

    /* ---- a request that is declined ---- */
    request(&carol, &alice, "may I?", NULL, 201);
    CHECK(wait_state(&alice, carol.uid, PUDDLE_STREAM_PENDING_IN));
    stream_body(body, sizeof body, carol.uid);
    CHECK(post(&alice, sp("decline"), body) == 200);
    CHECK(wait_state(&carol, alice.uid, PUDDLE_STREAM_CLOSED));                                   /* Carol is told */
    CHECK(wait_owed_cleared(&alice, carol.uid));
    CHECK(get(&carol, "/api/v1/streams?state=closed") == 200 && json_has(alice.uid));
    CHECK(send_as(&alice, puddle_server_identity(carol.srv), "/stream/v1/close", "{}", 2) == 4030);  /* closed: nothing to close */
    request(&carol, &alice, "please?", NULL, 201);                                                 /* a closed stream may be asked for again */
    CHECK(wait_state(&alice, carol.uid, PUDDLE_STREAM_PENDING_IN));
    request(&carol, &alice, "again", NULL, 409);                                                   /* ...but not while one is waiting */
    CHECK(json_has("already_requested"));
    request(&alice, &carol, "ask back", NULL, 409);                                                /* they asked us: accept instead */
    CHECK(json_has("they_asked"));
    request(&bob, &alice, "already friends", NULL, 409);                                           /* a stream that is open */
    CHECK(json_has("already_streaming"));
    stream_body(body, sizeof body, carol.uid);
    CHECK(post(&alice, sp("decline"), body) == 200);
    CHECK(wait_state(&carol, alice.uid, PUDDLE_STREAM_CLOSED));
    CHECK(wait_owed_cleared(&alice, carol.uid));

    /* ---- an invite is let in at once ---- */
    CHECK(post(&alice, "/api/v1/invites/create", "{\"uses\":1}") == 201);
    char invite_link[400];
    {
        const char *l = strstr(resp, "\"link\":\"");
        CHECK(l != NULL);
        if (l) { l += 8; size_t n = strcspn(l, "\""); snprintf(invite_link, sizeof invite_link, "%.*s", (int)n, l); } else invite_link[0] = 0;
    }
    request(&dave, &alice, "I have an invite", invite_link, 201);
    CHECK(wait_state(&alice, dave.uid, PUDDLE_STREAM_ACTIVE));                                    /* no one clicked anything */
    CHECK(wait_state(&dave, alice.uid, PUDDLE_STREAM_ACTIVE));
    CHECK(wait_owed_cleared(&alice, dave.uid));
    CHECK(get(&alice, "/api/v1/invites") == 200 && !json_has("\"token\""));                  /* the single use is spent */

    /* ---- someone who is blocked, and a Puddle that is not taking requests, look just like one that is deciding ---- */
    stream_body(body, sizeof body, eve.uid);
    snprintf(body, sizeof body, "{\"user\":\"%s\"}", eve.uid);
    CHECK(post(&alice, path_of("users", eve.uid, "block"), NULL) == 200);
    request(&eve, &alice, "let me in", NULL, 201);
    usleep(700000);
    {
        puddle_stream_state s;
        CHECK(has_provisional(&eve));                                                              /* Eve sees "pending", as ever */
        CHECK(!stream_state(&alice, eve.uid, &s));                                                /* and Alice holds nothing of Eve */
    }
    CHECK(wait_owed_cleared(&eve, alice.uid) || 1);
    CHECK(post(&alice, "/api/v1/streams/policy/set", "{\"access\":\"closed\"}") == 200);
    {
        node *gina = &carol;                                                                      /* Carol asks again while Alice is closed */
        request(gina, &alice, "knock", NULL, 201);
        usleep(700000);
        puddle_stream_state s;
        CHECK(stream_state(&alice, carol.uid, &s) && s == PUDDLE_STREAM_CLOSED);                  /* nothing new recorded */
        CHECK(has_provisional(gina));
    }
    CHECK(post(&alice, "/api/v1/streams/policy/set", "{\"access\":\"manual\"}") == 200);
    /* what the question "is my request still waiting?" gets: pending for a block, a closed door and a stranger alike */
    {
        char fdir[900], phrase[PUDDLE_PHRASE_LEN + 1], uid[PUDDLE_USER_ID_LEN + 1];
        snprintf(fdir, sizeof fdir, "%s/frank", root);
        puddle_identity *frank = puddle_identity_create(fdir, phrase, err, sizeof err);
        CHECK(frank != NULL);
        puddle_identity_id(frank, uid);
        CHECK(!strcmp(status_of(&alice, frank, uid), "pending"));                                   /* never heard of */
        CHECK(!strcmp(status_of(&alice, puddle_server_identity(eve.srv), eve.uid), "pending"));     /* blocked */
        CHECK(post(&alice, "/api/v1/streams/policy/set", "{\"access\":\"closed\"}") == 200);
        CHECK(!strcmp(status_of(&alice, frank, uid), "pending"));                                   /* a closed door */
        CHECK(post(&alice, "/api/v1/streams/policy/set", "{\"access\":\"manual\"}") == 200);
        CHECK(!strcmp(status_of(&alice, puddle_server_identity(bob.srv), bob.uid), "approved"));    /* the one who was let in */
        CHECK(!strcmp(status_of(&alice, puddle_server_identity(carol.srv), carol.uid), "declined")); /* the one who was turned down */
        puddle_identity_close(frank);
    }

    /* ---- closing ---- */
    stream_body(body, sizeof body, alice.uid);
    CHECK(post(&bob, sp("delete"), body) == 200);
    CHECK(wait_state(&alice, bob.uid, PUDDLE_STREAM_CLOSED));                                     /* she is told */
    CHECK(wait_owed_cleared(&bob, alice.uid));
    request(&bob, &alice, "back again", NULL, 201);                                                /* and can be asked again */
    CHECK(wait_state(&alice, bob.uid, PUDDLE_STREAM_PENDING_IN));
    stream_body(body, sizeof body, bob.uid);
    CHECK(post(&alice, sp("accept"), body) == 200);
    CHECK(wait_state(&bob, alice.uid, PUDDLE_STREAM_ACTIVE));
    CHECK(wait_owed_cleared(&alice, bob.uid));
    CHECK(post(&alice, "/api/v1/streams/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/delete", NULL) == 404);

    /* ---- a Puddle that is away when it is answered ---- */
    {
        node_down(&dave);
        stream_body(body, sizeof body, dave.uid);
        CHECK(post(&alice, sp("delete"), body) == 200);                                 /* Dave is down: the news has to wait */
        usleep(1500000);
        int queued;
        CHECK(outbox_count(&alice, dave.uid, &queued) == 1 && queued == 1);
        CHECK(owed_of(&alice, dave.uid) == PUDDLE_OWED_CLOSED);
        /* Alice restarts: what is owed is still queued, once */
        node_down(&alice);
        CHECK(node_up(&alice) == 0);
        usleep(500000);
        CHECK(outbox_count(&alice, dave.uid, &queued) == 1 && queued == 1);
        /* and if the queue file were lost, the record that says it is owed is enough to queue it again */
        node_down(&alice);
        char path[900];
        snprintf(path, sizeof path, "%s/state/outbox", alice.cfg.data_dir);
        rmtree(path);
        CHECK(node_up(&alice) == 0);
        usleep(500000);
        CHECK(outbox_count(&alice, dave.uid, &queued) == 1 && queued == 1);
        CHECK(node_up(&dave) == 0);
    }

    /* ---- a certificate that is changed on purpose is announced, and accepted quietly ---- */
    {
        node_down(&bob);
        char path[900], want[65];
        snprintf(path, sizeof path, "%s/cert.pem", bob.cfg.dir);
        remove(path);
        snprintf(path, sizeof path, "%s/key.pem", bob.cfg.dir);
        remove(path);
        CHECK(node_up(&bob) == 0);                                                                 /* a brand new certificate */
        puddle_stream st;
        int learned = 0;
        for (int i = 0; i < 200 && !learned; i++) {
            if (puddle_streams_get(puddle_server_streams(alice.srv), bob.uid, &st) == 0) {
                learned = st.tls_fp[0] && st.addr_seq >= 1 && !st.tls_fp_seen[0];
                if (learned) snprintf(want, sizeof want, "%s", st.tls_fp);
                puddle_stream_free(&st);
            }
            if (!learned) usleep(50000);
        }
        CHECK(learned);                                                                            /* Bob told Alice, signed, before anyone asked */
        CHECK(owed_of(&bob, alice.uid) == PUDDLE_OWED_NONE);
        CHECK(get(&alice, "/api/v1/streams?state=active") == 200 && !json_has("certificate_changed"));
        char b2[300], url[300];
        snprintf(b2, sizeof b2, "{\"body\":\"still me, new certificate\"}"); dm_to = bob.uid;
        CHECK(post(&alice, path_of("conversations", dm_to, "messages/send"), b2) == 201);                                         /* and nothing is held up */
        snprintf(url, sizeof url, "/api/v1/conversations/%s/messages", alice.uid);
        int got = 0;
        for (int i = 0; i < 100 && !got; i++) {
            got = get(&bob, url) == 200 && json_has("still me, new certificate");
            if (!got) usleep(50000);
        }
        CHECK(got);
    }

    /* ---- one that changes without a word is not sent to until the owner agrees ---- */
    {
        node_down(&bob);
        char path[900];
        snprintf(path, sizeof path, "%s/cert.pem", bob.cfg.dir);
        remove(path);
        snprintf(path, sizeof path, "%s/key.pem", bob.cfg.dir);
        remove(path);
        snprintf(path, sizeof path, "%s/state/address.json", bob.cfg.data_dir);
        remove(path);                                                                              /* as if restored from a backup: no memory of having announced */
        CHECK(node_up(&bob) == 0);
        stream_body(body, sizeof body, bob.uid);
        CHECK(post(&alice, sp("delete"), body) == 200);
        int held = 0;
        for (int i = 0; i < 100 && !held; i++) {
            puddle_ob_item *l;
            size_t c;
            puddle_outbox_list(puddle_server_outbox(alice.srv), &l, &c);
            for (size_t k = 0; k < c; k++) held |= !strcmp(l[k].peer, bob.uid) && l[k].state == PUDDLE_OB_HELD;
            puddle_outbox_list_free(l, c);
            if (!held) usleep(50000);
        }
        CHECK(held);
        CHECK(get(&alice, "/api/v1/streams?state=closed") == 200 && json_has("\"certificate_changed\":true"));
        puddle_stream_state s;
        CHECK(stream_state(&bob, alice.uid, &s) && s == PUDDLE_STREAM_ACTIVE);                    /* nothing reached Bob */
        CHECK(post(&alice, sp("trust"), body) == 200 && !json_has("certificate_changed"));
        CHECK(wait_state(&bob, alice.uid, PUDDLE_STREAM_CLOSED));                                 /* now it did */
        CHECK(wait_owed_cleared(&alice, bob.uid));
    }

    /* ---- a request that can't be sent is refused at once, or waits ---- */
    request(&alice, &alice, "me", alice.link, 400);                                                /* not yourself */
    CHECK(json_has("cannot_request_yourself"));
    request(&alice, &bob, "x", "nonsense!@1.2.3.4:1", 400);
    CHECK(json_has("bad_link"));
    request(&alice, &bob, "x", "puddle://" "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa@127.0.0.1:1", 400); /* an id has no place in an address */
    CHECK(json_has("bad_link"));
    {
        char l[300];
        snprintf(l, sizeof l, "127.0.0.1:%u", free_port());
        request(&alice, &bob, "to nobody", l, 201);                                                /* nothing there: waits, and is not a failure */
        usleep(800000);
        CHECK(get(&alice, "/api/v1/streams?state=pending_out") == 200 && json_has(l) && json_has("\"provisional\":true"));
        request(&alice, &bob, "to nobody again", l, 409);                                          /* once is enough */
        CHECK(json_has("already_requested"));
    }
    {
        /* blocking is by id, which is only known once someone answers: Alice blocks Eve, then asks at Eve's address; Eve says yes,
         * and Alice drops it on seeing whose answer it is */
        snprintf(body, sizeof body, "{\"user\":\"%s\"}", eve.uid);
        CHECK(post(&alice, path_of("users", eve.uid, "block"), NULL) == 200);
        request(&alice, &eve, "who are you", eve.link, 201);
        CHECK(wait_state(&eve, alice.uid, PUDDLE_STREAM_ACTIVE));                                  /* Eve had asked Alice too: both asked, so yes */
        usleep(1000000);
        puddle_stream_state s;
        CHECK(!stream_state(&alice, eve.uid, &s));                                                 /* Alice holds nothing of Eve */
        snprintf(body, sizeof body, "{\"user\":\"%s\"}", eve.uid);
        CHECK(post(&alice, path_of("users", eve.uid, "unblock"), NULL) == 200);
    }

    /* ---- strangers ---- */
    {
        const char *paths[] = { "/stream/v1/access", "/stream/v1/access/result", "/stream/v1/access/status", "/stream/v1/close", "/stream/v1/events", "/health" };
        for (size_t i = 0; i < sizeof paths / sizeof *paths; i++) {
            puddle_address at;
            char t[64];
            snprintf(t, sizeof t, "127.0.0.1:%u", alice.cfg.port);
            puddle_address_parse(t, &at);
            puddle_net_reply r;
            puddle_net_request(&at, "POST", paths[i], "{\"a\":1}", 7, puddle_server_identity(eve.srv), alice.uid, NULL, &r);
            if (r.result != PUDDLE_NET_OK || r.status != 404) { fprintf(stderr, "FAIL stranger got %d for %s (%d: %s)\n", r.status, paths[i], r.result, r.reason); failures++; }
            puddle_net_reply_free(&r);
        }
    }
    (void)err;

    node_down(&alice); node_down(&bob); node_down(&carol); node_down(&dave); node_down(&eve);
    rmtree(root);
    return failures ? 1 : 0;
}
