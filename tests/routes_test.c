#include "http/router.h"

#include "core/phrase.h"
#include "service/routes.h"
#include "api/api.h"

#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static char root[512];
static int64_t clock_now = 1760000000000LL;
static int64_t now_cb(void) { return clock_now; }
static int rm_cb(const char *p, const struct stat *sb, int t, struct FTW *f) { (void)sb; (void)t; (void)f; remove(p); return 0; }

typedef struct { int status; char out[40000]; size_t n; } reply;

static reply call(const puddle_router *r, const char *method, const char *path, const char *body, int owner)
{
    puddle_request req;
    memset(&req, 0, sizeof req);
    snprintf(req.method, sizeof req.method, "%s", method);
    snprintf(req.target, sizeof req.target, "%s", path);
    snprintf(req.path, sizeof req.path, "%s", path);
    size_t bl = body ? strlen(body) : 0;
    req.content_length = bl;
    unsigned char *copy = malloc(bl + 1);
    if (bl) memcpy(copy, body, bl);
    copy[bl] = 0;
    static reply rp; /* big: keep it off the stack */
    memset(&rp, 0, sizeof rp);
    rp.n = puddle_router_handle(r, &req, owner, copy, bl, rp.out, sizeof rp.out - 1, &rp.status);
    free(copy);
    return rp;
}

static const char *body_of(const reply *r)
{
    const char *b = strstr(r->out, "\r\n\r\n");
    return b ? b + 4 : "";
}

/* a member of the JSON answer, as text */
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

