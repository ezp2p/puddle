#include "core/group.h"

#include "base/encoding.h"
#include "events/group_events.h"

#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define T0 1760000000000LL
#define A "oaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define B "baaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define C "caaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define D "daaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define NONCE "0123456789abcdef0123456789abcdef"

static char root[512];
static int rm_cb(const char *p, const struct stat *sb, int t, struct FTW *f) { (void)sb; (void)t; (void)f; remove(p); return 0; }

static int parse(const char *ev, puddle_gevent *g, const char **why) { return puddle_group_event_parse(ev, strlen(ev), g, why); }

/* a group A made and invited B and C into, with B and C in */
static void make_group(puddle_group *g)
{
    char chat[PUDDLE_USER_ID_LEN + 1], ev[2000];
    const char *why;
    puddle_gevent e;
    CHECK(puddle_group_create_build(A, T0, NONCE, "Friends", chat, ev, sizeof ev) > 0);
    CHECK(parse(ev, &e, &why) == 0);
    CHECK(puddle_group_init_created(g, &e, T0) == 0);
    const char *who[] = { B, C };
    for (int i = 0; i < 2; i++) {
        CHECK(puddle_group_invite_build(chat, A, T0 + 10 + i, who[i], ev, sizeof ev) > 0 && parse(ev, &e, &why) == 0);
        CHECK(puddle_group_apply(g, &e, &why) == 0);
        CHECK(puddle_group_join_build(chat, who[i], T0 + 100 + i, ev, sizeof ev) > 0 && parse(ev, &e, &why) == 0);
        CHECK(puddle_group_apply(g, &e, &why) == 0);
    }
}

static int rename_fn(void *user, puddle_group *g, int exists)
{
    const char *mode = user;
    if (!exists)
        return 7;
    if (!strcmp(mode, "fail"))
        return -1;
    if (!strcmp(mode, "skip"))
        return 5;
    snprintf(g->name, sizeof g->name, "%s", mode);
    return 0;
}

static int apply_text(puddle_group *g, const char *ev, const char **why)
{
    puddle_gevent e;
    const char *pw;
    if (parse(ev, &e, &pw) != 0) { *why = pw; return -2; }
    return puddle_group_apply(g, &e, why);
}

