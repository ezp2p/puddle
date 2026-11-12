#include "http/router.h"
#include "core/people.h"

#include "base/encoding.h"
#include "core/phrase.h"
#include "service/routes.h"
#include "federation/courier.h"

#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define T0 1760000000000LL

static char root[512];
static int rm_cb(const char *p, const struct stat *sb, int t, struct FTW *f) { (void)sb; (void)t; (void)f; remove(p); return 0; }
static int64_t clock_now = T0;
static int64_t now_cb(void) { return clock_now; }

static puddle_identity *me;
static char my_id[PUDDLE_USER_ID_LEN + 1];
static puddle_ctx ctx;
static puddle_router R;
static puddle_streams *ss;
static puddle_people *ps;

static puddle_config cfg;

typedef struct { int status; char out[40000]; size_t n; } reply;

static reply *call(const char *method, const char *target, const char *body, int owner)
{
    static reply rp;
    puddle_request req;
    memset(&req, 0, sizeof req);
    snprintf(req.method, sizeof req.method, "%s", method);
    snprintf(req.target, sizeof req.target, "%s", target);
    snprintf(req.path, sizeof req.path, "%s", target);
    char *q = strchr(req.path, '?');
    if (q) *q = '\0';
    size_t bl = body ? strlen(body) : 0;
    req.content_length = bl;
    unsigned char *copy = malloc(bl + 1);
    if (bl) memcpy(copy, body, bl);
    copy[bl] = 0;
    memset(&rp, 0, sizeof rp);
    rp.n = puddle_router_handle(&R, &req, owner, copy, bl, rp.out, sizeof rp.out - 1, &rp.status);
    free(copy);
    return &rp;
}

static const char *body_of(const reply *r)
{
    const char *b = strstr(r->out, "\r\n\r\n");
    return b ? b + 4 : "";
}

static int valid_body(const reply *r) { const char *b = body_of(r); return puddle_json_valid_object(b, strlen(b)); }

static int status(const char *method, const char *target, const char *body) { return call(method, target, body, 1)->status; }

/* "/api/v1/streams/<id>/<verb>", or "/api/v1/users/<id>/<verb>" */
static const char *at(const char *kind, const char *id, const char *verb)
{
    static char buf[4][200];
    static int k;
    k = (k + 1) % 4;
    snprintf(buf[k], sizeof buf[k], "/api/v1/%s/%s%s%s", kind, id, verb ? "/" : "", verb ? verb : "");
    return buf[k];
}

static void fake_id(int i, char *out)
{
    unsigned char raw[32];
    memset(raw, 0, sizeof raw);
    raw[0] = (unsigned char)(i & 255);
    raw[1] = (unsigned char)(i >> 8);
    raw[2] = 0x5a;
    puddle_base32_encode(raw, 32, out, 64);
}

static void put(const char *id, puddle_stream_state st, int64_t updated, const char *note, const char *profile, const char *addr)
{
    puddle_stream rec;
    char err[300];
    CHECK(puddle_stream_init(&rec, id, st, updated) == 0);
    if (note) snprintf(rec.note, sizeof rec.note, "%s", note);
    if (profile) CHECK(puddle_people_set_profile(ps, id, PUDDLE_PEOPLE_DM, profile, strlen(profile), NULL, updated) == 0);
    if (addr) CHECK(puddle_stream_add_address(&rec, addr) == 0);
    CHECK(puddle_streams_put(ss, &rec, err, sizeof err) == 0);
    puddle_stream_free(&rec);
}

static int state_is(const char *id, puddle_stream_state want, puddle_owed owed)
{
    puddle_stream rec;
    if (puddle_streams_get(ss, id, &rec) != 0) return 0;
    int ok = rec.state == want && rec.owed == owed;
    puddle_stream_free(&rec);
    return ok;
}