int main(void)
{
    const char *base = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/puddle-routes-XXXXXX", base ? base : "/tmp");
    if (!mkdtemp(root)) return 2;

    static char about[2000 * 4 + 1];
    for (int i = 0; i < 2000; i++) memcpy(about + i * 4, "\xF0\x9F\x99\x82", 4); /* the longest an 'about' can be */
    char yaml[9000];
    snprintf(yaml, sizeof yaml,
             "profiles:\n- id: main\n  display_name: \"Zo\xC3\xAB \\\"Z\\\" Q\"\n  blurb: \"a \\\"quoted\\\" line\"\n  pronouns: they/them\n"
             "  color:\n    gradient: [\"#112233\", \"#445566\"]\n    angle: 135\n  about: \"%s\"\n", about);
    char path[700], err[300], phrase[PUDDLE_PHRASE_LEN + 1];
    snprintf(path, sizeof path, "%s/p.yaml", root);
    FILE *f = fopen(path, "wb");
    fputs(yaml, f);
    fclose(f);

    puddle_config cfg;
    memset(&cfg, 0, sizeof cfg);
    snprintf(cfg.dir, sizeof cfg.dir, "%s", root);
    snprintf(cfg.data_dir, sizeof cfg.data_dir, "%s/data", root);
    snprintf(cfg.address, sizeof cfg.address, "127.0.0.1");
    cfg.port = 8443;
    snprintf(cfg.profile_files[cfg.nprofile_files++], PUDDLE_PATH_MAX, "%s", path);

    puddle_identity *id = puddle_identity_create(cfg.data_dir, phrase, err, sizeof err);
    if (!id) { fprintf(stderr, "identity: %s\n", err); return 2; }
    puddle_identity_confirm(cfg.data_dir);
    puddle_profiles *profiles;
    char old_default[PUDDLE_PROFILE_ID_MAX + 1];
    if (!(profiles = puddle_profiles_open(&cfg, old_default, err, sizeof err))) { fprintf(stderr, "profiles: %s\n", err); return 2; }

    puddle_ctx ctx = { .config = &cfg, .identity = id, .profiles = profiles, .roll_slot = puddle_roll_slot_new(),
                       .replay = puddle_replay_new(64), .now_ms = now_cb };
    puddle_router R = { NULL, 0, &ctx };
    R.routes = puddle_all_routes(&R.count);
    reply r;
    char v[300], uid[PUDDLE_USER_ID_LEN + 1];
    puddle_identity_id(id, uid);

    /* ---- GET /api/v1/identity ---- */
    r = call(&R, "GET", "/api/v1/identity", NULL, 1);
    CHECK(r.status == 200);
    CHECK(member(&r, "id", v, sizeof v) == 0 && strcmp(v, uid) == 0);
    char want[200];
    snprintf(want, sizeof want, "127.0.0.1:8443");
    CHECK(member(&r, "link", v, sizeof v) == 0 && strcmp(v, want) == 0);
    CHECK(member(&r, "seq", v, sizeof v) == 0 && strcmp(v, "0") == 0);
    char kid0[300];
    CHECK(member(&r, "key_id", kid0, sizeof kid0) == 0 && strlen(kid0) == 64);

    /* where the link points */
    {
        puddle_config c2 = cfg;
        puddle_ctx x = ctx;
        x.config = &c2;
        puddle_router R2 = { puddle_routes, puddle_routes_count, &x };
        snprintf(c2.advertise, sizeof c2.advertise, "home.example.org:9000");
        r = call(&R2, "GET", "/api/v1/identity", NULL, 1);
        snprintf(want, sizeof want, "home.example.org:9000");
        CHECK(member(&r, "link", v, sizeof v) == 0 && strcmp(v, want) == 0);
        c2.advertise[0] = 0;
        snprintf(c2.address, sizeof c2.address, "::1");
        r = call(&R2, "GET", "/api/v1/identity", NULL, 1);
        snprintf(want, sizeof want, "[::1]:8443");
        CHECK(member(&r, "link", v, sizeof v) == 0 && strcmp(v, want) == 0);
        snprintf(c2.address, sizeof c2.address, "::");
        r = call(&R2, "GET", "/api/v1/identity", NULL, 1);
        CHECK(member(&r, "link", v, sizeof v) == 0 && strcmp(v, "null") == 0); /* every interface says nothing about our address */
        snprintf(c2.address, sizeof c2.address, "0.0.0.0");
        r = call(&R2, "GET", "/api/v1/identity", NULL, 1);
        CHECK(member(&r, "link", v, sizeof v) == 0 && strcmp(v, "null") == 0);
        snprintf(c2.advertise, sizeof c2.advertise, "[2001:db8::1]:8443");
        r = call(&R2, "GET", "/api/v1/identity", NULL, 1);
        CHECK(strstr(body_of(&r), "\"link\":\"[2001:db8::1]:8443\""));
    }

    /* ---- GET /api/v1/users/me ---- */
    r = call(&R, "GET", "/api/v1/users/me", NULL, 1);
    CHECK(r.status == 200 && puddle_json_valid_object(body_of(&r), strlen(body_of(&r))));
    CHECK(member(&r, "id", v, sizeof v) == 0 && strcmp(v, uid) == 0);
    {
        const char *b = body_of(&r);
        puddle_span p;
        CHECK(puddle_json_member(b, strlen(b), "profile", &p) == 0);
        char prof[20000];
        CHECK(p.len < sizeof prof);
        memcpy(prof, p.p, p.len); prof[p.len] = 0;
        puddle_span x;
        char s[300];
        CHECK(puddle_json_member(prof, p.len, "id", &x) == 0 && puddle_json_string(x, s, sizeof s) == 0 && !strcmp(s, "main"));
        CHECK(puddle_json_member(prof, p.len, "places", &x) == 0 && x.len == 2);                     /* how the owner is shown to themselves: shown nowhere yet */
        CHECK(puddle_json_member(prof, p.len, "display_name", &x) == 0 && puddle_json_string(x, s, sizeof s) == 0 &&
              !strcmp(s, "Zo\xC3\xAB \"Z\" Q"));
        CHECK(puddle_json_member(prof, p.len, "blurb", &x) == 0 && puddle_json_string(x, s, sizeof s) == 0 && !strcmp(s, "a \"quoted\" line"));
        CHECK(puddle_json_member(prof, p.len, "pronouns", &x) == 0);
        CHECK(puddle_json_member(prof, p.len, "color", &x) == 0 && memchr(x.p, '#', x.len) && memmem(x.p, x.len, "\"angle\":135", 11));
        CHECK(puddle_json_member(prof, p.len, "avatar", &x) == 1); /* none set: left out, not null */
        static char abig[9000];
        CHECK(puddle_json_member(prof, p.len, "about", &x) == 0 && puddle_json_string(x, abig, sizeof abig) == 0 &&
              strlen(abig) == 8000 && !strcmp(abig, about));
        puddle_span lk;
        CHECK(puddle_json_member(b, strlen(b), "link", &lk) == 0 && lk.p[0] == '"');
    }

    /* ---- strangers ---- */
    {
        const char *paths[] = { "/api/v1/identity", "/api/v1/users/me", "/api/v1/identity/roll", "/api/v1/identity/roll/confirm",
                                "/api/v1/identity/roll/cancel", "/nothing" };
        reply nothing = call(&R, "GET", "/nothing", NULL, 0);
        for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++)
            for (int m = 0; m < 2; m++) {
                r = call(&R, m ? "POST" : "GET", paths[i], NULL, 0);
                CHECK(r.status == 404 && r.n == nothing.n && memcmp(r.out, nothing.out, r.n) == 0);
            }
        CHECK(call(&R, "GET", "/api/v1/identity/roll", NULL, 1).status == 405); /* the owner is told it's a POST */
        CHECK(call(&R, "POST", "/api/v1/users/me", NULL, 1).status == 405);
        puddle_request q;
        memset(&q, 0, sizeof q);
        strcpy(q.method, "POST"); strcpy(q.path, "/api/v1/identity/roll");
        CHECK(puddle_router_body_limit(&R, &q, 1) == 512 && puddle_router_body_limit(&R, &q, 0) == 0);
    }

    /* ---- rolling the keys, in two steps ---- */
    char json[400];
    snprintf(json, sizeof json, "{\"phrase\":\"%s\"}", phrase);
    CHECK(call(&R, "POST", "/api/v1/identity/roll/confirm", NULL, 1).status == 409); /* nothing started */
    CHECK(call(&R, "POST", "/api/v1/identity/roll/confirm", "{}", 1).status == 409);

    r = call(&R, "POST", "/api/v1/identity/roll", "{\"phrase\":\"AAAA-AAAA-AAAA-AAAA-AAAA-AAAA-AAAA-AAAA\"}", 1);
    CHECK(r.status == 400 && member(&r, "error", v, sizeof v) == 0 && !strcmp(v, "bad_phrase") && member(&r, "message", v, sizeof v) == 0 && v[0]);
    r = call(&R, "POST", "/api/v1/identity/roll", "{\"phrase\":\"nonsense\"}", 1);
    CHECK(r.status == 400 && strstr(body_of(&r), "bad_phrase"));
    CHECK(call(&R, "POST", "/api/v1/identity/roll", "{}", 1).status == 400);
    CHECK(call(&R, "POST", "/api/v1/identity/roll", "", 1).status == 400);
    CHECK(call(&R, "POST", "/api/v1/identity/roll", "not json", 1).status == 400);
    CHECK(call(&R, "POST", "/api/v1/identity/roll", "{\"phrase\":7}", 1).status == 400);
    CHECK(call(&R, "POST", "/api/v1/identity/roll", "{\"phrase\":\"x\",\"phrase\":\"y\"}", 1).status == 400); /* ambiguous */
    CHECK(call(&R, "POST", "/api/v1/identity/roll/confirm", NULL, 1).status == 409); /* failures start nothing */

    r = call(&R, "POST", "/api/v1/identity/roll", json, 1);
    char fresh[100], fresh2[100];
    CHECK(r.status == 200 && member(&r, "new_phrase", fresh, sizeof fresh) == 0 && strlen(fresh) == PUDDLE_PHRASE_LEN);
    CHECK(strcmp(fresh, phrase) != 0 && member(&r, "expires_in", v, sizeof v) == 0 && !strcmp(v, "600"));
    CHECK(strstr(r.out, "Cache-Control: no-store") && !strstr(r.out, phrase)); /* the old phrase is never echoed */
    /* nothing has changed yet: the old phrase still rules */
    r = call(&R, "GET", "/api/v1/identity", NULL, 1);
    CHECK(member(&r, "seq", v, sizeof v) == 0 && !strcmp(v, "0") && member(&r, "key_id", v, sizeof v) == 0 && !strcmp(v, kid0));
    /* starting again replaces the unfinished roll, and gives a different new phrase */
    r = call(&R, "POST", "/api/v1/identity/roll", json, 1);
    CHECK(r.status == 200 && member(&r, "new_phrase", fresh2, sizeof fresh2) == 0 && strcmp(fresh, fresh2) != 0);
    /* cancel */
    r = call(&R, "POST", "/api/v1/identity/roll/cancel", NULL, 1);
    CHECK(r.status == 200 && strstr(body_of(&r), "\"cancelled\":true"));
    r = call(&R, "POST", "/api/v1/identity/roll/cancel", NULL, 1);
    CHECK(r.status == 200 && strstr(body_of(&r), "\"cancelled\":false"));
    CHECK(call(&R, "POST", "/api/v1/identity/roll/confirm", NULL, 1).status == 409);
    /* a roll left waiting too long is dropped: the phrase shown then may not have been written down */
    CHECK(call(&R, "POST", "/api/v1/identity/roll", json, 1).status == 200);
    clock_now += 10 * 60 * 1000 + 1;
    CHECK(call(&R, "POST", "/api/v1/identity/roll/confirm", NULL, 1).status == 409);
    r = call(&R, "GET", "/api/v1/identity", NULL, 1);
    CHECK(member(&r, "seq", v, sizeof v) == 0 && !strcmp(v, "0"));
    /* just inside the time limit it works */
    r = call(&R, "POST", "/api/v1/identity/roll", json, 1);
    CHECK(r.status == 200 && member(&r, "new_phrase", fresh, sizeof fresh) == 0);
    clock_now += 10 * 60 * 1000;
    r = call(&R, "POST", "/api/v1/identity/roll/confirm", NULL, 1);
    CHECK(r.status == 200);
    CHECK(member(&r, "seq", v, sizeof v) == 0 && !strcmp(v, "1"));
    CHECK(member(&r, "id", v, sizeof v) == 0 && !strcmp(v, uid)); /* the id never changes */
    char kid1[300];
    CHECK(member(&r, "key_id", kid1, sizeof kid1) == 0 && strcmp(kid1, kid0) != 0);
    CHECK(call(&R, "POST", "/api/v1/identity/roll/confirm", NULL, 1).status == 409); /* it is consumed */
    {
        size_t cl;
        char *chain = puddle_identity_chain(id, &cl);
        puddle_chain_head h;
        CHECK(chain && puddle_chain_verify(chain, cl, uid, &h, err, sizeof err) == 0 && h.seq == 1);
        free(chain);
    }
    /* the old phrase has been spent; the new one works */
    r = call(&R, "POST", "/api/v1/identity/roll", json, 1);
    CHECK(r.status == 400 && strstr(body_of(&r), "bad_phrase"));
    snprintf(json, sizeof json, "{\"phrase\":\"%s\"}", fresh);
    r = call(&R, "POST", "/api/v1/identity/roll", json, 1);
    CHECK(r.status == 200);
    CHECK(call(&R, "POST", "/api/v1/identity/roll/confirm", "{}", 1).status == 200);
    r = call(&R, "GET", "/api/v1/identity", NULL, 1);
    CHECK(member(&r, "seq", v, sizeof v) == 0 && !strcmp(v, "2"));

    /* the phrase typed in is not echoed back, and a failed attempt leaves nothing behind */
    snprintf(json, sizeof json, "{\"phrase\":\"%s\"}", phrase);
    r = call(&R, "POST", "/api/v1/identity/roll", json, 1);
    CHECK(r.status == 400 && !strstr(r.out, phrase));

    puddle_roll_slot_free(ctx.roll_slot);
    puddle_replay_free(ctx.replay);
    puddle_profiles_close(profiles);
    puddle_identity_close(id);
    nftw(root, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    return failures ? 1 : 0;
}