int main(void)
{
    const char *base = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/puddle-group-XXXXXX", base ? base : "/tmp");
    if (!mkdtemp(root)) return 2;
    char err[300], chat[PUDDLE_USER_ID_LEN + 1], ev[3000];
    const char *why;
    puddle_gevent g;

    /* ---- the id ---- */
    {
        char a[53], b[53], n2[33];
        CHECK(puddle_group_chat_id(A, T0, NONCE, a) == 0 && puddle_id_valid(a) && strlen(a) == 52);
        CHECK(puddle_group_chat_id(A, T0, NONCE, b) == 0 && !strcmp(a, b));
        CHECK(puddle_group_chat_id(B, T0, NONCE, b) == 0 && strcmp(a, b) != 0);        /* each part counts */
        CHECK(puddle_group_chat_id(A, T0 + 1, NONCE, b) == 0 && strcmp(a, b) != 0);
        CHECK(puddle_group_chat_id(A, T0, "fedcba9876543210fedcba9876543210", b) == 0 && strcmp(a, b) != 0);
        CHECK(puddle_group_chat_id("short", T0, NONCE, b) != 0 && puddle_group_chat_id(A, T0, "nothex", b) != 0 && puddle_group_chat_id(A, 5, NONCE, b) != 0);
        CHECK(puddle_group_nonce(n2) == 0 && strlen(n2) == 32 && puddle_group_chat_id(A, T0, n2, b) == 0);
    }

    /* ---- names ---- */
    CHECK(puddle_group_name_ok("Friends") && puddle_group_name_ok("Caf\xC3\xA9 \xE2\x98\x95"));
    CHECK(!puddle_group_name_ok("") && !puddle_group_name_ok(NULL) && !puddle_group_name_ok(" padded") && !puddle_group_name_ok("two\nlines") &&
          !puddle_group_name_ok("tab\there") && !puddle_group_name_ok("bell\x07"));
    {
        char long_name[500];
        memset(long_name, 'n', 100); long_name[100] = 0;
        CHECK(puddle_group_name_ok(long_name));
        long_name[100] = 'n'; long_name[101] = 0;
        CHECK(!puddle_group_name_ok(long_name));
    }

    /* ---- building and reading events ---- */
    CHECK(puddle_group_create_build(A, T0, NONCE, "Friends", chat, ev, sizeof ev) > 0);
    CHECK(parse(ev, &g, &why) == 0 && g.type == PUDDLE_GE_CREATE && !strcmp(g.chat, chat) && !strcmp(g.sender, A) && g.ts == T0 && !strcmp(g.name, "Friends") &&
          !strcmp(g.nonce, NONCE));
    CHECK(puddle_group_create_build(A, T0, NONCE, "", chat, ev, sizeof ev) == 0);
    CHECK(puddle_group_create_build(A, T0, NONCE, "Friends", chat, ev, 20) == 0);         /* does not fit */
    {
        /* a create whose id is not the one its parts make is refused */
        char forged[600];
        snprintf(forged, sizeof forged, "{\"type\":\"chat.create\",\"chat\":\"%s\",\"sender\":\"%s\",\"ts\":%lld,\"name\":\"X\",\"nonce\":\"%s\"}", B, A, (long long)T0, NONCE);
        CHECK(parse(forged, &g, &why) == -1 && !strcmp(why, "bad_group_id"));
        snprintf(forged, sizeof forged, "{\"type\":\"chat.create\",\"chat\":\"%s\",\"sender\":\"%s\",\"ts\":%lld,\"name\":\"X\"}", chat, A, (long long)T0);
        CHECK(parse(forged, &g, &why) == -1);                                              /* no nonce */
    }
    CHECK(puddle_group_rename_build(chat, B, T0 + 5, "New name", ev, sizeof ev) > 0 && parse(ev, &g, &why) == 0 && g.type == PUDDLE_GE_RENAME &&
          !strcmp(g.name, "New name") && !strcmp(g.sender, B));
    CHECK(puddle_group_rename_build(chat, B, T0, "two\nlines", ev, sizeof ev) == 0);
    CHECK(puddle_group_invite_build(chat, A, T0 + 5, C, ev, sizeof ev) > 0 && parse(ev, &g, &why) == 0 && g.type == PUDDLE_GE_INVITE && !strcmp(g.member, C));
    CHECK(puddle_group_invite_build(chat, A, T0, A, ev, sizeof ev) > 0 && parse(ev, &g, &why) == -1);   /* nobody invites themselves */
    CHECK(puddle_group_invite_build(chat, A, T0, "nope", ev, sizeof ev) == 0);
    CHECK(puddle_group_join_build(chat, C, T0 + 9, ev, sizeof ev) > 0 && parse(ev, &g, &why) == 0 && g.type == PUDDLE_GE_JOIN);
    CHECK(puddle_group_leave_build(chat, C, T0 + 9, ev, sizeof ev) > 0 && parse(ev, &g, &why) == 0 && g.type == PUDDLE_GE_LEAVE);
    {
        /* anything unexpected is refused: extra members, wrong types, no ts, silly times, a ref */
        const char *bad[] = {
            "{\"type\":\"member.join\",\"chat\":\"" A "\",\"sender\":\"" B "\",\"ts\":1760000000000,\"member\":\"" C "\"}",
            "{\"type\":\"member.join\",\"chat\":\"" A "\",\"sender\":\"" B "\",\"ts\":1760000000000,\"body\":\"hi\"}",
            "{\"type\":\"member.join\",\"chat\":\"" A "\",\"sender\":\"" B "\",\"ts\":1760000000000,\"ref\":\"x\"}",
            "{\"type\":\"member.leave\",\"chat\":\"" A "\",\"sender\":\"" B "\"}",
            "{\"type\":\"member.leave\",\"chat\":\"nope\",\"sender\":\"" B "\",\"ts\":1760000000000}",
            "{\"type\":\"member.invite\",\"chat\":\"" A "\",\"sender\":\"" B "\",\"ts\":1760000000000}",
            "{\"type\":\"member.invite\",\"chat\":\"" A "\",\"sender\":\"" B "\",\"ts\":1760000000000,\"member\":\"x\"}",
            "{\"type\":\"chat.rename\",\"chat\":\"" A "\",\"sender\":\"" B "\",\"ts\":1760000000000}",
            "{\"type\":\"chat.rename\",\"chat\":\"" A "\",\"sender\":\"" B "\",\"ts\":1760000000000,\"body\":\"\"}",
            "{\"type\":\"member.join\",\"chat\":\"" A "\",\"sender\":\"" B "\",\"ts\":5}",
            "{\"type\":\"member.join\",\"chat\":\"" A "\",\"sender\":\"" B "\",\"ts\":99999999999999}",
            "not json", "{}", "{\"type\":5}",
        };
        for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
            if (parse(bad[i], &g, &why) == 0) { fprintf(stderr, "FAIL accepted: %s\n", bad[i]); failures++; }
        CHECK(parse("{\"type\":\"message\",\"chat\":\"" A "\",\"sender\":\"" B "\",\"ts\":1760000000000,\"body\":\"hi\"}", &g, &why) == -1 && !strcmp(why, "unsupported_type"));
        CHECK(parse("{\"type\":\"a.very.long.type.name.indeed\",\"chat\":\"x\"}", &g, &why) == -1 && !strcmp(why, "unsupported_type"));
    }

    /* ---- the rules ---- */
    puddle_group grp;
    make_group(&grp);
    CHECK(puddle_group_count(&grp, PUDDLE_GM_MEMBER) == 3 && puddle_group_member(&grp, B)->state == PUDDLE_GM_MEMBER && !strcmp(puddle_group_member(&grp, B)->by, A));
    CHECK(!strcmp(grp.creator, A) && grp.my == PUDDLE_GROUP_IN && !strcmp(grp.name, "Friends"));
    {
        char gid[53];
        CHECK(puddle_group_chat_id(A, T0, NONCE, gid) == 0);
        /* who may do what */
        CHECK(puddle_group_invite_build(gid, D, T0 + 200, C, ev, sizeof ev) > 0 && apply_text(&grp, ev, &why) == -1 && !strcmp(why, "not_a_member"));  /* a stranger */
        CHECK(puddle_group_rename_build(gid, D, T0 + 200, "Mine now", ev, sizeof ev) > 0 && apply_text(&grp, ev, &why) == -1 && !strcmp(why, "not_a_member"));
        CHECK(puddle_group_join_build(gid, D, T0 + 200, ev, sizeof ev) > 0 && apply_text(&grp, ev, &why) == -1 && !strcmp(why, "not_invited"));      /* uninvited */
        CHECK(puddle_group_leave_build(gid, D, T0 + 200, ev, sizeof ev) > 0 && apply_text(&grp, ev, &why) == -1 && !strcmp(why, "not_a_member"));
        CHECK(!puddle_group_member(&grp, D));
        /* an invited person is not yet a member, and cannot bring anyone in */
        {
            const char *E = "eaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", *F = "faaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
            CHECK(puddle_group_invite_build(gid, A, T0 + 290, E, ev, sizeof ev) > 0 && apply_text(&grp, ev, &why) == 0);
            CHECK(puddle_group_invite_build(gid, E, T0 + 295, F, ev, sizeof ev) > 0 && apply_text(&grp, ev, &why) == -1 && !strcmp(why, "not_a_member") &&
                  !puddle_group_member(&grp, F));
            CHECK(puddle_group_leave_build(gid, E, T0 + 296, ev, sizeof ev) > 0 && apply_text(&grp, ev, &why) == 0);
        }
        /* any member may invite, and rename */
        CHECK(puddle_group_invite_build(gid, B, T0 + 300, D, ev, sizeof ev) > 0 && apply_text(&grp, ev, &why) == 0);
        CHECK(puddle_group_member(&grp, D)->state == PUDDLE_GM_INVITED && !strcmp(puddle_group_member(&grp, D)->by, B));
        CHECK(puddle_group_invite_build(gid, C, T0 + 301, D, ev, sizeof ev) > 0 && apply_text(&grp, ev, &why) == 1);                                  /* again: nothing new */
        CHECK(puddle_group_join_build(gid, D, T0 + 250, ev, sizeof ev) > 0 && apply_text(&grp, ev, &why) == -1);                                      /* an answer older than the question */
        CHECK(puddle_group_join_build(gid, D, T0 + 400, ev, sizeof ev) > 0 && apply_text(&grp, ev, &why) == 0 && puddle_group_member(&grp, D)->state == PUDDLE_GM_MEMBER);
        CHECK(puddle_group_join_build(gid, D, T0 + 401, ev, sizeof ev) > 0 && apply_text(&grp, ev, &why) == 1);                                       /* already in */
        CHECK(puddle_group_rename_build(gid, C, T0 + 500, "Renamed", ev, sizeof ev) > 0 && apply_text(&grp, ev, &why) == 0 && !strcmp(grp.name, "Renamed"));
        CHECK(puddle_group_rename_build(gid, B, T0 + 450, "Older", ev, sizeof ev) > 0 && apply_text(&grp, ev, &why) == 1 && !strcmp(grp.name, "Renamed"));
        /* the same moment: the name that sorts later wins, on every Puddle */
        CHECK(puddle_group_rename_build(gid, B, T0 + 500, "Zeta", ev, sizeof ev) > 0 && apply_text(&grp, ev, &why) == 0 && !strcmp(grp.name, "Zeta"));
        CHECK(puddle_group_rename_build(gid, C, T0 + 500, "Alpha", ev, sizeof ev) > 0 && apply_text(&grp, ev, &why) == 1 && !strcmp(grp.name, "Zeta"));
        /* leaving */
        CHECK(puddle_group_leave_build(gid, D, T0 + 390, ev, sizeof ev) > 0 && apply_text(&grp, ev, &why) == 1 && puddle_group_member(&grp, D)->state == PUDDLE_GM_MEMBER); /* stale */
        CHECK(puddle_group_leave_build(gid, D, T0 + 600, ev, sizeof ev) > 0 && apply_text(&grp, ev, &why) == 0 && puddle_group_member(&grp, D)->state == PUDDLE_GM_LEFT);
        CHECK(puddle_group_leave_build(gid, D, T0 + 601, ev, sizeof ev) > 0 && apply_text(&grp, ev, &why) == 1);
        CHECK(puddle_group_rename_build(gid, D, T0 + 700, "Ex", ev, sizeof ev) > 0 && apply_text(&grp, ev, &why) == -1 && !strcmp(why, "not_a_member"));  /* a leaver has no say */
        /* a leaver can be invited again, by a later event only */
        CHECK(puddle_group_invite_build(gid, A, T0 + 500, D, ev, sizeof ev) > 0 && apply_text(&grp, ev, &why) == 1 && puddle_group_member(&grp, D)->state == PUDDLE_GM_LEFT);
        CHECK(puddle_group_invite_build(gid, A, T0 + 800, D, ev, sizeof ev) > 0 && apply_text(&grp, ev, &why) == 0 && puddle_group_member(&grp, D)->state == PUDDLE_GM_INVITED);
        /* an invited person can decline by leaving */
        CHECK(puddle_group_leave_build(gid, D, T0 + 900, ev, sizeof ev) > 0 && apply_text(&grp, ev, &why) == 0 && puddle_group_member(&grp, D)->state == PUDDLE_GM_LEFT);
        /* a create is never folded in */
        CHECK(puddle_group_create_build(A, T0, NONCE, "Friends", chat, ev, sizeof ev) > 0 && apply_text(&grp, ev, &why) == -1);
    }
    /* full */
    {
        puddle_group big;
        puddle_gevent create;
        char gid[53];
        CHECK(puddle_group_create_build(A, T0, NONCE, "Big", gid, ev, sizeof ev) > 0 && parse(ev, &create, &why) == 0 && puddle_group_init_created(&big, &create, T0) == 0);
        int added = 0, full = 0;
        for (int i = 0; i < 60; i++) {
            unsigned char raw[32] = { (unsigned char)(i + 10) };
            char who[53];
            CHECK(puddle_base32_encode(raw, 32, who, sizeof who) == 52);
            who[51] = 'a';
            if (!puddle_id_valid(who)) continue;
            CHECK(puddle_group_invite_build(gid, A, T0 + 10 + i, who, ev, sizeof ev) > 0);
            int r = apply_text(&big, ev, &why);
            if (r == 0) added++;
            else if (r == -1 && !strcmp(why, "group_full")) full++;
        }
        CHECK(added == PUDDLE_GROUP_MAX_MEMBERS - 1 && full > 0 && puddle_group_count(&big, PUDDLE_GM_MEMBER) + puddle_group_count(&big, PUDDLE_GM_INVITED) == PUDDLE_GROUP_MAX_MEMBERS);
        puddle_group_free(&big);
    }

    /* ---- the store ---- */
    {
        char data[700];
        snprintf(data, sizeof data, "%s/data", root);
        puddle_groups *gs = puddle_groups_open(data, err, sizeof err);
        CHECK(gs != NULL);
        puddle_group got;
        CHECK(puddle_groups_get(gs, grp.id, &got) == 1 && puddle_groups_get(gs, "nope", &got) == -1);
        CHECK(puddle_groups_put(gs, &grp, err, sizeof err) == 0);
        CHECK(puddle_groups_get(gs, grp.id, &got) == 0 && !strcmp(got.name, "Zeta") && got.nmembers == grp.nmembers && got.my == PUDDLE_GROUP_IN &&
              puddle_group_member(&got, D)->state == PUDDLE_GM_LEFT && !strcmp(puddle_group_member(&got, B)->by, A) && got.name_ts == T0 + 500);
        puddle_group_free(&got);
        CHECK(puddle_groups_update(gs, grp.id, rename_fn, "Changed", err, sizeof err) == 0);
        CHECK(puddle_groups_get(gs, grp.id, &got) == 0 && !strcmp(got.name, "Changed") && got.nmembers == grp.nmembers);
        puddle_group_free(&got);
        CHECK(puddle_groups_update(gs, grp.id, rename_fn, "skip", err, sizeof err) == 5);
        CHECK(puddle_groups_update(gs, grp.id, rename_fn, "fail", err, sizeof err) == -1);
        CHECK(puddle_groups_get(gs, grp.id, &got) == 0 && !strcmp(got.name, "Changed"));                 /* nothing stored by those */
        puddle_group_free(&got);
        CHECK(puddle_groups_update(gs, "caaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", rename_fn, "x", err, sizeof err) == 7); /* none there */
        CHECK(puddle_groups_update(gs, "bad", rename_fn, "x", err, sizeof err) == -1);
        puddle_group second;
        puddle_gevent create;
        char gid2[53];
        CHECK(puddle_group_create_build(B, T0 + 1, NONCE, "Second \xC3\xA9", gid2, ev, sizeof ev) > 0 && parse(ev, &create, &why) == 0);
        CHECK(puddle_group_init_created(&second, &create, T0) == 0);
        second.my = PUDDLE_GROUP_INVITED;
        snprintf(second.invited_by, sizeof second.invited_by, "%s", B);
        CHECK(puddle_groups_put(gs, &second, err, sizeof err) == 0);
        CHECK(puddle_groups_pending(gs) == 1);
        puddle_group *list;
        size_t n;
        CHECK(puddle_groups_list(gs, &list, &n) == 0 && n == 2);
        puddle_group_list_free(list, n);
        puddle_group_free(&second);
        puddle_groups_close(gs);
        /* survives a restart, and a damaged file is skipped */
        gs = puddle_groups_open(data, err, sizeof err);
        CHECK(puddle_groups_pending(gs) == 1 && puddle_groups_get(gs, gid2, &got) == 0 && !strcmp(got.name, "Second \xC3\xA9") && got.my == PUDDLE_GROUP_INVITED &&
              !strcmp(got.invited_by, B));
        puddle_group_free(&got);
        char path[1200];
        snprintf(path, sizeof path, "%s/state/groups/%s.json", data, gid2);
        FILE *f = fopen(path, "w");
        fputs("{\"v\":1,\"id\":\"x\"", f);
        fclose(f);
        CHECK(puddle_groups_get(gs, gid2, &got) == 1);
        CHECK(puddle_groups_list(gs, &list, &n) == 0 && n == 1);
        puddle_group_list_free(list, n);
        puddle_groups_close(gs);
    }
    puddle_group_free(&grp);

    nftw(root, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    return failures ? 1 : 0;
}
