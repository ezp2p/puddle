#include "core/outbox.h"

#include "base/encoding.h"
#include "core/phrase.h"
#include "tlsserver.h"
#include "federation/sender.h"
#include "http/client.h"

#include <dirent.h>
#include <pthread.h>
#include <ftw.h>
#include <sys/stat.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define MIN (60 * 1000LL)
#define HOUR (60 * MIN)
#define T0 1760000000000LL

static char root[512];
static int rm_cb(const char *p, const struct stat *sb, int t, struct FTW *f) { (void)sb; (void)t; (void)f; remove(p); return 0; }

static int64_t clock_now = T0;
static int64_t now_cb(void) { return clock_now; }

/* what the Puddle knows about its peers, as the hooks see it */
#define MAXP 64
static struct {
    char id[64];
    char addr[2][64];
    int naddr;
    char fp[65];       /* recorded fingerprint, "" until first contact */
} peers[MAXP];
static int npeers;

static int find_peer(const char *id)
{
    for (int i = 0; i < npeers; i++) if (!strcmp(peers[i].id, id)) return i;
    return -1;
}

static int h_addresses(void *u, const char *peer, puddle_address *out, int max)
{
    (void)u;
    int i = find_peer(peer), n = 0;
    if (i < 0) return 0;
    for (int k = 0; k < peers[i].naddr && n < max; k++)
        if (puddle_address_parse(peers[i].addr[k], &out[n]) == 0) n++;
    return n;
}

static int h_fingerprint(void *u, const char *peer, const char *fp)
{
    (void)u;
    int i = find_peer(peer);
    if (i < 0) return 1;
    if (!peers[i].fp[0]) { snprintf(peers[i].fp, 65, "%s", fp); return 1; }
    return !strcmp(peers[i].fp, fp);
}

#define MAXLOG 128
static struct { size_t reply_len; char peer[64]; char kind[32]; int outcome; char reason[200]; int status; } done_log[MAXLOG];
static int ndone;
static puddle_outbox *g_ob;
static int enqueue_from_done; /* when set, the first delivery queues one more item to the same peer */

static pthread_mutex_t log_mu = PTHREAD_MUTEX_INITIALIZER;

static void h_done(void *u, const puddle_ob_item *it, puddle_ob_outcome o, const puddle_net_reply *r)
{
    (void)u;
    pthread_mutex_lock(&log_mu);
    if (ndone < MAXLOG) {
        snprintf(done_log[ndone].kind, 32, "%s", it->kind);
        snprintf(done_log[ndone].peer, 64, "%s", it->peer);
        done_log[ndone].reply_len = r && r->body ? r->body_len : 0;
        done_log[ndone].outcome = (int)o;
        snprintf(done_log[ndone].reason, 200, "%s", it->reason);
        done_log[ndone].status = it->status;
        ndone++;
    }
    pthread_mutex_unlock(&log_mu);
    if (enqueue_from_done && o == PUDDLE_OB_DELIVERED) {
        enqueue_from_done = 0;
        char err[200];
        CHECK(puddle_outbox_enqueue(g_ob, it->peer, "chained", "/stream/v1/x", "{\"n\":1}", 7, err, sizeof err) == 0);
    }
}

static puddle_identity *me;
static puddle_outbox_hooks hooks;

static puddle_outbox *open_ob(const char *dir)
{
    char err[300];
    puddle_outbox *ob = puddle_outbox_open(dir, &hooks, err, sizeof err);
    if (!ob) fprintf(stderr, "open: %s\n", err);
    g_ob = ob;
    return ob;
}

static void fake_id(int i, char *out)
{
    unsigned char raw[32];
    memset(raw, 0, sizeof raw);
    raw[0] = (unsigned char)i;
    raw[2] = 0x33;
    puddle_base32_encode(raw, 32, out, 64);
}

static int add_peer(int i, unsigned port)
{
    fake_id(i, peers[npeers].id);
    snprintf(peers[npeers].addr[0], 64, "127.0.0.1:%u", port);
    peers[npeers].naddr = 1;
    peers[npeers].fp[0] = '\0';
    return npeers++;
}

static int count(puddle_outbox *ob, puddle_ob_state st)
{
    puddle_ob_item *l;
    size_t n;
    int c = 0;
    puddle_outbox_list(ob, &l, &n);
    for (size_t i = 0; i < n; i++) c += l[i].state == st;
    puddle_outbox_list_free(l, n);
    return c;
}

static int files_in(const char *dir)
{
    char p[900];
    snprintf(p, sizeof p, "%s/state/outbox", dir);
    DIR *d = opendir(p);
    int n = 0;
    struct dirent *e;
    while (d && (e = readdir(d)))
        if (strstr(e->d_name, ".json")) n++;
    if (d) closedir(d);
    return n;
}

/* the newest item of this kind, copied; 1 if there is one */
static int find_kind(puddle_outbox *ob, const char *kind, puddle_ob_item *out)
{
    puddle_ob_item *l;
    size_t n;
    int found = 0;
    puddle_outbox_list(ob, &l, &n);
    for (size_t i = 0; i < n; i++)
        if (!strcmp(l[i].kind, kind)) {
            if (found) free(out->body);
            *out = l[i];
            out->body = strdup(l[i].body);
            found = 1;
        }
    puddle_outbox_list_free(l, n);
    return found;
}

/* the item with this seq, copied */
static int get_item(puddle_outbox *ob, size_t index, puddle_ob_item *out)
{
    puddle_ob_item *l;
    size_t n;
    puddle_outbox_list(ob, &l, &n);
    int ok = index < n;
    if (ok) { *out = l[index]; out->body = strdup(l[index].body); }
    puddle_outbox_list_free(l, n);
    return ok;
}