static int member(const reply *r, const char *key, char *out, size_t cap)
{
    const char *b = body_of(r);
    puddle_span v;
    if (puddle_json_member(b, strlen(b), key, &v) != 0) return -1;
    if (v.len && v.p[0] == '"') return puddle_json_string(v, out, cap);
    if (v.len >= cap) return -1;
    memcpy(out, v.p, v.len);
    out[v.len] = 0;
    return 0;
}

static int count_of(const char *haystack, const char *needle)
{
    int n = 0;
    for (const char *p = haystack; (p = strstr(p, needle)); p += strlen(needle)) n++;
    return n;
}

int main(void)
{
    const char *base = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/puddle-ostreams-XXXXXX", base ? base : "/tmp");
    if (!mkdtemp(root)) return 2;
    char dir[700], phrase[PUDDLE_PHRASE_LEN + 1], err[300];
    snprintf(dir, sizeof dir, "%s/me", root);
    me = puddle_identity_create(dir, phrase, err, sizeof err);
    if (!me) { fprintf(stderr, "identity: %s\n", err); return 2; }
    puddle_identity_confirm(dir);
    puddle_identity_id(me, my_id);
    snprintf(dir, sizeof dir, "%s/data", root);
    ss = puddle_streams_open(dir, err, sizeof err);
    if (!ps) ps = puddle_people_open(dir, err, sizeof err);
    memset(&cfg, 0, sizeof cfg);
    snprintf(cfg.address, sizeof cfg.address, "::");
    cfg.port = 8443;
    ctx = (puddle_ctx){ .courier = &puddle_federation_courier, .config = &cfg, .identity = me, .streams = ss, .people = ps, .now_ms = now_cb };
    R = (puddle_router){ .ctx = &ctx };
    R.routes = puddle_all_routes(&R.count);

    char a[64], b[64], c[64], d[64], e[64], f[64];
    fake_id(1, a); fake_id(2, b); fake_id(3, c); fake_id(4, d); fake_id(5, e); fake_id(6, f);
    reply *r;
    char v[400], body[300];

    /* ---- nobody else gets anything ---- */
    {
        const char *get[] = { "/api/v1/streams", "/api/v1/invites", "/api/v1/streams/policy", "/api/v1/blocks", at("streams", a, NULL),
                              at("users", a, NULL), "/api/v1/users/me" };
        const char *post[] = { at("streams", a, "accept"), at("streams", a, "decline"), at("streams", a, "delete"), "/api/v1/invites/create" };
        const char *post2[] = { "/api/v1/invites/abcdefghijklmnopqrstuvwxy2/revoke", "/api/v1/streams/policy/set", at("users", a, "block"),
                                at("users", a, "unblock") };
        reply *nothing = call("GET", "/nope", NULL, 0);
        size_t nn = nothing->n;
        char keep[sizeof nothing->out];
        memcpy(keep, nothing->out, nn);
        for (size_t i = 0; i < sizeof get / sizeof *get; i++) {
            r = call("GET", get[i], NULL, 0);
            CHECK(r->status == 404 && r->n == nn && !memcmp(r->out, keep, nn));
        }
        for (size_t i = 0; i < 4; i++) {
            r = call("POST", post[i], "{}", 0);
            CHECK(r->status == 404 && r->n == nn && !memcmp(r->out, keep, nn));
            r = call("POST", post2[i], "{}", 0);
            CHECK(r->status == 404 && r->n == nn && !memcmp(r->out, keep, nn));
        }
        CHECK(call("POST", "/api/v1/streams", "{}", 1)->status == 405);          /* the owner is told it is a GET */
        CHECK(call("GET", at("streams", a, "accept"), NULL, 1)->status == 405);
    }

    /* ---- listing ---- */
    r = call("GET", "/api/v1/streams", NULL, 1);
    CHECK(r->status == 200 && !strcmp(body_of(r), "{\"items\":[],\"next\":null}"));

    put(a, PUDDLE_STREAM_PENDING_IN, T0 + 100, "hi, it's \"A\"\nfrom work", "{\"display_name\":\"Ann\",\"blurb\":\"x\"}", "203.0.113.7:8443");
    put(b, PUDDLE_STREAM_ACTIVE, T0 + 300, NULL, "{\"display_name\":\"Bo\"}", NULL);
    put(c, PUDDLE_STREAM_PENDING_OUT, T0 + 200, NULL, NULL, "[2001:db8::1]:8443");
    put(d, PUDDLE_STREAM_CLOSED, T0 + 50, NULL, NULL, NULL);
    puddle_assignments *as = NULL;
    {
        char e2[200];
        as = puddle_assignments_open(dir, e2, sizeof e2);
        ctx.assignments = as;
    }
    r = call("GET", "/api/v1/streams", NULL, 1);
    CHECK(r->status == 200 && valid_body(r));
    {
        const char *body_s = body_of(r);
        const char *pa = strstr(body_s, a), *pb = strstr(body_s, b), *pc = strstr(body_s, c), *pd = strstr(body_s, d);
        CHECK(pa && pb && pc && pd && pb < pc && pc < pa && pa < pd);          /* most recently changed first */
        CHECK(strstr(body_s, "\"message\":\"hi, it's \\\"A\\\"\\nfrom work\""));
        CHECK(strstr(body_s, "\"address\":\"203.0.113.7:8443\"") && strstr(body_s, "\"address\":\"[2001:db8::1]:8443\""));
        /* the person is a User: their profile, and the state of the stream with them */
        CHECK(strstr(body_s, "\"user\":{\"id\":\"") && strstr(body_s, "\"profile\":{\"display_name\":\"Ann\",\"blurb\":\"x\"},\"stream\":\"pending_in\"}"));
        CHECK(strstr(body_s, "\"state\":\"pending_in\"") && strstr(body_s, "\"state\":\"active\"") && strstr(body_s, "\"state\":\"pending_out\"") && strstr(body_s, "\"state\":\"closed\""));
        CHECK(strstr(body_s, "\"next\":null"));
    }
    r = call("GET", "/api/v1/streams?state=pending_in", NULL, 1);
    CHECK(r->status == 200 && count_of(body_of(r), "\"state\"") == 1 && strstr(body_of(r), a));
    r = call("GET", "/api/v1/streams?state=active", NULL, 1);
    CHECK(count_of(body_of(r), "\"state\"") == 1 && strstr(body_of(r), b));
    r = call("GET", "/api/v1/streams?limit=2", NULL, 1);
    CHECK(count_of(body_of(r), "\"state\"") == 2 && member(r, "next", v, sizeof v) == 0 && !strcmp(v, "2"));
    r = call("GET", "/api/v1/streams?limit=2&offset=2", NULL, 1);
    CHECK(count_of(body_of(r), "\"state\"") == 2 && member(r, "next", v, sizeof v) == 0 && !strcmp(v, "null"));
    r = call("GET", "/api/v1/streams?offset=9", NULL, 1);
    CHECK(r->status == 200 && !strcmp(body_of(r), "{\"items\":[],\"next\":null}"));
    r = call("GET", "/api/v1/streams?state=closed&limit=1", NULL, 1);
    CHECK(count_of(body_of(r), "\"state\"") == 1 && strstr(body_of(r), d));
    const char *bad_queries[] = { "?limit=0", "?limit=201", "?limit=abc", "?limit=-1", "?limit=", "?limit", "?state=bogus", "?x=1",
                                  "?limit=1&limit=2", "?limit=1&", "?&limit=1", "?state=a%20b", "?offset=99999999999", "?=1", "?limit=1&&offset=1" };
    for (size_t i = 0; i < sizeof bad_queries / sizeof *bad_queries; i++) {
        char t[100];
        snprintf(t, sizeof t, "/api/v1/streams%s", bad_queries[i]);
        if (status("GET", t, NULL) != 400) { fprintf(stderr, "FAIL %s was not refused\n", t); failures++; }
    }
    /* one stream on its own */
    r = call("GET", at("streams", a, NULL), NULL, 1);
    CHECK(r->status == 200 && member(r, "id", v, sizeof v) == 0 && !strcmp(v, a) && member(r, "state", v, sizeof v) == 0 && !strcmp(v, "pending_in"));
    CHECK(status("GET", at("streams", f, NULL), NULL) == 404);
    CHECK(status("GET", "/api/v1/streams/zzzz", NULL) == 404);                   /* not an id: no such path */
    /* the profile we show them */
    {
        char e2[200];
        char place[PUDDLE_PLACE_MAX];
        CHECK(puddle_place_dm(a, place) == 0 && puddle_assignments_set(as, place, "default", e2, sizeof e2) == 0);
        r = call("GET", "/api/v1/streams?state=pending_in", NULL, 1);
        CHECK(strstr(body_of(r), "\"profile\":\"default\""));
    }
    /* a person, as a User */
    r = call("GET", at("users", a, NULL), NULL, 1);
    CHECK(r->status == 200 && strncmp(body_of(r), "{\"id\":\"", 7) == 0);
    CHECK(strstr(body_of(r), "\"profile\":{\"display_name\":\"Ann\",\"blurb\":\"x\"}") && strstr(body_of(r), "\"stream\":\"pending_in\"") &&
          !strstr(body_of(r), "blocked"));
    CHECK(status("GET", at("users", f, NULL), NULL) == 404);                     /* nobody we know */
    r = call("GET", "/api/v1/users/me", NULL, 1);
    CHECK(r->status == 200 && member(r, "id", v, sizeof v) == 0 && !strcmp(v, my_id) && member(r, "link", v, sizeof v) == 0 && !strcmp(v, "null"));
    r = call("GET", at("users", my_id, NULL), NULL, 1);
    CHECK(r->status == 200 && member(r, "id", v, sizeof v) == 0 && !strcmp(v, my_id));

    /* ---- big entries: the page stops before the buffer does, and the next page carries on ---- */
    {
        static char about[8001];
        memset(about, 'x', 8000);
        about[8000] = 0;
        static char prof[9000], note[2001];
        snprintf(prof, sizeof prof, "{\"display_name\":\"Big\",\"about\":\"%s\"}", about);
        memset(note, 'n', 2000);
        note[2000] = 0;
        for (int i = 0; i < 12; i++) {
            char id[64];
            fake_id(100 + i, id);
            put(id, PUDDLE_STREAM_PENDING_IN, T0 + 1000 + i, note, prof, "10.0.0.1:1");
        }
        int seen = 0;
        size_t offset = 0;
        int pages = 0;
        for (;;) {
            char t[100];
            snprintf(t, sizeof t, "/api/v1/streams?state=pending_in&limit=200&offset=%zu", offset);
            r = call("GET", t, NULL, 1);
            CHECK(r->status == 200 && valid_body(r) && r->n < sizeof r->out - 1);
            seen += count_of(body_of(r), "\"state\"");
            pages++;
            if (member(r, "next", v, sizeof v) != 0 || !strcmp(v, "null")) break;
            offset = (size_t)atoi(v);
            if (pages > 20) { failures++; break; }
        }
        CHECK(seen == 13 && pages >= 2);                                       /* a, and the twelve big ones, each once */
        for (int i = 0; i < 12; i++) { char id[64]; fake_id(100 + i, id); put(id, PUDDLE_STREAM_CLOSED, T0, NULL, NULL, NULL); }
    }

    /* ---- accept ---- */
    clock_now = T0 + 5000;
    r = call("POST", at("streams", a, "accept"), NULL, 1);
    CHECK(r->status == 200 && valid_body(r) && member(r, "state", v, sizeof v) == 0 && !strcmp(v, "active"));
    CHECK(member(r, "updated_at", v, sizeof v) == 0 && atoll(v) == T0 + 5000);
    CHECK(state_is(a, PUDDLE_STREAM_ACTIVE, PUDDLE_OWED_APPROVED));              /* and the requester still has to be told */
    CHECK(status("POST", at("streams", a, "accept"), NULL) == 409);              /* not waiting any more */
    CHECK(status("POST", at("streams", c, "accept"), NULL) == 409);              /* we asked them: nothing to accept */
    CHECK(status("POST", at("streams", f, "accept"), NULL) == 404);
    CHECK(status("POST", "/api/v1/streams/../../x/accept", NULL) == 404);        /* not an id: no such path */
    CHECK(status("POST", "/api/v1/streams/zzzz/accept", NULL) == 404);

    /* ---- decline ---- */
    put(e, PUDDLE_STREAM_PENDING_IN, T0, "please", NULL, "10.0.0.2:1");
    r = call("POST", at("streams", e, "decline"), NULL, 1);
    CHECK(r->status == 200 && valid_body(r) && member(r, "state", v, sizeof v) == 0 && !strcmp(v, "closed"));
    CHECK(state_is(e, PUDDLE_STREAM_CLOSED, PUDDLE_OWED_DECLINED));
    CHECK(status("POST", at("streams", e, "decline"), NULL) == 409);             /* once */
    CHECK(status("POST", at("streams", b, "decline"), NULL) == 409);             /* an active stream is deleted, not declined */
    CHECK(status("POST", at("streams", f, "decline"), NULL) == 404);

    /* ---- delete ---- */
    clock_now = T0 + 6000;
    r = call("POST", at("streams", b, "delete"), NULL, 1);
    CHECK(r->status == 200 && member(r, "state", v, sizeof v) == 0 && !strcmp(v, "closed"));
    CHECK(state_is(b, PUDDLE_STREAM_CLOSED, PUDDLE_OWED_CLOSED));
    CHECK(status("POST", at("streams", b, "delete"), NULL) == 409);              /* already closed */
    CHECK(status("POST", at("streams", c, "delete"), NULL) == 200);              /* withdraw a request we made */
    CHECK(state_is(c, PUDDLE_STREAM_CLOSED, PUDDLE_OWED_CLOSED));
    put(f, PUDDLE_STREAM_PENDING_IN, T0, NULL, NULL, NULL);
    CHECK(status("POST", at("streams", f, "delete"), NULL) == 409);              /* a request waiting on us: decline it */
    {
        char z[64];
        fake_id(900, z);
        CHECK(status("POST", at("streams", z, "delete"), NULL) == 404);
    }

    /* ---- blocks ---- */
    CHECK(!strcmp(body_of(call("GET", "/api/v1/blocks", NULL, 1)), "{\"items\":[],\"next\":null}"));
    clock_now = T0 + 7000;
    r = call("POST", at("users", a, "block"), "{\"note\":\"ignored\"}", 1);
    CHECK(r->status == 200 && member(r, "id", v, sizeof v) == 0 && !strcmp(v, a) && strstr(body_of(r), "\"blocked\":true") &&
          strstr(body_of(r), "\"stream\":\"closed\""));
    CHECK(state_is(a, PUDDLE_STREAM_CLOSED, PUDDLE_OWED_NONE));                   /* closed, and nothing owed: a block is silent */
    clock_now = T0 + 8000;
    CHECK(status("POST", at("users", a, "block"), NULL) == 200);
    {
        char z[64];
        fake_id(901, z);
        r = call("POST", at("users", z, "block"), NULL, 1);                      /* someone we have nothing with */
        CHECK(r->status == 200 && strstr(body_of(r), "\"blocked\":true"));
        CHECK(status("POST", at("users", z, "unblock"), NULL) == 200);
        CHECK(status("GET", at("users", z, NULL), NULL) == 404);                 /* and now nobody we know */
    }
    CHECK(status("POST", at("users", f, "block"), NULL) == 200);
    CHECK(status("POST", at("users", my_id, "block"), NULL) == 400);              /* not ourselves */
    CHECK(status("POST", "/api/v1/users/abc/block", NULL) == 404);
    r = call("GET", "/api/v1/blocks", NULL, 1);
    CHECK(r->status == 200 && valid_body(r) && count_of(body_of(r), "\"user\"") == 2 && strstr(body_of(r), a) < strstr(body_of(r), f));
    {
        char want[200];
        snprintf(want, sizeof want, "\"display_name\":\"Ann\"},\"created_at\":%lld", (long long)(T0 + 7000)); /* the first time stands */
        CHECK(strstr(body_of(r), want) != NULL);
    }
    r = call("GET", "/api/v1/blocks?limit=1", NULL, 1);
    CHECK(count_of(body_of(r), "\"user\"") == 1 && member(r, "next", v, sizeof v) == 0 && !strcmp(v, "1"));
    r = call("GET", "/api/v1/blocks?limit=1&offset=1", NULL, 1);
    CHECK(count_of(body_of(r), "\"user\"") == 1 && strstr(body_of(r), f) && member(r, "next", v, sizeof v) == 0 && !strcmp(v, "null"));
    CHECK(status("GET", "/api/v1/blocks?state=x", NULL) == 400);
    r = call("POST", at("users", a, "unblock"), NULL, 1);
    CHECK(r->status == 200 && !strstr(body_of(r), "blocked"));
    CHECK(status("POST", at("users", a, "unblock"), NULL) == 404);
    CHECK(!puddle_streams_is_blocked(ss, a) && puddle_streams_is_blocked(ss, f));

    /* ---- a peer whose certificate changed ---- */
    {
        const char *fp = "4444444444444444444444444444444444444444444444444444444444444444";
        const char *fp2 = "5555555555555555555555555555555555555555555555555555555555555555";
        char g[64];
        fake_id(7, g);
        put(g, PUDDLE_STREAM_ACTIVE, T0 + 50000, NULL, NULL, "10.0.0.7:1");
        CHECK(puddle_streams_check_fingerprint(ss, g, fp) == 1);
        CHECK(status("POST", at("streams", g, "trust"), NULL) == 409);              /* nothing to decide */
        r = call("GET", "/api/v1/streams?state=active", NULL, 1);
        CHECK(!strstr(body_of(r), "certificate_changed"));
        CHECK(puddle_streams_check_fingerprint(ss, g, fp2) == 0);
        r = call("GET", "/api/v1/streams?state=active", NULL, 1);
        CHECK(strstr(body_of(r), "\"certificate_changed\":true"));                  /* the owner is told */
        r = call("POST", at("streams", g, "trust"), NULL, 1);
        CHECK(r->status == 200 && valid_body(r) && !strstr(body_of(r), "certificate_changed"));
        CHECK(puddle_streams_check_fingerprint(ss, g, fp2) == 1);                    /* the new one is now the one */
        CHECK(status("POST", at("streams", g, "trust"), NULL) == 409);
        CHECK(call("POST", at("streams", g, "trust"), NULL, 0)->status == 404);
        CHECK(status("POST", at("streams", f, "trust"), NULL) == 409 || status("POST", at("streams", f, "trust"), NULL) == 404);
        put(g, PUDDLE_STREAM_CLOSED, T0, NULL, NULL, NULL);
    }

    /* ---- the policy ---- */
    r = call("GET", "/api/v1/streams/policy", NULL, 1);
    CHECK(r->status == 200 && !strcmp(body_of(r), "{\"access\":\"manual\"}"));
    r = call("POST", "/api/v1/streams/policy/set", "{\"access\":\"open\"}", 1);
    CHECK(r->status == 200 && !strcmp(body_of(r), "{\"access\":\"open\"}") && puddle_streams_access(ss) == PUDDLE_ACCESS_OPEN);
    CHECK(status("POST", "/api/v1/streams/policy/set", "{\"access\":\"closed\"}") == 200 && puddle_streams_access(ss) == PUDDLE_ACCESS_CLOSED);
    CHECK(status("POST", "/api/v1/streams/policy/set", "{\"access\":\"Open\"}") == 400 && puddle_streams_access(ss) == PUDDLE_ACCESS_CLOSED);
    CHECK(status("POST", "/api/v1/streams/policy/set", "{}") == 400 && status("POST", "/api/v1/streams/policy/set", "{\"access\":1}") == 400);
    CHECK(status("POST", "/api/v1/streams/policy/set", "{\"access\":\"manual\"}") == 200);

    /* ---- invites ---- */
    r = call("POST", "/api/v1/invites/create", "", 1);
    CHECK(r->status == 409 && strstr(body_of(r), "no_address") && strstr(body_of(r), "advertise"));  /* nowhere to point the link */
    CHECK(puddle_streams_invites(ss, clock_now, NULL, 0) == 0);                   /* and none was made */
    snprintf(cfg.advertise, sizeof cfg.advertise, "203.0.113.7:8443");
    clock_now = T0 + 10000;
    r = call("POST", "/api/v1/invites/create", "", 1);
    CHECK(r->status == 201 && valid_body(r));
    char token[64], link[300];
    CHECK(member(r, "token", token, sizeof token) == 0 && puddle_invite_token_valid(token));
    CHECK(member(r, "link", link, sizeof link) == 0);
    {
        puddle_link l;
        CHECK(puddle_link_parse(link, &l, err, sizeof err) == 0 && !strcmp(l.invite, token) &&
              !strcmp(l.address.address, "203.0.113.7:8443"));
    }
    CHECK(member(r, "uses", v, sizeof v) == 0 && !strcmp(v, "1"));
    CHECK(member(r, "expires_at", v, sizeof v) == 0 && atoll(v) == T0 + 10000 + 604800000LL);
    r = call("POST", "/api/v1/invites/create", "{\"uses\":5,\"expires_in\":3600}", 1);
    CHECK(r->status == 201 && member(r, "uses", v, sizeof v) == 0 && !strcmp(v, "5") && member(r, "expires_at", v, sizeof v) == 0 && atoll(v) == T0 + 10000 + 3600000LL);
    const char *bad_invites[] = { "{\"uses\":0}", "{\"uses\":1001}", "{\"uses\":\"2\"}", "{\"uses\":1.5}", "{\"expires_in\":59}",
                                  "{\"expires_in\":2592001}", "{\"expires_in\":\"x\"}", "nonsense", "[1]" };
    for (size_t i = 0; i < sizeof bad_invites / sizeof *bad_invites; i++)
        if (status("POST", "/api/v1/invites/create", bad_invites[i]) != 400) { fprintf(stderr, "FAIL invite %s accepted\n", bad_invites[i]); failures++; }
    CHECK(status("POST", "/api/v1/invites/create", "{\"uses\":1000,\"expires_in\":2592000}") == 201);
    r = call("GET", "/api/v1/invites", NULL, 1);
    CHECK(r->status == 200 && valid_body(r) && count_of(body_of(r), "\"token\"") == 3 && strstr(body_of(r), token) && strstr(body_of(r), "\"next\":null"));
    snprintf(body, sizeof body, "/api/v1/invites/%s/revoke", token);
    r = call("POST", body, NULL, 1);
    CHECK(r->status == 200 && member(r, "token", v, sizeof v) == 0 && !strcmp(v, token) && member(r, "uses", v, sizeof v) == 0 && !strcmp(v, "0"));
    CHECK(status("POST", body, NULL) == 404);
    CHECK(status("POST", "/api/v1/invites/junk/revoke", NULL) == 404);
    CHECK(status("POST", "/api/v1/invites/a.b/revoke", NULL) == 404);
    CHECK(count_of(body_of(call("GET", "/api/v1/invites", NULL, 1)), "\"token\"") == 2);
    /* a token made here is one a stranger's request can spend */
    {
        puddle_invite live[PUDDLE_MAX_INVITES];
        size_t n = puddle_streams_invites(ss, clock_now, live, PUDDLE_MAX_INVITES);
        CHECK(n == 2 && puddle_streams_invite_consume(ss, live[0].token, clock_now) == 1);
    }

    puddle_assignments_close(as);
    puddle_people_close(ps);
    puddle_streams_close(ss);
    puddle_identity_close(me);
    nftw(root, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    return failures ? 1 : 0;
}
