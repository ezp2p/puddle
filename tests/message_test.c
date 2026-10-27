#include "events/message.h"

#include "base/encoding.h"
#include "base/hash.h"
#include "core/link.h"
#include "core/phrase.h"

#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static char root[512];
static int rm_cb(const char *p, const struct stat *sb, int t, struct FTW *f) { (void)sb; (void)t; (void)f; remove(p); return 0; }

static puddle_identity *make(const char *name, char *phrase, char *uid)
{
    char dir[700], err[300];
    snprintf(dir, sizeof dir, "%s/%s", root, name);
    puddle_identity *id = puddle_identity_create(dir, phrase, err, sizeof err);
    if (!id) exit(2);
    puddle_identity_confirm(dir);
    puddle_identity_id(id, uid);
    return id;
}

#define T 1760000000000LL

int main(void)
{
    const char *base = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/puddle-message-XXXXXX", base ? base : "/tmp");
    if (!mkdtemp(root)) return 2;
    char pa[PUDDLE_PHRASE_LEN + 1], pb[PUDDLE_PHRASE_LEN + 1], ua[64], ub[64];
    puddle_identity *alice = make("alice", pa, ua), *bob = make("bob", pb, ub);

    /* ---- the chat id ---- */
    char c1[64], c2[64], c3[64];
    CHECK(puddle_dm_chat_id(ua, ub, c1) == 0 && puddle_dm_chat_id(ub, ua, c2) == 0 && !strcmp(c1, c2));   /* both sides agree */
    CHECK(strlen(c1) == 52 && puddle_id_valid(c1));
    CHECK(puddle_dm_chat_id(ua, ua, c3) == 0 && strcmp(c3, c1) != 0);
    CHECK(puddle_dm_chat_id(ua, "short", c3) == -1 && puddle_dm_chat_id("", ub, c3) == -1);
    {
        /* fixed: the formula in docs/storage.md, worked out independently */
        char in[2 + 104], hex[65];
        const char *lo = strcmp(ua, ub) < 0 ? ua : ub, *hi = lo == ua ? ub : ua;
        memcpy(in, "dm", 2); memcpy(in + 2, lo, 52); memcpy(in + 54, hi, 52);
        puddle_sha256_hex(in, 106, hex);
        unsigned char raw[32];
        char want[64];
        CHECK(puddle_hex_decode(hex, raw, 32) == 0 && puddle_base32_encode(raw, 32, want, sizeof want) == 52 && !strcmp(want, c1));
    }

    /* ---- bodies ---- */
    CHECK(puddle_message_body_ok("hello", 5) && puddle_message_body_ok("a", 1));
    CHECK(puddle_message_body_ok("line\none\ttab", 12));
    CHECK(puddle_message_body_ok("\xC3\xA9\xE2\x82\xAC\xF0\x9F\x99\x82 \xE6\x97\xA5\xE6\x9C\xAC", 16));  /* any language */
    CHECK(!puddle_message_body_ok("", 0));
    CHECK(!puddle_message_body_ok("a\0b", 3) && !puddle_message_body_ok("bell\a", 5) && !puddle_message_body_ok("x\x7f", 2) && !puddle_message_body_ok("x\r", 2));
    CHECK(!puddle_message_body_ok("bad\xFF", 4) && !puddle_message_body_ok("\xC0\xAF", 2) && !puddle_message_body_ok("\xED\xA0\x80", 3) && !puddle_message_body_ok("\xE2\x82", 2));
    CHECK(!puddle_message_body_ok("a\xC2\x85z", 4));                                                   /* a C1 control */
    CHECK(!puddle_message_body_ok("a\xE2\x80\xA8z", 5) && !puddle_message_body_ok("a\xE2\x80\xA9z", 5)); /* line/paragraph separators */
    {
        char big[16100];
        memset(big, 'x', sizeof big);
        CHECK(puddle_message_body_ok(big, 4000) && !puddle_message_body_ok(big, 4001));
        for (int i = 0; i < 4000; i++) memcpy(big + i * 4, "\xF0\x9F\x99\x82", 4);
        CHECK(puddle_message_body_ok(big, 16000));                                                     /* 4000 characters of 4 bytes */
        CHECK(!puddle_message_body_ok(big, 16004));
    }

    /* ---- time never repeats ---- */
    {
        int64_t a = puddle_event_next_ts(T), b = puddle_event_next_ts(T), c = puddle_event_next_ts(T - 5000), d = puddle_event_next_ts(T + 100000);
        CHECK(a == T && b == T + 1 && c == T + 2 && d == T + 100000);
    }

    /* ---- build, parse ---- */
    char ev[PUDDLE_EVENT_MAX_BYTES];
    size_t el = puddle_message_build(c1, ua, T, "hi \"there\"\nsecond", 17, NULL, ev, sizeof ev);
    CHECK(el > 0 && el == strlen(ev) && !strchr(ev, '\n') && !strstr(ev, ", "));                     /* compact, one line */
    CHECK(!strncmp(ev, "{\"type\":\"message\",\"chat\":\"", 26));                                  /* the fields come in a fixed order */
    puddle_message m;
    const char *why;
    CHECK(puddle_message_parse(ev, el, &m, &why) == 0 && !strcmp(m.chat, c1) && !strcmp(m.sender, ua) && m.ts == T &&
          !strcmp(m.body, "hi \"there\"\nsecond") && !m.ref[0]);
    puddle_message_free(&m);
    const char *ref = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    el = puddle_message_build(c1, ua, T, "re", 2, ref, ev, sizeof ev);
    CHECK(el > 0 && puddle_message_parse(ev, el, &m, &why) == 0 && !strcmp(m.ref, ref));
    puddle_message_free(&m);
    CHECK(puddle_message_build(c1, ua, T, "x", 1, "not an id", ev, sizeof ev) == 0);
    CHECK(puddle_message_build(c1, ua, T, "", 0, NULL, ev, sizeof ev) == 0);
    CHECK(puddle_message_build(c1, ua, T, "bell\a", 5, NULL, ev, sizeof ev) == 0);
    CHECK(puddle_message_build(c1, ua, T, "hello", 5, NULL, ev, 40) == 0);                              /* does not fit */
    CHECK(puddle_message_build(c1, ua, T, "x", 1, "", ev, sizeof ev) > 0);                              /* an empty reply is none */

    /* what a receiver refuses, and says why */
    {
        struct { const char *event; const char *why; } bad[] = {
            { "", "malformed" }, { "[]", "malformed" }, { "{", "malformed" }, { "{\"type\":\"message\"}", "malformed" },
            { "{\"type\":\"edit\",\"chat\":\"x\",\"sender\":\"y\",\"ts\":1,\"body\":\"b\"}", "unsupported_type" },
            { "{\"type\":7}", "malformed" },
        };
        for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
            if (puddle_message_parse(bad[i].event, strlen(bad[i].event), &m, &why) == 0 || strcmp(why, bad[i].why)) {
                fprintf(stderr, "FAIL parse %zu: %s\n", i, why);
                failures++;
            }
        }
        char t[1200];
        const char *fmt = "{\"type\":\"message\",\"chat\":\"%s\",\"sender\":\"%s\",\"ts\":%lld,\"body\":%s%s}";
        struct { long long ts; const char *body; const char *extra; const char *why; } v[] = {
            { 0, "\"x\"", "", "bad_time" }, { 946684799999LL, "\"x\"", "", "bad_time" }, { 4102444800000LL, "\"x\"", "", "bad_time" },
            { -5, "\"x\"", "", "bad_time" }, { T, "\"\"", "", "bad_body" }, { T, "7", "", "bad_body" }, { T, "\"a\\u0000b\"", "", "bad_body" },
            { T, "\"bell\\u0007\"", "", "bad_body" }, { T, "\"x\"", ",\"ref\":\"short\"", "malformed" }, { T, "\"x\"", ",\"ref\":5", "malformed" },
            { T, "\"x\"", ",\"ref\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdeg\"", "malformed" },
        };
        for (size_t i = 0; i < sizeof v / sizeof *v; i++) {
            snprintf(t, sizeof t, fmt, c1, ua, v[i].ts, v[i].body, v[i].extra);
            if (puddle_message_parse(t, strlen(t), &m, &why) == 0 || strcmp(why, v[i].why)) {
                fprintf(stderr, "FAIL parse variant %zu: %s (wanted %s)\n", i, why, v[i].why);
                failures++;
            }
        }
        snprintf(t, sizeof t, fmt, c1, ua, (long long)T, "\"fine\"", "");
        CHECK(puddle_message_parse(t, strlen(t), &m, &why) == 0);
        puddle_message_free(&m);
        snprintf(t, sizeof t, "{\"type\":\"message\",\"type\":\"message\",\"chat\":\"%s\",\"sender\":\"%s\",\"ts\":%lld,\"body\":\"x\"}", c1, ua, (long long)T);
        CHECK(puddle_message_parse(t, strlen(t), &m, &why) == -1);                                     /* a repeated key is ambiguous */
        snprintf(t, sizeof t, fmt, "short", ua, (long long)T, "\"x\"", "");
        CHECK(puddle_message_parse(t, strlen(t), &m, &why) == -1);
        static char huge[PUDDLE_EVENT_MAX_BYTES + 100];
        memset(huge, 'x', sizeof huge);
        huge[0] = '{';
        huge[sizeof huge - 1] = '}';
        CHECK(puddle_message_parse(huge, sizeof huge, &m, &why) == -1 && !strcmp(why, "malformed"));
    }

    /* ---- edits and redactions ---- */
    {
        const char *target = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
        size_t l2 = puddle_edit_build(c1, ua, T + 5, target, "new text", 8, ev, sizeof ev);
        CHECK(l2 > 0 && !strncmp(ev, "{\"type\":\"edit\",\"chat\":\"", 23));
        CHECK(puddle_chat_event_parse(ev, l2, &m, &why) == 0 && m.type == PUDDLE_EV_EDIT && !strcmp(m.ref, target) && !strcmp(m.body, "new text") && m.ts == T + 5);
        puddle_message_free(&m);
        CHECK(puddle_message_parse(ev, l2, &m, &why) == -1 && !strcmp(why, "unsupported_type"));          /* an edit is not a message */
        size_t l3 = puddle_redact_build(c1, ua, T + 6, target, ev, sizeof ev);
        CHECK(l3 > 0 && !strstr(ev, "body"));
        CHECK(puddle_chat_event_parse(ev, l3, &m, &why) == 0 && m.type == PUDDLE_EV_REDACT && !strcmp(m.ref, target) && m.body == NULL);
        puddle_message_free(&m);
        CHECK(puddle_message_parse(ev, l3, &m, &why) == -1 && !strcmp(why, "unsupported_type"));
        CHECK(puddle_edit_build(c1, ua, T, "not an id", "x", 1, ev, sizeof ev) == 0 && puddle_edit_build(c1, ua, T, target, "", 0, ev, sizeof ev) == 0);
        CHECK(puddle_edit_build(c1, ua, T, target, "bell\a", 5, ev, sizeof ev) == 0 && puddle_redact_build(c1, ua, T, "short", ev, sizeof ev) == 0);
        CHECK(puddle_edit_build(c1, ua, T, target, "x", 1, ev, 30) == 0);
        char t[1200];
        const char *base_f = "{\"type\":\"%s\",\"chat\":\"%s\",\"sender\":\"%s\",\"ts\":%lld%s}";
        struct { const char *type, *extra; const char *why; } v[] = {
            { "edit", "", "bad_body" },                                                                  /* an edit with no text */
            { "edit", ",\"body\":\"x\"", "malformed" },                                                    /* ...or no target */
            { "edit", ",\"body\":\"x\",\"ref\":\"short\"", "malformed" },
            { "edit", ",\"body\":\"\",\"ref\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"", "bad_body" },
            { "redact", "", "malformed" },                                                               /* a redaction with no target */
            { "redact", ",\"ref\":\"short\"", "malformed" },
            { "redact", ",\"ref\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\",\"body\":\"smuggled\"", "malformed" },
            { "redact", ",\"ref\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\",\"body\":\"\"", "malformed" },
            { "wipe", ",\"ref\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"", "unsupported_type" },
        };
        for (size_t i = 0; i < sizeof v / sizeof *v; i++) {
            snprintf(t, sizeof t, base_f, v[i].type, c1, ua, (long long)T, v[i].extra);
            if (puddle_chat_event_parse(t, strlen(t), &m, &why) == 0 || strcmp(why, v[i].why)) {
                fprintf(stderr, "FAIL edit variant %zu: %s (wanted %s)\n", i, why ? why : "ok", v[i].why);
                failures++;
            }
        }
        snprintf(t, sizeof t, base_f, "edit", c1, ua, 0LL, ",\"body\":\"x\",\"ref\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"");
        CHECK(puddle_chat_event_parse(t, strlen(t), &m, &why) == -1 && !strcmp(why, "bad_time"));
    }

    /* ---- the view of a chat ---- */
    {
        /* events as the store would hand them back: in chat order, each with its id */
        puddle_event evs[32];
        size_t n = 0;
        char ev2[PUDDLE_EVENT_MAX_BYTES];
        char ids[32][65];
        struct add_fn { int dummy; } unused;
        (void)unused;
        #define ADD(buf, len) do { memset(&evs[n], 0, sizeof evs[n]); evs[n].event = strdup(buf); evs[n].event_len = len; \
            puddle_sha256_hex(buf, len, evs[n].id); snprintf(ids[n], 65, "%s", evs[n].id); evs[n].rts = 1000 + (int64_t)n; n++; } while (0)
        size_t l;
        l = puddle_message_build(c1, ua, T + 10, "A one", 5, NULL, ev2, sizeof ev2); ADD(ev2, l);                       /* 0 */
        l = puddle_message_build(c1, ub, T + 20, "B one", 5, NULL, ev2, sizeof ev2); ADD(ev2, l);                       /* 1 */
        l = puddle_message_build(c1, ua, T + 30, "A two", 5, ids[1], ev2, sizeof ev2); ADD(ev2, l);                     /* 2: a reply to 1 */
        l = puddle_edit_build(c1, ua, T + 40, ids[0], "A one, edited", 13, ev2, sizeof ev2); ADD(ev2, l);               /* 3 */
        l = puddle_edit_build(c1, ub, T + 41, ids[0], "B tries to edit A", 17, ev2, sizeof ev2); ADD(ev2, l);           /* 4: not theirs */
        l = puddle_edit_build(c1, ua, T + 42, ids[0], "A one, edited again", 19, ev2, sizeof ev2); ADD(ev2, l);         /* 5: the last one stands */
        l = puddle_edit_build(c1, ua, T + 10, ids[2], "too early", 9, ev2, sizeof ev2); ADD(ev2, l);                    /* 6: before what it edits */
        l = puddle_edit_build(c1, ua, T + 43, ids[3], "an edit of an edit", 18, ev2, sizeof ev2); ADD(ev2, l);          /* 7: not a message */
        l = puddle_redact_build(c1, ub, T + 50, ids[2], ev2, sizeof ev2); ADD(ev2, l);                                  /* 8: B cannot take back A's */
        l = puddle_edit_build(c1, ub, T + 51, ids[1], "B one, edited", 13, ev2, sizeof ev2); ADD(ev2, l);               /* 9 */
        l = puddle_redact_build(c1, ub, T + 52, ids[1], ev2, sizeof ev2); ADD(ev2, l);                                  /* 10: B takes it back */
        l = puddle_edit_build(c1, ub, T + 53, ids[1], "B one, after taking back", 24, ev2, sizeof ev2); ADD(ev2, l);   /* 11: too late */
        l = puddle_redact_build(c1, ua, T + 30, ids[2], ev2, sizeof ev2); ADD(ev2, l);                                  /* 12: not after what it redacts */
        l = puddle_redact_build(c1, ua, T + 60, "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff", ev2, sizeof ev2); ADD(ev2, l); /* 13: nothing there */
        l = puddle_message_build(c1, ua, T + 70, "A replies to A", 14, ids[0], ev2, sizeof ev2); ADD(ev2, l);          /* 14: a reply to one's own is not an edit */
        ADD("{\"type\":\"chat.create\"}", 22);                                                                       /* 15: not ours to read */
        ADD("not json", 8);                                                                                              /* 16 */
        puddle_view_msg *view;
        size_t count;
        CHECK(puddle_chat_view(evs, n, &view, &count) == 0 && count == 4);
        CHECK(!strcmp(view[0].id, ids[0]) && !strcmp(view[0].body, "A one, edited again") && view[0].edited && view[0].edited_ts == T + 42 && !view[0].deleted);
        CHECK(view[0].ts == T + 10 && view[0].rts == 1000 && !strcmp(view[0].sender, ua));                          /* it keeps its place and its time */
        CHECK(!strcmp(view[1].id, ids[1]) && view[1].deleted && view[1].body == NULL);                              /* taken back, and stays so */
        CHECK(!strcmp(view[2].id, ids[2]) && !strcmp(view[2].body, "A two") && !view[2].edited && !view[2].deleted && !strcmp(view[2].ref, ids[1]));
        CHECK(!strcmp(view[3].body, "A replies to A") && !strcmp(view[3].ref, ids[0]) && !view[3].edited);
        CHECK(!strcmp(view[0].body, "A one, edited again"));                                                         /* ...and what it replied to is as it was */
        puddle_view_free(view, count);
        /* the same, a window that starts after the first message: its edits are for nothing */
        CHECK(puddle_chat_view(evs + 1, n - 1, &view, &count) == 0 && count == 3 && view[1].edited == 0);
        puddle_view_free(view, count);
        /* a redaction after an edit wins, and before an edit wins too */
        {
            puddle_event two[3];
            memset(two, 0, sizeof two);
            char b1[PUDDLE_EVENT_MAX_BYTES], b2[PUDDLE_EVENT_MAX_BYTES], b3[PUDDLE_EVENT_MAX_BYTES];
            size_t k1 = puddle_message_build(c1, ua, T + 10, "x", 1, NULL, b1, sizeof b1);
            char mid[65];
            puddle_sha256_hex(b1, k1, mid);
            size_t k2 = puddle_redact_build(c1, ua, T + 20, mid, b2, sizeof b2);
            size_t k3 = puddle_edit_build(c1, ua, T + 30, mid, "late edit", 9, b3, sizeof b3);
            two[0].event = b1; two[0].event_len = k1; snprintf(two[0].id, 65, "%s", mid);
            two[1].event = b2; two[1].event_len = k2; puddle_sha256_hex(b2, k2, two[1].id);
            two[2].event = b3; two[2].event_len = k3; puddle_sha256_hex(b3, k3, two[2].id);
            CHECK(puddle_chat_view(two, 3, &view, &count) == 0 && count == 1 && view[0].deleted && !view[0].body && !view[0].edited);
            puddle_view_free(view, count);
        }
        CHECK(puddle_chat_view(evs, 0, &view, &count) == 0 && count == 0);
        puddle_view_free(view, count);
        for (size_t i = 0; i < n; i++) free(evs[i].event);
        #undef ADD
    }

    /* ---- signatures ---- */
    {
        el = puddle_message_build(c1, ua, T, "signed", 6, NULL, ev, sizeof ev);
        char sig[PUDDLE_SIG_TEXT_MAX], kid[65], mykid[65];
        unsigned char s[64], pub[32], bpub[32];
        CHECK(puddle_event_sign(alice, ev, el, sig) == 0);
        puddle_identity_public(alice, pub, NULL, NULL);
        puddle_identity_public(bob, bpub, NULL, NULL);
        puddle_key_id(pub, mykid);
        CHECK(puddle_event_sig_parse(sig, kid, s) == 0 && !strcmp(kid, mykid));
        CHECK(puddle_event_verify(pub, ev, el, s));
        CHECK(!puddle_event_verify(bpub, ev, el, s));                                                  /* someone else's key */
        ev[el - 5] ^= 1;
        CHECK(!puddle_event_verify(pub, ev, el, s));                                                   /* one bit changed */
        ev[el - 5] ^= 1;
        CHECK(!puddle_event_verify(pub, ev, el - 1, s));                                               /* cut short */
        /* the signature is over a domain and the bytes: the bytes alone do not verify */
        unsigned char plain[64];
        CHECK(puddle_identity_sign(alice, ev, el, plain) == 0 && !puddle_event_verify(pub, ev, el, plain));
        CHECK(puddle_verify_signature(pub, ev, el, plain));
        /* a signature made here is not a signature over the same bytes without the domain */
        CHECK(!puddle_verify_signature(pub, ev, el, s));

        CHECK(puddle_event_sig_parse(NULL, kid, s) == -1 && puddle_event_sig_parse("", kid, s) == -1 && puddle_event_sig_parse("nocolon", kid, s) == -1);
        char t[300];
        snprintf(t, sizeof t, "%.63s:%s", sig, strchr(sig, ':') + 1);
        CHECK(puddle_event_sig_parse(t, kid, s) == -1);                                                /* a short key id */
        snprintf(t, sizeof t, "%s", sig);
        t[10] = 'g';
        CHECK(puddle_event_sig_parse(t, kid, s) == -1);                                                /* not hex */
        snprintf(t, sizeof t, "%.64s:AAAA", sig);
        CHECK(puddle_event_sig_parse(t, kid, s) == -1);                                                /* too short */
        snprintf(t, sizeof t, "%.64s:%s=", sig, strchr(sig, ':') + 1);
        CHECK(puddle_event_sig_parse(t, kid, s) == -1);                                                /* too long */
        snprintf(t, sizeof t, "%.64s", sig);
        CHECK(puddle_event_sig_parse(t, kid, s) == -1);                                                /* no signature */
    }

    puddle_identity_close(alice);
    puddle_identity_close(bob);
    nftw(root, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    return failures ? 1 : 0;
}