int main(void)
{
    const char *base = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/puddle-outbox-XXXXXX", base ? base : "/tmp");
    if (!mkdtemp(root)) return 2;
    char dir[700], phrase[PUDDLE_PHRASE_LEN + 1], err[300], data[700];
    snprintf(dir, sizeof dir, "%s/me", root);
    me = puddle_identity_create(dir, phrase, err, sizeof err);
    if (!me) return 2;
    snprintf(data, sizeof data, "%s/data", root);

    testsrv srv, srv2;
    snprintf(dir, sizeof dir, "%s/srv", root);
    if (ts_start(&srv, dir, 0) != 0) return 2;
    snprintf(dir, sizeof dir, "%s/srv2", root);
    if (ts_start(&srv2, dir, 0) != 0) return 2;

    hooks = (puddle_outbox_hooks){ .send = puddle_federation_send, .identity = me, .addresses = h_addresses, .fingerprint = h_fingerprint, .done = h_done,
                                   .now_ms = now_cb, .net = { .connect_timeout_s = 2, .io_timeout_s = 2, .total_timeout_s = 5 } };
    puddle_outbox *ob = open_ob(data);
    CHECK(ob != NULL);

    int a = add_peer(1, srv.port), b = add_peer(2, srv2.port);
    char *A = peers[a].id, *B = peers[b].id;

    /* ---- the retry schedule ---- */
    CHECK(puddle_outbox_retry_delay_ms(T0, T0) == 5 * MIN);
    CHECK(puddle_outbox_retry_delay_ms(T0, T0 + 24 * HOUR - 1) == 5 * MIN);
    CHECK(puddle_outbox_retry_delay_ms(T0, T0 + 24 * HOUR) == 30 * MIN);
    CHECK(puddle_outbox_retry_delay_ms(T0, T0 + 1000 * HOUR) == 30 * MIN);

    /* ---- what can be queued ---- */
    const char *ok_body = "{\"x\":1}";
    CHECK(puddle_outbox_enqueue(ob, "nope", "k", "/stream/v1/x", ok_body, 7, err, sizeof err) == -1 && err[0]);
    CHECK(puddle_outbox_enqueue(ob, A, "", "/stream/v1/x", ok_body, 7, err, sizeof err) == -1);
    CHECK(puddle_outbox_enqueue(ob, A, "Bad Kind", "/stream/v1/x", ok_body, 7, err, sizeof err) == -1);
    CHECK(puddle_outbox_enqueue(ob, A, "k", "/api/v1/x", ok_body, 7, err, sizeof err) == -1);       /* only the stream API */
    CHECK(puddle_outbox_enqueue(ob, A, "k", "/api/v1/stream/accept", ok_body, 7, err, sizeof err) == -1);
    CHECK(puddle_outbox_enqueue(ob, A, "k", "/stream/v2/accessxx", ok_body, 7, err, sizeof err) == -1);
    CHECK(puddle_outbox_enqueue(ob, A, "k", "/stream/v1/", ok_body, 7, err, sizeof err) == -1);
    CHECK(puddle_outbox_enqueue(ob, A, "k", "/stream/v1/a b", ok_body, 7, err, sizeof err) == -1);
    CHECK(puddle_outbox_enqueue(ob, A, "k", "/stream/v1/a?x=1", ok_body, 7, err, sizeof err) == -1);
    CHECK(puddle_outbox_enqueue(ob, A, "k", "/stream/v1/x", "[1]", 3, err, sizeof err) == -1);
    CHECK(puddle_outbox_enqueue(ob, A, "k", "/stream/v1/x", "", 0, err, sizeof err) == -1);
    CHECK(puddle_outbox_enqueue(ob, A, "k", "/stream/v1/x", "{\"x\":", 5, err, sizeof err) == -1);
    CHECK(puddle_outbox_enqueue(ob, A, "k", "/stream/v1/x", "{\"x\":1} trailing", 17, err, sizeof err) == -1);
    {
        char *big = malloc(PUDDLE_OUTBOX_BODY_MAX + 100);
        memset(big, ' ', PUDDLE_OUTBOX_BODY_MAX + 100);
        big[0] = '{';
        big[PUDDLE_OUTBOX_BODY_MAX + 99] = '}';
        CHECK(puddle_outbox_enqueue(ob, A, "k", "/stream/v1/x", big, PUDDLE_OUTBOX_BODY_MAX + 100, err, sizeof err) == -1);
        big[PUDDLE_OUTBOX_BODY_MAX - 1] = '}';
        big[PUDDLE_OUTBOX_BODY_MAX] = 0;
        memset(big + 1, ' ', PUDDLE_OUTBOX_BODY_MAX - 2);
        CHECK(puddle_outbox_enqueue(ob, A, "big", "/stream/v1/x", big, PUDDLE_OUTBOX_BODY_MAX, err, sizeof err) == 0); /* exactly the limit */
        free(big);
        puddle_ob_item *l;
        size_t n;
        puddle_outbox_list(ob, &l, &n);
        for (size_t i = 0; i < n; i++) puddle_outbox_dismiss(ob, l[i].seq); /* queued ones can't be dismissed */
        CHECK(n == 1);
        puddle_outbox_list_free(l, n);
        ts_set(&srv, TS_NORMAL, 200, "{}", 0);
        puddle_outbox_poll(ob);                                                                   /* send it, to clear the way */
        CHECK(count(ob, PUDDLE_OB_QUEUED) == 0 && srv.requests == 1 && srv.body_len == PUDDLE_OUTBOX_BODY_MAX);
        ndone = 0;
    }

    /* ---- delivery, in order, and nothing left behind ---- */
    CHECK(puddle_outbox_enqueue(ob, A, "one", "/stream/v1/access", "{\"n\":1}", 7, err, sizeof err) == 0);
    CHECK(puddle_outbox_enqueue(ob, A, "two", "/stream/v1/access/result", "{\"n\":2}", 7, err, sizeof err) == 0);
    CHECK(puddle_outbox_enqueue(ob, B, "three", "/stream/v1/close", "{\"n\":3}", 7, err, sizeof err) == 0);
    CHECK(files_in(data) == 3);
    ts_set(&srv2, TS_NORMAL, 200, "{}", 0);
    int before = srv.requests;
    puddle_outbox_poll(ob);
    CHECK(srv.requests == before + 2 && srv2.requests == 1);
    CHECK(!strcmp(srv.target, "/stream/v1/access/result") && srv.body_len == 7 && !memcmp(srv.body, "{\"n\":2}", 7) && !strcmp(srv2.target, "/stream/v1/close"));
    {
        /* the two peers run side by side, so their ends may come in either order; each peer's own are in order */
        int one = -1, two = -1, three = -1;
        for (int i = 0; i < ndone; i++) {
            if (!strcmp(done_log[i].kind, "one")) one = i;
            if (!strcmp(done_log[i].kind, "two")) two = i;
            if (!strcmp(done_log[i].kind, "three")) three = i;
            CHECK(done_log[i].outcome == PUDDLE_OB_DELIVERED && done_log[i].status == 200);
        }
        CHECK(ndone == 3 && one >= 0 && two > one && three >= 0);
    }
    CHECK(count(ob, PUDDLE_OB_QUEUED) == 0 && files_in(data) == 0);
    CHECK(!strcmp(peers[a].fp, srv.fingerprint) && !strcmp(peers[b].fp, srv2.fingerprint));        /* recorded at first contact */
    puddle_outbox_poll(ob);
    CHECK(srv.requests == before + 2);                                                             /* nothing is sent twice */

    /* a `done` that queues more: no deadlock, and it goes out in the same pass */
    ndone = 0;
    enqueue_from_done = 1;
    CHECK(puddle_outbox_enqueue(ob, A, "first", "/stream/v1/x", "{\"n\":1}", 7, err, sizeof err) == 0);
    before = srv.requests;
    puddle_outbox_poll(ob);
    CHECK(srv.requests == before + 2 && ndone == 2 && !strcmp(done_log[1].kind, "chained"));

    /* ---- offline: tried at the schedule, not before, and not all at once ---- */
    ts_stop(&srv);
    unsigned port_a = srv.port;
    CHECK(puddle_outbox_enqueue(ob, A, "o1", "/stream/v1/x", "{\"n\":1}", 7, err, sizeof err) == 0);
    CHECK(puddle_outbox_enqueue(ob, A, "o2", "/stream/v1/x", "{\"n\":2}", 7, err, sizeof err) == 0);
    ndone = 0;
    clock_now = T0 + 10 * MIN;
    int64_t t_off = clock_now;
    puddle_outbox_poll(ob);
    {
        puddle_ob_item i0, i1;
        CHECK(get_item(ob, 0, &i0) && get_item(ob, 1, &i1));
        CHECK(i0.state == PUDDLE_OB_QUEUED && i0.attempts == 1 && i1.attempts == 0);               /* only the oldest was tried */
        CHECK(i0.first_offline_ms == t_off && i1.first_offline_ms == t_off);
        CHECK(i0.next_ms == t_off + 5 * MIN && i1.next_ms == t_off + 5 * MIN);
        CHECK(strstr(i0.reason, "cannot reach"));
        free(i0.body); free(i1.body);
    }
    CHECK(ndone == 0);                                                                             /* not an outcome, just a wait */
    clock_now = t_off + 4 * MIN + 59 * 1000;
    puddle_outbox_poll(ob);
    {
        puddle_ob_item i0;
        CHECK(get_item(ob, 0, &i0) && i0.attempts == 1);                                           /* not due yet: not tried */
        free(i0.body);
    }
    clock_now = t_off + 5 * MIN;
    puddle_outbox_poll(ob);
    {
        puddle_ob_item i0;
        CHECK(get_item(ob, 0, &i0) && i0.attempts == 2 && i0.next_ms == t_off + 10 * MIN && i0.first_offline_ms == t_off);
        free(i0.body);
    }
    /* B is fine all the while, and does not wait on A */
    CHECK(puddle_outbox_enqueue(ob, B, "b1", "/stream/v1/x", "{\"n\":9}", 7, err, sizeof err) == 0);
    puddle_outbox_poll(ob);
    CHECK(srv2.requests == 2 && count(ob, PUDDLE_OB_QUEUED) == 2);

    /* it survives a restart, schedule and all */
    puddle_outbox_close(ob);
    ob = open_ob(data);
    {
        puddle_ob_item i0, i1;
        CHECK(count(ob, PUDDLE_OB_QUEUED) == 2 && get_item(ob, 0, &i0) && get_item(ob, 1, &i1));
        CHECK(!strcmp(i0.kind, "o1") && !strcmp(i1.kind, "o2") && i0.attempts == 2 && i0.next_ms == t_off + 10 * MIN && i0.first_offline_ms == t_off);
        CHECK(i1.seq == i0.seq + 1 && !strcmp(i0.body, "{\"n\":1}"));
        free(i0.body); free(i1.body);
        CHECK(puddle_outbox_enqueue(ob, B, "after", "/stream/v1/x", "{}", 2, err, sizeof err) == 0);
        puddle_ob_item i2;
        CHECK(get_item(ob, 2, &i2) && i2.seq > i1.seq);                                            /* numbering carries on */
        free(i2.body);
        puddle_outbox_poll(ob);
    }

    /* after a day away it slows to every half hour */
    clock_now = t_off + 24 * HOUR - 1;
    puddle_outbox_poll(ob);
    {
        puddle_ob_item i0;
        CHECK(get_item(ob, 0, &i0) && i0.next_ms == t_off + 24 * HOUR - 1 + 5 * MIN);              /* still inside the first day */
        free(i0.body);
    }
    clock_now = t_off + 24 * HOUR + 10 * MIN;
    puddle_outbox_poll(ob);
    {
        puddle_ob_item i0;
        CHECK(get_item(ob, 0, &i0) && i0.next_ms == clock_now + 30 * MIN && i0.first_offline_ms == t_off);
        free(i0.body);
    }
    ndone = 0;

    /* ...and when they come back, everything goes, oldest first, at once */
    {
        char dd[700];
        snprintf(dd, sizeof dd, "%s/srv", root);
        CHECK(ts_start(&srv, dd, port_a) == 0);
        ts_set(&srv, TS_NORMAL, 200, "{}", 0);
        clock_now += 30 * MIN;
        puddle_outbox_poll(ob);
        CHECK(srv.requests == 2 && ndone == 2 && !strcmp(done_log[0].kind, "o1") && !strcmp(done_log[1].kind, "o2"));
        CHECK(count(ob, PUDDLE_OB_QUEUED) == 0);
    }

    /* ---- anything else fails once, and stays failed ---- */
    ndone = 0;
    ts_set(&srv, TS_NORMAL, 404, "{\"error\":\"Not Found\"}", 0);
    CHECK(puddle_outbox_enqueue(ob, A, "f1", "/stream/v1/x", "{\"n\":1}", 7, err, sizeof err) == 0);
    CHECK(puddle_outbox_enqueue(ob, A, "f2", "/stream/v1/x", "{\"n\":2}", 7, err, sizeof err) == 0);
    before = srv.requests;
    puddle_outbox_poll(ob);
    CHECK(srv.requests == before + 2);                                                             /* each tried once */
    CHECK(count(ob, PUDDLE_OB_FAILED) == 2 && count(ob, PUDDLE_OB_QUEUED) == 0);
    CHECK(ndone == 2 && done_log[0].outcome == PUDDLE_OB_REJECTED && done_log[0].status == 404 && strstr(done_log[0].reason, "HTTP 404"));
    clock_now += 24 * HOUR;
    puddle_outbox_poll(ob);
    CHECK(srv.requests == before + 2);                                                             /* never again */
    {
        puddle_ob_item i0;
        CHECK(get_item(ob, 0, &i0) && i0.state == PUDDLE_OB_FAILED && i0.attempts == 1 && strstr(i0.reason, "404"));
        free(i0.body);
    }
    puddle_outbox_close(ob);
    ob = open_ob(data);
    CHECK(count(ob, PUDDLE_OB_FAILED) == 2);                                                       /* the reason outlives a restart */
    {
        puddle_ob_item *l;
        size_t n;
        puddle_outbox_list(ob, &l, &n);
        CHECK(n == 2 && puddle_outbox_dismiss(ob, l[0].seq) == 0 && puddle_outbox_dismiss(ob, l[0].seq) == 1 && puddle_outbox_dismiss(ob, 99999) == 1);
        puddle_outbox_list_free(l, n);
        CHECK(count(ob, PUDDLE_OB_FAILED) == 1 && files_in(data) == 1);
        puddle_outbox_list(ob, &l, &n);
        CHECK(puddle_outbox_dismiss(ob, l[0].seq) == 0);
        puddle_outbox_list_free(l, n);
    }
    /* a failure does not stop the items behind it */
    ndone = 0;
    ts_set(&srv, TS_NORMAL, 200, "{}", 0);
    pthread_mutex_lock(&srv.mu);
    srv.first_n = srv.requests + 1;
    srv.first_status = 400;
    pthread_mutex_unlock(&srv.mu);
    CHECK(puddle_outbox_enqueue(ob, A, "bad", "/stream/v1/x", "{\"n\":1}", 7, err, sizeof err) == 0);
    CHECK(puddle_outbox_enqueue(ob, A, "good", "/stream/v1/x", "{\"n\":2}", 7, err, sizeof err) == 0);
    puddle_outbox_poll(ob);
    CHECK(ndone == 2 && done_log[0].outcome == PUDDLE_OB_REJECTED && done_log[1].outcome == PUDDLE_OB_DELIVERED);
    CHECK(count(ob, PUDDLE_OB_FAILED) == 1);
    {
        puddle_ob_item *l;
        size_t n;
        puddle_outbox_list(ob, &l, &n);
        for (size_t i = 0; i < n; i++) puddle_outbox_dismiss(ob, l[i].seq);
        puddle_outbox_list_free(l, n);
    }
    /* TLS that does not work is a failure too, and so is a peer we know nothing about */
    ts_set(&srv, TS_PLAIN, 200, "", 0);
    ndone = 0;
    CHECK(puddle_outbox_enqueue(ob, A, "tls", "/stream/v1/x", "{}", 2, err, sizeof err) == 0);
    puddle_outbox_poll(ob);
    CHECK(ndone == 1 && done_log[0].outcome == PUDDLE_OB_REJECTED && strstr(done_log[0].reason, "TLS"));
    {
        char stranger[64];
        fake_id(77, stranger);
        CHECK(puddle_outbox_enqueue(ob, stranger, "who", "/stream/v1/x", "{}", 2, err, sizeof err) == 0);
        puddle_outbox_poll(ob);
        CHECK(ndone == 2 && done_log[1].outcome == PUDDLE_OB_REJECTED && strstr(done_log[1].reason, "no address"));
    }
    {
        puddle_ob_item *l;
        size_t n;
        puddle_outbox_list(ob, &l, &n);
        for (size_t i = 0; i < n; i++) puddle_outbox_dismiss(ob, l[i].seq);
        puddle_outbox_list_free(l, n);
    }
    ts_set(&srv, TS_NORMAL, 200, "{}", 0);

    /* ---- the certificate changes: hold everything, send nothing, until the owner says so ---- */
    {
        char old_fp[65];
        snprintf(old_fp, 65, "%s", peers[a].fp);
        snprintf(peers[a].fp, 65, "%s", "0000000000000000000000000000000000000000000000000000000000000000");
        ndone = 0;
        CHECK(puddle_outbox_enqueue(ob, A, "h1", "/stream/v1/x", "{\"n\":1}", 7, err, sizeof err) == 0);
        CHECK(puddle_outbox_enqueue(ob, A, "h2", "/stream/v1/x", "{\"n\":2}", 7, err, sizeof err) == 0);
        before = srv.requests;
        puddle_outbox_poll(ob);
        CHECK(srv.requests == before);                                                             /* nothing reached it */
        CHECK(count(ob, PUDDLE_OB_HELD) == 2 && count(ob, PUDDLE_OB_QUEUED) == 0);
        CHECK(ndone == 1 && done_log[0].outcome == PUDDLE_OB_HOLD && strstr(done_log[0].reason, "different certificate"));
        puddle_outbox_poll(ob);
        CHECK(ndone == 1 && srv.connections >= 1);                                                 /* and it is not tried again */
        puddle_outbox_close(ob);
        ob = open_ob(data);
        CHECK(count(ob, PUDDLE_OB_HELD) == 2);                                                     /* still held after a restart */
        CHECK(puddle_outbox_release(ob, B) == 0);                                                  /* another peer's: not ours to release */
        CHECK(puddle_outbox_release(ob, A) == 2 && count(ob, PUDDLE_OB_QUEUED) == 2);
        snprintf(peers[a].fp, 65, "%s", old_fp);                                                   /* the owner trusted the new one: here, the old */
        puddle_outbox_poll(ob);
        CHECK(srv.requests == before + 2 && count(ob, PUDDLE_OB_QUEUED) == 0 && ndone == 3);
        CHECK(puddle_outbox_release(ob, A) == 0);
    }

    /* ---- several addresses: the first that answers ---- */
    {
        int c = add_peer(5, 1);                                                                    /* nothing listens on port 1 */
        snprintf(peers[c].addr[1], 64, "127.0.0.1:%u", srv.port);
        peers[c].naddr = 2;
        before = srv.requests;
        CHECK(puddle_outbox_enqueue(ob, peers[c].id, "two_addrs", "/stream/v1/x", "{}", 2, err, sizeof err) == 0);
        puddle_outbox_poll(ob);
        CHECK(srv.requests == before + 1 && count(ob, PUDDLE_OB_QUEUED) == 0);
    }

    /* ---- the queue has a limit, and so does the pile of failures ---- */
    {
        puddle_outbox_close(ob);
        hooks.max_queued = 3;
        hooks.max_failed = 2;
        ob = open_ob(data);
        ts_stop(&srv2);                                                                            /* B is away: things wait */
        for (int i = 0; i < 3; i++) CHECK(puddle_outbox_enqueue(ob, B, "w", "/stream/v1/x", "{}", 2, err, sizeof err) == 0);
        CHECK(puddle_outbox_enqueue(ob, B, "w", "/stream/v1/x", "{}", 2, err, sizeof err) == -1 && strstr(err, "too many"));
        CHECK(count(ob, PUDDLE_OB_QUEUED) == 3 && files_in(data) == 3);
        puddle_outbox_close(ob);
        hooks.max_queued = 0;
        ob = open_ob(data);
        ts_set(&srv, TS_NORMAL, 500, "{}", 0);
        for (int i = 0; i < 4; i++) CHECK(puddle_outbox_enqueue(ob, A, "fail", "/stream/v1/x", "{}", 2, err, sizeof err) == 0);
        puddle_outbox_poll(ob);
        CHECK(count(ob, PUDDLE_OB_FAILED) == 2); /* only the newest two are kept */
        puddle_outbox_close(ob);
        hooks.max_failed = 2;
        ob = open_ob(data);
        for (int i = 0; i < 3; i++) CHECK(puddle_outbox_enqueue(ob, A, "fail", "/stream/v1/x", "{}", 2, err, sizeof err) == 0);
        puddle_outbox_poll(ob);
        CHECK(count(ob, PUDDLE_OB_FAILED) == 2);                                                   /* the oldest are forgotten */
        puddle_ob_item *l;
        size_t n;
        puddle_outbox_list(ob, &l, &n);
        for (size_t i = 0; i < n; i++) if (l[i].state == PUDDLE_OB_FAILED) puddle_outbox_dismiss(ob, l[i].seq);
        puddle_outbox_list_free(l, n);
        puddle_outbox_close(ob);
        hooks.max_failed = 0;
        ob = open_ob(data);
        ts_set(&srv, TS_NORMAL, 200, "{}", 0);
    }

    /* ---- damaged files are skipped, not fatal ---- */
    {
        puddle_outbox_close(ob);
        char p[900];
        snprintf(p, sizeof p, "%s/state/outbox/000000009999.json", data);
        FILE *f = fopen(p, "wb");
        fputs("{not json", f);
        fclose(f);
        snprintf(p, sizeof p, "%s/state/outbox/stray.json", data);
        f = fopen(p, "wb");
        fputs("{}", f);
        fclose(f);
        ob = open_ob(data);
        CHECK(ob != NULL);
    }

    /* ---- the news of a key change goes first; waiting items can be changed; "now" means now ---- */
    {
        ts_set(&srv, TS_NORMAL, 200, "{}", 0);
        ts_set(&srv2, TS_NORMAL, 200, "{}", 0);
        clock_now += 10 * MIN;
        puddle_outbox_poll(ob);                                                                   /* clear what was waiting */
        ndone = 0;
        CHECK(puddle_outbox_enqueue(ob, A, "first", "/stream/v1/x", "{\"n\":1}", 7, err, sizeof err) == 0);
        CHECK(puddle_outbox_enqueue(ob, A, "second", "/stream/v1/x", "{\"n\":2}", 7, err, sizeof err) == 0);
        CHECK(puddle_outbox_enqueue(ob, A, "identity", "/stream/v1/identity", "{\"chain\":\"c\"}", 13, err, sizeof err) == 0);
        puddle_outbox_poll(ob);
        CHECK(ndone == 3 && !strcmp(done_log[0].kind, "identity") && !strcmp(done_log[1].kind, "first") && !strcmp(done_log[2].kind, "second"));

        /* change what a waiting item will say, and have it survive a restart */
        ts_stop(&srv);
        CHECK(puddle_outbox_enqueue(ob, A, "edit", "/stream/v1/x", "{\"v\":1}", 7, err, sizeof err) == 0);
        puddle_outbox_poll(ob);                                                                   /* away: it waits */
        puddle_ob_item i0;
        CHECK(find_kind(ob, "edit", &i0) && i0.attempts == 1);
        CHECK(puddle_outbox_rewrite(ob, i0.seq, "{\"v\":2}", 7) == 0);
        CHECK(puddle_outbox_rewrite(ob, i0.seq, "[1]", 3) == -1 && puddle_outbox_rewrite(ob, i0.seq, "", 0) == -1 && puddle_outbox_rewrite(ob, i0.seq, "{\"v\":", 6) == -1);
        CHECK(puddle_outbox_rewrite(ob, 99999, "{}", 2) == 1);
        puddle_outbox_close(ob);
        ob = open_ob(data);
        free(i0.body);
        CHECK(find_kind(ob, "edit", &i0) && !strcmp(i0.body, "{\"v\":2}") && i0.attempts == 1 && i0.state == PUDDLE_OB_QUEUED);

        /* "now", not at the next scheduled time */
        {
            char dd[700];
            snprintf(dd, sizeof dd, "%s/srv", root);
            CHECK(ts_start(&srv, dd, port_a) == 0);
            ts_set(&srv, TS_NORMAL, 200, "{}", 0);
            before = srv.requests;
            puddle_outbox_poll(ob);
            CHECK(srv.requests == before);                                                        /* not yet due */
            CHECK(puddle_outbox_retry_now(ob, "zaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") == 0);   /* nothing of theirs is waiting */
            CHECK(puddle_outbox_retry_now(ob, A) == 1);
            puddle_outbox_poll(ob);
            CHECK(srv.requests == before + 1 && !strcmp((char *)srv.body, "{\"v\":2}"));
        }
        free(i0.body);
        /* a failed item is not for changing */
        ts_set(&srv, TS_NORMAL, 404, "{}", 0);
        CHECK(puddle_outbox_enqueue(ob, A, "doomed", "/stream/v1/x", "{}", 2, err, sizeof err) == 0);
        puddle_outbox_poll(ob);
        CHECK(find_kind(ob, "doomed", &i0) && i0.state == PUDDLE_OB_FAILED && puddle_outbox_rewrite(ob, i0.seq, "{}", 2) == 1);
        CHECK(puddle_outbox_dismiss(ob, i0.seq) == 0);
        free(i0.body);
        ts_set(&srv, TS_NORMAL, 200, "{}", 0);
        ndone = 0;
    }

    /* ---- fetching: a GET, whose answer reaches the hook ---- */
    {
        ts_set(&srv, TS_NORMAL, 200, "{\"answer\":true}", 0);
        ts_set(&srv2, TS_NORMAL, 200, "{}", 0);
        puddle_outbox_poll(ob);
        ndone = 0;
        const char *hash = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
        char path[200];
        snprintf(path, sizeof path, "/stream/v1/media?hash=%s", hash);
        CHECK(puddle_outbox_enqueue_get(ob, A, "media", path, 50000, err, sizeof err) == 0);
        puddle_ob_item g;
        CHECK(find_kind(ob, "media", &g) && !strcmp(g.method, "GET") && g.body_len == 0 && g.max_reply == 50000 && !strcmp(g.path, path));
        free(g.body);
        puddle_outbox_close(ob);
        ob = open_ob(data);                                                                       /* what it is survives a restart */
        CHECK(find_kind(ob, "media", &g) && !strcmp(g.method, "GET") && g.max_reply == 50000 && !strcmp(g.path, path));
        free(g.body);
        before = srv.requests;
        puddle_outbox_poll(ob);
        CHECK(srv.requests == before + 1 && !strcmp(srv.method, "GET") && !strcmp(srv.target, path) && srv.body_len == 0 && ndone == 1 &&
              done_log[0].outcome == PUDDLE_OB_DELIVERED && done_log[0].reply_len == 15);          /* the answer was handed over */
        /* a GET is held to its size: an answer over it fails, once */
        ts_set(&srv, TS_BIG, 200, "", 0);
        ndone = 0;
        CHECK(puddle_outbox_enqueue_get(ob, A, "media", path, 1000, err, sizeof err) == 0);
        puddle_outbox_poll(ob);
        CHECK(ndone == 1 && done_log[0].outcome == PUDDLE_OB_REJECTED && strstr(done_log[0].reason, "large"));
        ts_set(&srv, TS_NORMAL, 200, "{}", 0);
        puddle_ob_item *l;
        size_t n;
        puddle_outbox_list(ob, &l, &n);
        for (size_t i = 0; i < n; i++) if (l[i].state == PUDDLE_OB_FAILED) puddle_outbox_dismiss(ob, l[i].seq);
        puddle_outbox_list_free(l, n);
        /* ...and may be as large as the caller says */
        ts_set(&srv, TS_BIG, 200, "", 0);
        ndone = 0;
        CHECK(puddle_outbox_enqueue_get(ob, A, "media", path, 100000, err, sizeof err) == 0);
        puddle_outbox_poll(ob);
        CHECK(ndone == 1 && done_log[0].outcome == PUDDLE_OB_DELIVERED && done_log[0].reply_len == PUDDLE_NET_BODY_MAX + 20000);
        ts_set(&srv, TS_NORMAL, 200, "{}", 0);
        /* a file that says it is some other kind of request is not taken up at start */
        {
            puddle_outbox_close(ob);
            char f1[900], good[900];
            snprintf(good, sizeof good, "%s/state/outbox", data);
            DIR *d = opendir(good);
            struct dirent *e;
            while (d && (e = readdir(d)))
                if (strstr(e->d_name, ".json")) snprintf(f1, sizeof f1, "%s/%s", good, e->d_name);
            if (d) closedir(d);
            (void)f1;
            char bad[900];
            snprintf(bad, sizeof bad, "%s/000000090001.json", good);
            FILE *bf = fopen(bad, "wb");
            fprintf(bf, "{\"v\":1,\"seq\":90001,\"peer\":\"%s\",\"kind\":\"k\",\"method\":\"DELETE\",\"path\":\"/stream/v1/x\",\"body\":\"{}\",\"created\":1,\"next\":1,"
                       "\"first_offline\":0,\"attempts\":0,\"state\":\"queued\",\"status\":0,\"reason\":\"\"}", A);
            fclose(bf);
            ob = open_ob(data);
            puddle_ob_item *l2;
            size_t n2;
            puddle_outbox_list(ob, &l2, &n2);
            int seen_bad = 0;
            for (size_t i = 0; i < n2; i++) seen_bad |= l2[i].seq == 90001;
            CHECK(!seen_bad);
            puddle_outbox_list_free(l2, n2);
        }
        /* what a GET may and may not be */
        CHECK(puddle_outbox_enqueue_get(ob, A, "media", "/stream/v1/media?hash=a", 0, err, sizeof err) == -1);                       /* a size is needed */
        CHECK(puddle_outbox_enqueue_get(ob, A, "media", "/stream/v1/media?hash=a", 64u * 1024 * 1024 + 1, err, sizeof err) == -1);
        CHECK(puddle_outbox_enqueue_get(ob, A, "media", "/stream/v1/media?hash=a?b", 10, err, sizeof err) == -1);                     /* one query only */
        CHECK(puddle_outbox_enqueue_get(ob, A, "media", "/stream/v1/media?hash=A", 10, err, sizeof err) == -1);                        /* no capitals */
        CHECK(puddle_outbox_enqueue_get(ob, A, "media", "/stream/v1/media?hash=a b", 10, err, sizeof err) == -1);
        CHECK(puddle_outbox_enqueue_get(ob, A, "media", "/api/v1/media?hash=a", 10, err, sizeof err) == -1);
        CHECK(puddle_outbox_enqueue_get(ob, "nope", "media", "/stream/v1/media?hash=a", 10, err, sizeof err) == -1);
        CHECK(puddle_outbox_enqueue(ob, A, "k", "/stream/v1/x?y=1", "{}", 2, err, sizeof err) == -1);                                  /* a POST has no query */
        CHECK(puddle_outbox_enqueue(ob, A, "k", "/stream/v1/x=1", "{}", 2, err, sizeof err) == -1);                                    /* and no `=` in a path */
        CHECK(puddle_outbox_enqueue_get(ob, A, "media", "/stream/v1/media=hash", 10, err, sizeof err) == -1);
        ndone = 0;
    }

    /* ---- peers are delivered to side by side ---- */
    {
        puddle_outbox_close(ob);
        hooks.now_ms = now_cb;
        hooks.net.io_timeout_s = 6;
        hooks.net.total_timeout_s = 10;
        ob = open_ob(data);
        char dd[700];
        snprintf(dd, sizeof dd, "%s/srv2", root);
        ts_stop(&srv2);
        CHECK(ts_start(&srv2, dd, peers[b].addr[0][10] ? (unsigned)atoi(strrchr(peers[b].addr[0], ':') + 1) : 0) == 0);
        ts_set(&srv, TS_NORMAL, 200, "{}", 0);
        ts_set(&srv2, TS_NORMAL, 200, "{}", 0);
        clock_now += 10 * MIN;
        puddle_outbox_poll(ob);                                                                    /* clear what B was owed while it was away */
        CHECK(count(ob, PUDDLE_OB_QUEUED) == 0);
        ts_set(&srv, TS_SLOW, 200, "{}", 1500);
        ts_set(&srv2, TS_SLOW, 200, "{}", 1500);
        ndone = 0;
        CHECK(puddle_outbox_enqueue(ob, A, "slowa", "/stream/v1/x", "{}", 2, err, sizeof err) == 0 || (fprintf(stderr, "ERR: %s\n", err), 0));
        CHECK(puddle_outbox_enqueue(ob, B, "slowb", "/stream/v1/x", "{}", 2, err, sizeof err) == 0);
        struct timespec t1, t2;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        puddle_outbox_poll(ob);
        clock_gettime(CLOCK_MONOTONIC, &t2);
        double took = (double)(t2.tv_sec - t1.tv_sec) + (double)(t2.tv_nsec - t1.tv_nsec) / 1e9;
        CHECK(ndone == 2 && count(ob, PUDDLE_OB_QUEUED) == 0);
        CHECK(took >= 1.4 && took < 2.8);                                                          /* 1.5 s, not 3 */

        /* more peers than the limit all get done, and the same peer's items are still one at a time, in order */
        ts_set(&srv, TS_NORMAL, 200, "{}", 0);
        ndone = 0;
        before = srv.requests;
        int many = PUDDLE_OUTBOX_PARALLEL + 4;
        for (int i = 0; i < many; i++) {
            int pi = add_peer(20 + i, srv.port);
            CHECK(puddle_outbox_enqueue(ob, peers[pi].id, "m1", "/stream/v1/x", "{}", 2, err, sizeof err) == 0);
            CHECK(puddle_outbox_enqueue(ob, peers[pi].id, "m2", "/stream/v1/x", "{}", 2, err, sizeof err) == 0);
        }
        puddle_outbox_poll(ob);
        CHECK(srv.requests == before + 2 * many && ndone == 2 * many && count(ob, PUDDLE_OB_QUEUED) == 0);
        for (int i = 0; i < many; i++) { /* per peer, m1 ended before m2 */
            const char *id = peers[npeers - many + i].id;
            int p1 = -1, p2 = -1;
            for (int k = 0; k < ndone; k++)
                if (!strcmp(done_log[k].peer, id)) {
                    if (!strcmp(done_log[k].kind, "m1")) p1 = k;
                    if (!strcmp(done_log[k].kind, "m2")) p2 = k;
                }
            CHECK(p1 >= 0 && p2 > p1);
        }
        npeers -= many;
        ts_set(&srv, TS_NORMAL, 200, "{}", 0);
        ts_set(&srv2, TS_NORMAL, 200, "{}", 0);
    }

    /* a slow peer delays no one: with the worker running, a fast peer is served while a slow one is still being talked to */
    {
        puddle_outbox_close(ob);
        hooks.now_ms = NULL;
        ob = open_ob(data);
        ts_set(&srv, TS_SLOW, 200, "{}", 2500);
        ndone = 0;
        CHECK(puddle_outbox_start(ob) == 0);
        before = srv2.requests;
        CHECK(puddle_outbox_enqueue(ob, A, "slow", "/stream/v1/x", "{}", 2, err, sizeof err) == 0);
        usleep(300000);                                                                            /* A's delivery is under way */
        struct timespec t1, t2;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        CHECK(puddle_outbox_enqueue(ob, B, "fast", "/stream/v1/x", "{}", 2, err, sizeof err) == 0);
        for (int i = 0; i < 100 && srv2.requests == before; i++) usleep(20000);
        clock_gettime(CLOCK_MONOTONIC, &t2);
        double took = (double)(t2.tv_sec - t1.tv_sec) + (double)(t2.tv_nsec - t1.tv_nsec) / 1e9;
        CHECK(srv2.requests == before + 1 && took < 1.5);                                          /* B did not wait for A */
        /* more for the slow peer while it is being talked to: the same peer is never delivered to twice at once */
        int a_before = srv.requests;
        CHECK(puddle_outbox_enqueue(ob, A, "slow2", "/stream/v1/x", "{}", 2, err, sizeof err) == 0);
        usleep(300000);
        CHECK(puddle_outbox_enqueue(ob, A, "slow3", "/stream/v1/x", "{}", 2, err, sizeof err) == 0);
        for (int i = 0; i < 400 && count(ob, PUDDLE_OB_QUEUED) > 0; i++) usleep(50000);
        CHECK(count(ob, PUDDLE_OB_QUEUED) == 0);
        usleep(300000);
        CHECK(srv.requests == a_before + 3);                                                       /* slow, slow2 and slow3: each once */
        puddle_outbox_close(ob);
        ob = open_ob(data);
        ts_set(&srv, TS_NORMAL, 200, "{}", 0);

        /* closing while a delivery is in progress waits for it, and does not pull the outbox out from under it */
        ts_set(&srv, TS_SLOW, 200, "{}", 1200);
        CHECK(puddle_outbox_start(ob) == 0);
        before = srv.requests;
        CHECK(puddle_outbox_enqueue(ob, A, "closing", "/stream/v1/x", "{}", 2, err, sizeof err) == 0);
        usleep(300000);
        ndone = 0;
        puddle_outbox_close(ob);
        CHECK(ndone == 1 && srv.requests == before + 1);                                           /* it had finished by the time close returned */
        ob = open_ob(data);
        CHECK(count(ob, PUDDLE_OB_QUEUED) == 0);
        ts_set(&srv, TS_NORMAL, 200, "{}", 0);
    }

    /* ---- the worker does it by itself ---- */
    {
        puddle_outbox_close(ob);
        hooks.now_ms = NULL; /* the real clock */
        ob = open_ob(data);
        ndone = 0;
        CHECK(puddle_outbox_start(ob) == 0 && puddle_outbox_start(ob) == 0);
        before = srv.requests;
        CHECK(puddle_outbox_enqueue(ob, A, "auto", "/stream/v1/x", "{}", 2, err, sizeof err) == 0);
        for (int i = 0; i < 100 && srv.requests == before; i++) usleep(50000);
        CHECK(srv.requests == before + 1);
        for (int i = 0; i < 100 && ndone == 0; i++) usleep(50000);
        CHECK(ndone == 1 && done_log[0].outcome == PUDDLE_OB_DELIVERED);
        puddle_outbox_close(ob);                                                                   /* stops and joins the worker */
    }

    ts_stop(&srv);
    ts_stop(&srv2);
    puddle_identity_close(me);
    nftw(root, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    return failures ? 1 : 0;
}
