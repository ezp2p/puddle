#include "federation/flow.h"

#include "federation/access.h"
#include "core/phrase.h"
#include "federation/courier.h"
#include "federation/sender.h"
#include "http/client.h"

#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define T0 1760000000000LL
static char root[512], data[700];
static int rm_cb(const char *p, const struct stat *sb, int t, struct FTW *f) { (void)sb; (void)t; (void)f; remove(p); return 0; }
static int64_t clock_now = T0;
static int64_t now_cb(void) { return clock_now; }

static puddle_ctx ctx;
static puddle_config cfg;
static puddle_streams *ss;
static puddle_outbox *ob;
static puddle_profiles *profiles;
static puddle_assignments *as;
static puddle_identity *me;
static char my_id[PUDDLE_USER_ID_LEN + 1];

static void make_id(int i, char *out)
{
    static const char *const ids[] = {
        "oaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "baaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "caaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "daaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "eaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "faaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" };
    snprintf(out, 64, "%s", ids[i]);
}

static puddle_link link_to(const char *addr, const char *invite)
{
    puddle_link l;
    char t[400], err[100];
    puddle_link_format(addr, invite, t, sizeof t);
    if (puddle_link_parse(t, &l, err, sizeof err) != 0) { fprintf(stderr, "bad link %s: %s\n", t, err); exit(2); }
    return l;
}

typedef struct { puddle_stream_state state; puddle_owed owed; const char *error; int create; } setup;
static int set_apply(void *u, puddle_stream *st, int exists)
{
    setup *s = u;
    (void)exists;
    st->state = s->state;
    st->owed = s->owed;
    snprintf(st->error, sizeof st->error, "%s", s->error);
    return 0;
}
static void set_record(const char *id, puddle_stream_state st, puddle_owed owed, const char *error)
{
    setup s = { st, owed, error, 1 };
    char err[200];
    CHECK(puddle_streams_update(ss, id, set_apply, &s, err, sizeof err) == 0);
}
static void rec_of(const char *id, puddle_stream *out) { CHECK(puddle_streams_get(ss, id, out) == 0); }

static int queued_of(const char *peer, const char *kind)
{
    puddle_ob_item *l;
    size_t n;
    int c = 0;
    puddle_outbox_list(ob, &l, &n);
    for (size_t i = 0; i < n; i++)
        c += !strcmp(l[i].peer, peer) && (!kind || !strcmp(l[i].kind, kind)) && l[i].state == PUDDLE_OB_QUEUED;
    puddle_outbox_list_free(l, n);
    return c;
}

/* the body of the newest queued item for `peer` and `kind`, as a malloc'd string */
static char *last_body(const char *peer, const char *kind, char *path_out)
{
    puddle_ob_item *l;
    size_t n;
    char *body = NULL;
    puddle_outbox_list(ob, &l, &n);
    for (size_t i = 0; i < n; i++)
        if (!strcmp(l[i].peer, peer) && !strcmp(l[i].kind, kind)) {
            free(body);
            body = strdup(l[i].body);
            if (path_out) snprintf(path_out, 128, "%s", l[i].path);
        }
    puddle_outbox_list_free(l, n);
    return body;
}

static puddle_ob_item item_of(const char *peer, const char *kind, const char *body, const char *reason)
{
    puddle_ob_item it;
    memset(&it, 0, sizeof it);
    snprintf(it.peer, sizeof it.peer, "%s", peer);
    snprintf(it.kind, sizeof it.kind, "%s", kind);
    it.body = (char *)body;
    it.body_len = strlen(body);
    snprintf(it.reason, sizeof it.reason, "%s", reason);
    return it;
}

int main(void)
{
    const char *base = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/puddle-flow-XXXXXX", base ? base : "/tmp");
    if (!mkdtemp(root)) return 2;
    char err[300], phrase[PUDDLE_PHRASE_LEN + 1], dir[700];
    snprintf(dir, sizeof dir, "%s/me", root);
    me = puddle_identity_create(dir, phrase, err, sizeof err);
    if (!me) return 2;
    puddle_identity_confirm(dir);
    puddle_identity_id(me, my_id);
    snprintf(data, sizeof data, "%s/data", root);
    mkdir(root, 0700);
    snprintf(dir, sizeof dir, "%s/profiles.yaml", root);
    FILE *f = fopen(dir, "w");
    fputs("profiles:\n  - id: main\n    display_name: Me\n    blurb: the default\n  - id: work\n    display_name: Me at work\n", f);
    fclose(f);
    memset(&cfg, 0, sizeof cfg);
    snprintf(cfg.dir, sizeof cfg.dir, "%s", root);
    snprintf(cfg.data_dir, sizeof cfg.data_dir, "%s", data);
    snprintf(cfg.address, sizeof cfg.address, "127.0.0.1");
    cfg.port = 8443;
    snprintf(cfg.profile_files[0], PUDDLE_PATH_MAX, "%s", dir);
    cfg.nprofile_files = 1;
    char old_default[PUDDLE_PROFILE_ID_MAX + 1];
    if (!(profiles = puddle_profiles_open(&cfg, old_default, err, sizeof err))) { fprintf(stderr, "%s\n", err); return 2; }
    ss = puddle_streams_open(data, err, sizeof err);
    as = puddle_assignments_open(data, err, sizeof err);
    puddle_outbox_hooks hooks = { .send = puddle_federation_send, .identity = me, .now_ms = now_cb, .max_queued = 6 };
    ob = puddle_outbox_open(data, &hooks, err, sizeof err);
    ctx = (puddle_ctx){ .courier = &puddle_federation_courier, .config = &cfg, .identity = me, .profiles = profiles, .assignments = as, .streams = ss, .outbox = ob, .now_ms = now_cb };

    char a[64], b[64], c[64], d[64], e[64], g[64], id_out[64];
    make_id(0, a); make_id(1, b); make_id(2, c); make_id(3, d); make_id(4, e); make_id(5, g);
    const char *code, *msg;
    puddle_stream rec;

    /* ---- our address ---- */
    {
        char out[200];
        CHECK(puddle_my_address(&cfg, out, sizeof out) && !strcmp(out, "127.0.0.1:8443"));
        puddle_config c2 = cfg;
        snprintf(c2.address, sizeof c2.address, "::1");
        CHECK(puddle_my_address(&c2, out, sizeof out) && !strcmp(out, "[::1]:8443"));
        snprintf(c2.address, sizeof c2.address, "::");
        CHECK(!puddle_my_address(&c2, out, sizeof out));
        snprintf(c2.address, sizeof c2.address, "0.0.0.0");
        CHECK(!puddle_my_address(&c2, out, sizeof out));
        snprintf(c2.advertise, sizeof c2.advertise, "Home.Example.org:9000");
        CHECK(puddle_my_address(&c2, out, sizeof out) && !strcmp(out, "home.example.org:9000"));
        CHECK(!puddle_my_address(&c2, out, 8));                                                   /* does not fit */
    }

    /* ---- asking: by address; the record is filed under a made-up id until the answer says who it is ---- */
    char pa[64]; /* the made-up id of the request to `l` */
    {
        puddle_link self = link_to("127.0.0.1:8443", NULL);
        CHECK(puddle_flow_request(&ctx, &self, "", NULL, id_out, &code, &msg) == 400 && !strcmp(code, "cannot_request_yourself"));
        puddle_link l = link_to("203.0.113.7:9000", "abcdefghijklmnopqrstuvwxy2");
        char longmsg[700];
        memset(longmsg, 'm', 501); longmsg[501] = 0;
        CHECK(puddle_flow_request(&ctx, &l, longmsg, NULL, id_out, &code, &msg) == 400 && !strcmp(code, "bad_request"));
        CHECK(puddle_flow_request(&ctx, &l, "hi", "nope", id_out, &code, &msg) == 400 && !strcmp(code, "unknown_profile"));
        {
            puddle_ctx noaddr = ctx;
            puddle_config c2 = cfg;
            snprintf(c2.address, sizeof c2.address, "::");
            noaddr.config = &c2;
            CHECK(puddle_flow_request(&noaddr, &l, "hi", NULL, id_out, &code, &msg) == 409 && !strcmp(code, "no_address") && strstr(msg, "advertise"));
        }
        {
            puddle_stream *all;
            size_t n;
            CHECK(puddle_streams_list(ss, &all, &n) == 0 && n == 0);                              /* none of those left a record... */
            puddle_stream_list_free(all, n);
        }

        /* the real thing */
        char path[128];
        CHECK(puddle_flow_request(&ctx, &l, "  hello\\there  ", "work", id_out, &code, &msg) == 0 && puddle_id_valid(id_out));
        memcpy(pa, id_out, sizeof pa);
        rec_of(pa, &rec);
        CHECK(rec.state == PUDDLE_STREAM_PENDING_OUT && rec.provisional && strlen(rec.ticket) == 32 && rec.naddr == 1 &&
              !strcmp(rec.addresses[0], "203.0.113.7:9000") && !strcmp(rec.note, "hello\\there") && rec.owed == PUDDLE_OWED_NONE && rec.created_ms == T0);
        char ticket_a[33];
        memcpy(ticket_a, rec.ticket, sizeof ticket_a);
        puddle_stream_free(&rec);
        char prof[PUDDLE_PROFILE_ID_MAX + 1];
        char place[PUDDLE_PLACE_MAX];
        CHECK(puddle_place_dm(pa, place) == 0 && puddle_assignments_get(as, place, prof) == 0 && !strcmp(prof, "work")); /* they see the profile we chose */
        CHECK(queued_of(pa, PUDDLE_OB_KIND_OPEN_ACCESS) == 1 && queued_of(pa, "access") == 0);
        char *body = last_body(pa, PUDDLE_OB_KIND_OPEN_ACCESS, path);
        CHECK(body && !strcmp(path, "/stream/v1/access"));
        {
            puddle_span req, v;
            char s[300];
            CHECK(puddle_json_member(body, strlen(body), "requester", &req) == 0);
            puddle_peer_info pi;
            CHECK(puddle_peer_parse(req.p, req.len, &pi) == 0 && !strcmp(pi.id, my_id) && !strcmp(pi.address, "127.0.0.1:8443"));
            CHECK(strstr(pi.profile, "Me at work") && !strstr(pi.profile, "the default"));
            unsigned char pub[32];
            char kid[65];
            puddle_identity_public(me, pub, NULL, NULL);
            puddle_key_id(pub, kid);
            CHECK(puddle_peer_key(&pi, my_id, kid, pub) == 1);                                    /* a chain the other side can verify */
            puddle_peer_info_free(&pi);
            CHECK(puddle_json_member(body, strlen(body), "message", &v) == 0 && puddle_json_string(v, s, sizeof s) == 0 && !strcmp(s, "hello\\there"));
            CHECK(puddle_json_member(body, strlen(body), "invite", &v) == 0 && puddle_json_string(v, s, sizeof s) == 0 && !strcmp(s, "abcdefghijklmnopqrstuvwxy2"));
            CHECK(puddle_json_member(body, strlen(body), "ticket", &v) == 0 && puddle_json_string(v, s, sizeof s) == 0 && !strcmp(s, ticket_a)); /* and the ticket */
        }
        free(body);

        /* the same address again, while one is waiting, or once it is a stream */
        CHECK(puddle_flow_request(&ctx, &l, "again", NULL, id_out, &code, &msg) == 409 && !strcmp(code, "already_requested"));
        set_record(pa, PUDDLE_STREAM_ACTIVE, PUDDLE_OWED_NONE, "");
        CHECK(puddle_flow_request(&ctx, &l, "again", NULL, id_out, &code, &msg) == 409 && !strcmp(code, "already_streaming"));
        set_record(pa, PUDDLE_STREAM_PENDING_IN, PUDDLE_OWED_NONE, "");
        CHECK(puddle_flow_request(&ctx, &l, "again", NULL, id_out, &code, &msg) == 409 && !strcmp(code, "they_asked") && strstr(msg, "accept"));
        CHECK(queued_of(pa, PUDDLE_OB_KIND_OPEN_ACCESS) == 1);
        set_record(pa, PUDDLE_STREAM_CLOSED, PUDDLE_OWED_NONE, "an old failure");                  /* closed: ask again, as a new request */
        clock_now += 5000;
        CHECK(puddle_flow_request(&ctx, &l, "second try", NULL, id_out, &code, &msg) == 0 && strcmp(id_out, pa) != 0);
        rec_of(id_out, &rec);
        CHECK(rec.state == PUDDLE_STREAM_PENDING_OUT && rec.provisional && !rec.error[0] && !strcmp(rec.note, "second try") && rec.created_ms == T0 + 5000 &&
              strcmp(rec.ticket, ticket_a) != 0);
        puddle_stream_free(&rec);
        CHECK(queued_of(id_out, PUDDLE_OB_KIND_OPEN_ACCESS) == 1);
        body = last_body(id_out, PUDDLE_OB_KIND_OPEN_ACCESS, NULL);
        CHECK(body && strstr(body, "second try") && strstr(body, "\"invite\":\"abcdefghijklmnopqrstuvwxy2\""));
        free(body);

        /* the outbox is full: nothing is left half done */
        puddle_link lb = link_to("203.0.113.8:9000", NULL), lc = link_to("203.0.113.9:9000", NULL), ld = link_to("203.0.113.10:9000", NULL),
                    le = link_to("203.0.113.11:9000", NULL), lg = link_to("203.0.113.12:9000", NULL);
        CHECK(puddle_flow_request(&ctx, &lb, "x", NULL, id_out, &code, &msg) == 0);              /* 3 queued */
        CHECK(puddle_flow_request(&ctx, &lc, "x", NULL, id_out, &code, &msg) == 0);              /* 4 */
        CHECK(puddle_flow_request(&ctx, &ld, "x", NULL, id_out, &code, &msg) == 0);              /* 5 */
        CHECK(puddle_flow_request(&ctx, &le, "x", NULL, id_out, &code, &msg) == 0);              /* 6: full */
        puddle_stream *before, *after;
        size_t nb, na;
        CHECK(puddle_streams_list(ss, &before, &nb) == 0);
        CHECK(puddle_flow_request(&ctx, &lg, "x", NULL, id_out, &code, &msg) == 500);
        CHECK(puddle_streams_list(ss, &after, &na) == 0 && na == nb);                             /* no record left looking like it is on its way */
        puddle_stream_list_free(before, nb);
        puddle_stream_list_free(after, na);
    }

    /* ---- what is owed ---- */
    {
        puddle_outbox_close(ob);
        char path[900];
        snprintf(path, sizeof path, "%s/state/outbox", data);
        nftw(path, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
        hooks.max_queued = 0;
        ob = puddle_outbox_open(data, &hooks, err, sizeof err);
        ctx.outbox = ob;

        set_record(d, PUDDLE_STREAM_ACTIVE, PUDDLE_OWED_APPROVED, "");
        CHECK(puddle_flow_owe(&ctx, d, PUDDLE_OWED_APPROVED) == 0);
        char p[128];
        char *body = last_body(d, "result", p);
        CHECK(body && !strcmp(p, "/stream/v1/access/result") && strstr(body, "\"approved\":true") && strstr(body, "\"responder\":{\"id\":\""));
        free(body);
        CHECK(puddle_flow_owe(&ctx, d, PUDDLE_OWED_DECLINED) == 0);
        body = last_body(d, "result", p);
        CHECK(body && strstr(body, "\"approved\":false"));
        free(body);
        CHECK(puddle_flow_owe(&ctx, d, PUDDLE_OWED_CLOSED) == 0);
        body = last_body(d, "close", p);
        CHECK(body && !strcmp(p, "/stream/v1/close") && !strcmp(body, "{}"));
        free(body);
        CHECK(puddle_flow_owe(&ctx, d, PUDDLE_OWED_NONE) == -1);
        puddle_ctx none = ctx;
        none.outbox = NULL;
        CHECK(puddle_flow_owe(&none, d, PUDDLE_OWED_APPROVED) == -1);
        CHECK(queued_of(d, NULL) == 3);

        /* at start: what a record says is owed and the queue does not hold is queued; what is held is not doubled */
        set_record(e, PUDDLE_STREAM_ACTIVE, PUDDLE_OWED_APPROVED, "");
        set_record(g, PUDDLE_STREAM_CLOSED, PUDDLE_OWED_CLOSED, "");
        set_record(c, PUDDLE_STREAM_CLOSED, PUDDLE_OWED_NONE, "");
        set_record(d, PUDDLE_STREAM_CLOSED, PUDDLE_OWED_CLOSED, "");                              /* already has a "close" waiting */
        CHECK(puddle_flow_resume(&ctx) == 2);
        CHECK(queued_of(e, "result") == 1 && queued_of(g, "close") == 1 && queued_of(c, NULL) == 0 && queued_of(d, "close") == 1);
        CHECK(puddle_flow_resume(&ctx) == 0);                                                      /* and not again */
        CHECK(puddle_flow_resume(&none) == 0);
    }

    /* ---- how an item turned out ---- */
    {
        const char *approved = "{\"responder\":{},\"approved\":true}", *declined = "{\"responder\":{},\"approved\":false}";
        puddle_ob_item it;

        /* an answer was delivered: it is no longer owed */
        set_record(a, PUDDLE_STREAM_ACTIVE, PUDDLE_OWED_APPROVED, "earlier trouble");
        it = item_of(a, "result", approved, "");
        puddle_flow_done(&ctx, &it, PUDDLE_OB_DELIVERED, NULL);
        rec_of(a, &rec);
        CHECK(rec.owed == PUDDLE_OWED_NONE && !rec.error[0] && rec.state == PUDDLE_STREAM_ACTIVE);
        puddle_stream_free(&rec);
        /* ...but only that answer: a newer decision is still owed */
        set_record(a, PUDDLE_STREAM_CLOSED, PUDDLE_OWED_CLOSED, "");
        puddle_flow_done(&ctx, &it, PUDDLE_OB_DELIVERED, NULL);
        rec_of(a, &rec);
        CHECK(rec.owed == PUDDLE_OWED_CLOSED);
        puddle_stream_free(&rec);
        it = item_of(a, "close", "{}", "");
        puddle_flow_done(&ctx, &it, PUDDLE_OB_DELIVERED, NULL);
        rec_of(a, &rec);
        CHECK(rec.owed == PUDDLE_OWED_NONE);
        puddle_stream_free(&rec);
        /* a decline is a decline, not an approval */
        set_record(a, PUDDLE_STREAM_CLOSED, PUDDLE_OWED_DECLINED, "");
        it = item_of(a, "result", approved, "");
        puddle_flow_done(&ctx, &it, PUDDLE_OB_DELIVERED, NULL);
        rec_of(a, &rec);
        CHECK(rec.owed == PUDDLE_OWED_DECLINED);                                                 /* an "approved" item says nothing of it */
        puddle_stream_free(&rec);
        it = item_of(a, "result", declined, "");
        puddle_flow_done(&ctx, &it, PUDDLE_OB_DELIVERED, NULL);
        rec_of(a, &rec);
        CHECK(rec.owed == PUDDLE_OWED_NONE);
        puddle_stream_free(&rec);

        /* it could not be delivered: given up, with the reason where the owner will see it */
        set_record(a, PUDDLE_STREAM_ACTIVE, PUDDLE_OWED_APPROVED, "");
        it = item_of(a, "result", approved, "they answered HTTP 404");
        puddle_flow_done(&ctx, &it, PUDDLE_OB_REJECTED, NULL);
        rec_of(a, &rec);
        CHECK(rec.owed == PUDDLE_OWED_NONE && strstr(rec.error, "could not be told") && strstr(rec.error, "HTTP 404") && rec.state == PUDDLE_STREAM_ACTIVE);
        puddle_stream_free(&rec);

        /* a held item changes nothing */
        set_record(a, PUDDLE_STREAM_ACTIVE, PUDDLE_OWED_APPROVED, "");
        it = item_of(a, "result", approved, "different certificate");
        puddle_flow_done(&ctx, &it, PUDDLE_OB_HOLD, NULL);
        rec_of(a, &rec);
        CHECK(rec.owed == PUDDLE_OWED_APPROVED && !rec.error[0]);
        puddle_stream_free(&rec);

        /* our request: delivery changes nothing, a refusal closes it, and only it */
        set_record(b, PUDDLE_STREAM_PENDING_OUT, PUDDLE_OWED_NONE, "");
        it = item_of(b, "access", "{}", "");
        puddle_flow_done(&ctx, &it, PUDDLE_OB_DELIVERED, NULL);
        CHECK(puddle_streams_get(ss, b, &rec) == 0 && rec.state == PUDDLE_STREAM_PENDING_OUT);
        puddle_stream_free(&rec);
        puddle_flow_done(&ctx, &it, PUDDLE_OB_HOLD, NULL);
        CHECK(puddle_streams_get(ss, b, &rec) == 0 && rec.state == PUDDLE_STREAM_PENDING_OUT);
        puddle_stream_free(&rec);
        snprintf(it.reason, sizeof it.reason, "they answered HTTP 404");
        puddle_flow_done(&ctx, &it, PUDDLE_OB_REJECTED, NULL);
        rec_of(b, &rec);
        CHECK(rec.state == PUDDLE_STREAM_CLOSED && strstr(rec.error, "could not be asked") && strstr(rec.error, "HTTP 404"));
        puddle_stream_free(&rec);
        set_record(b, PUDDLE_STREAM_ACTIVE, PUDDLE_OWED_NONE, "");                                /* it was answered meanwhile */
        puddle_flow_done(&ctx, &it, PUDDLE_OB_REJECTED, NULL);
        rec_of(b, &rec);
        CHECK(rec.state == PUDDLE_STREAM_ACTIVE && !rec.error[0]);
        puddle_stream_free(&rec);

        /* kinds we do not know, and peers we have no record of */
        it = item_of(b, "strange", "{}", "");
        puddle_flow_done(&ctx, &it, PUDDLE_OB_REJECTED, NULL);
        rec_of(b, &rec);
        CHECK(rec.state == PUDDLE_STREAM_ACTIVE && !rec.error[0]);
        puddle_stream_free(&rec);
        char nobody[64];
        snprintf(nobody, sizeof nobody, "%s", "zaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
        it = item_of(nobody, "close", "{}", "x");
        puddle_flow_done(&ctx, &it, PUDDLE_OB_REJECTED, NULL);
        CHECK(puddle_streams_get(ss, nobody, &rec) == 1);                                         /* nothing is made up for them */
    }

    puddle_outbox_close(ob);
    puddle_assignments_close(as);
    puddle_streams_close(ss);
    puddle_profiles_close(profiles);
    puddle_identity_close(me);
    nftw(root, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    return failures ? 1 : 0;
}
