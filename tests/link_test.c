#include "core/link.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define ID "omhh6dd2fusc2douu4rdqigbshrjdz2zprtyxdf3j7igten6pmrq"
#define TOKEN "abcdefghijklmnopqrstuvwxy2"

static int parse(const char *t, puddle_link *l) { char e[100]; return puddle_link_parse(t, l, e, sizeof e); }
static int addr(const char *t, puddle_address *a) { return puddle_address_parse(t, a); }

int main(void)
{
    puddle_link l;
    puddle_address a;

    /* ids */
    CHECK(puddle_id_valid(ID));
    CHECK(puddle_id_valid("n2uaokv4vemetktypypeiykt2loiodxpi5wyabccwfaorgvgxgya"));
    CHECK(!puddle_id_valid(NULL) && !puddle_id_valid("") && !puddle_id_valid("abc"));
    CHECK(!puddle_id_valid("omhh6dd2fusc2douu4rdqigbshrjdz2zprtyxdf3j7igten6pmrb")); /* a second spelling of an id */
    CHECK(!puddle_id_valid("OMHH6dd2fusc2douu4rdqigbshrjdz2zprtyxdf3j7igten6pmrq")); /* upper case */
    CHECK(!puddle_id_valid("omhh6dd2fusc2douu4rdqigbshrjdz2zprtyxdf3j7igten6pmr1")); /* 1 is not base32 */
    CHECK(!puddle_id_valid(ID "q"));
    CHECK(!puddle_id_valid("../../etc/passwd/../../xxxxxxxxxxxxxxxxxxxxxxxxxxxxq"));

    /* what an owner types or pastes: an address, with or without the scheme */
    CHECK(parse("203.0.113.7:8443", &l) == 0);
    CHECK(!strcmp(l.address.host, "203.0.113.7") && l.address.port == 8443 && !strcmp(l.address.address, "203.0.113.7:8443") && l.invite[0] == '\0');
    CHECK(parse("puddle://203.0.113.7:8443", &l) == 0 && !strcmp(l.address.address, "203.0.113.7:8443"));
    CHECK(parse("[2001:db8::1]:9000", &l) == 0 && !strcmp(l.address.host, "2001:db8::1") && l.address.port == 9000 &&
          !strcmp(l.address.address, "[2001:db8::1]:9000"));
    CHECK(parse("Home.Example.ORG:1", &l) == 0 && !strcmp(l.address.address, "home.example.org:1"));
    CHECK(parse("home.example.org", &l) == 0 && l.address.port == 443 && !strcmp(l.address.address, "home.example.org:443")); /* port defaults to 443 */
    CHECK(parse("203.0.113.7", &l) == 0 && l.address.port == 443);
    CHECK(parse("[::1]", &l) == 0 && !strcmp(l.address.address, "[::1]:443"));
    CHECK(parse("puddle://[::1]", &l) == 0 && l.address.port == 443);
    CHECK(parse("10.0.0.1:8443?invite=" TOKEN, &l) == 0 && !strcmp(l.invite, TOKEN));
    CHECK(parse("puddle://10.0.0.1?invite=" TOKEN, &l) == 0 && !strcmp(l.invite, TOKEN) && l.address.port == 443);
    CHECK(parse("10.0.0.1:8443/", &l) == 0);
    CHECK(parse("10.0.0.1:8443/?invite=" TOKEN, &l) != 0); /* a path before the query: not ours */
    CHECK(parse("[::1]:1?invite=" TOKEN, &l) == 0);

    const char *bad[] = {
        "", "puddle://", "puddle:", "https://1.2.3.4:1", "PUDDLE://1.2.3.4:1", " 1.2.3.4:1", "1.2.3.4:1 ", "1.2.3.4:1\n", "1.2.3.4:1\t",
        "puddle://" ID "@1.2.3.4:1",                                   /* an id is not part of an address */
        "bob@1.2.3.4:1", "1.2.3.4:1@5.6.7.8:1", "evil@1.2.3.4:1", "@1.2.3.4:1",
        "1.2.3.4:1/path", "1.2.3.4:1#frag", "1.2.3.4:1?x=1", "1.2.3.4:1?invite=", "1.2.3.4:1?invite=short",
        "1.2.3.4:1?invite=" TOKEN "x", "1.2.3.4:1?invite=ABCDEFGHIJKLMNOPQRSTUVWXY2",
        "1.2.3.4:1?invite=" TOKEN "&invite=" TOKEN, "1.2.3.4:1?invite=" TOKEN "&x=1",
        "1.2.3.4:0", "1.2.3.4:65536", "1.2.3.4:-1", "1.2.3.4:", "1.2.3.4:80a", "1.2.3.4:123456", "1.2.3.4:+80", "1.2.3.4:8 0",
        "999.1.1.1:1", "256.1.1.1:1", "1.2.3.256:1", "1.2.3.4:1?abcdef=" TOKEN, "1.2.3.4:000080", "1.2.3:1", "1.2.3.4.5:1", "1.2.3.:1",
        "01234.1.1.1:1",
        "::1:8443", "[::1:8443", "::1]:8443", "[]:1", "[::1]x", "[::1]:", "[g::1]:1", "[1:2:3]:1",
        "[1::2::3]:1", "[:1]:1", "[1:]:1", "[1:::2]:1", "[12345::]:1", "[1:2:3:4:5:6:7:8:9]:1", "[1:2:3:4:5:6:7:8::]:1",
        "[::ffff:1.2.3.4]:1", "[fe80::1%eth0]:1",
        "-a.example:1", "a-.example:1", "a..example:1", ".example:1", "exa_mple.org:1", "exa mple.org:1",
        "\xC3\xA9xample.org:1", "a.example.:1x", "localhost:8443:9", "[::1]:8443:9",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        char with_scheme[400];
        snprintf(with_scheme, sizeof with_scheme, "puddle://%s", bad[i]);
        if (strncmp(bad[i], "puddle:", 7) && parse(with_scheme, &l) == 0 && strcmp(bad[i], "") && strcmp(bad[i], " 1.2.3.4:1") &&
            strcmp(bad[i], "1.2.3.4:1 ") && strcmp(bad[i], "1.2.3.4:1\n") && strcmp(bad[i], "1.2.3.4:1\t")) {
            fprintf(stderr, "FAIL accepted with a scheme: %s\n", bad[i]);
            failures++;
        }
        if (parse(bad[i], &l) == 0) {
            fprintf(stderr, "FAIL accepted: %s\n", bad[i]);
            failures++;
        }
    }
    char e[100] = "";
    CHECK(puddle_link_parse("nope!", &l, e, sizeof e) == -1 && e[0]);
    CHECK(puddle_link_parse(NULL, &l, e, sizeof e) == -1);
    CHECK(puddle_link_parse("1.2.3.4:1", &l, NULL, 0) == 0);

    /* an over-long hostname label or name */
    char host[400], text[600];
    memset(host, 'a', 64); host[64] = 0;
    snprintf(text, sizeof text, "%s.example:1", host);
    CHECK(parse(text, &l) != 0);
    memset(host, 'a', 63); host[63] = 0;
    snprintf(text, sizeof text, "%s.example:1", host);
    CHECK(parse(text, &l) == 0);
    memset(host, 'a', 300); host[300] = 0;
    snprintf(text, sizeof text, "%s:1", host);
    CHECK(parse(text, &l) != 0);

    /* addresses on their own */
    CHECK(addr("1.2.3.4:5", &a) == 0 && a.port == 5 && !strcmp(a.address, "1.2.3.4:5"));
    CHECK(addr("fe80::1", &a) != 0 && addr("[fe80::1]", &a) == 0 && !strcmp(a.address, "[fe80::1]:443"));
    CHECK(addr("[::]:1", &a) == 0 && addr("[::ABCD]:1", &a) == 0 && !strcmp(a.host, "::abcd")); /* case folded */
    CHECK(addr("", &a) != 0 && addr(NULL, &a) != 0 && addr("a b", &a) != 0);
    CHECK(addr("a:1", &a) == 0 && addr("A:1", &a) == 0 && !strcmp(a.host, "a"));
    CHECK(addr("1-2.example:1", &a) == 0);

    /* tokens */
    CHECK(puddle_invite_token_valid(TOKEN) && !puddle_invite_token_valid("") && !puddle_invite_token_valid(NULL));
    CHECK(!puddle_invite_token_valid("abcdefghijklmnopqrstuvwxy")); /* one short */
    CHECK(!puddle_invite_token_valid("abcdefghijklmnopqrstuvwxy1")); /* 1 is not base32 */
    CHECK(!puddle_invite_token_valid("ABCDEFGHIJKLMNOPQRSTUVWXY2"));

    /* formatting, and back: the port everyone assumes is not written */
    char out[300];
    CHECK(puddle_link_format("[::1]:8443", NULL, out, sizeof out) == 0 && !strcmp(out, "[::1]:8443"));
    CHECK(puddle_link_format("[::1]:443", NULL, out, sizeof out) == 0 && !strcmp(out, "[::1]") && parse(out, &l) == 0 && l.address.port == 443);
    CHECK(puddle_link_format("home.example.org:443", NULL, out, sizeof out) == 0 && !strcmp(out, "home.example.org"));
    CHECK(puddle_link_format("1.2.3.4:5443", NULL, out, sizeof out) == 0 && !strcmp(out, "1.2.3.4:5443")); /* only exactly :443 */
    CHECK(puddle_link_format("1.2.3.4:5", TOKEN, out, sizeof out) == 0 && parse(out, &l) == 0 && !strcmp(l.invite, TOKEN) && l.address.port == 5);
    CHECK(puddle_link_format("1.2.3.4:443", TOKEN, out, sizeof out) == 0 && !strcmp(out, "1.2.3.4?invite=" TOKEN));
    CHECK(puddle_link_format("1.2.3.4:5", "", out, sizeof out) == 0 && !strchr(out, '?'));
    CHECK(puddle_link_format("1.2.3.4:5", NULL, out, 4) == -1);

    /* nothing random crashes, and what is accepted is always the canonical thing */
    srand(11);
    const char alphabet[] = "puddle:/@[]?=&#%.-_ 09afAF:xyz\xC3\x01";
    for (int i = 0; i < 40000; i++) {
        char t[200];
        size_t n = (size_t)(rand() % 120);
        for (size_t k = 0; k < n; k++)
            t[k] = rand() % 3 ? alphabet[rand() % (sizeof alphabet - 1)] : (char)(rand() % 256 | 1);
        t[n] = 0;
        char buf[300];
        if (rand() % 2) { /* sometimes start from something well-formed */
            snprintf(buf, sizeof buf, "10.0.0.%s", t);
            if (parse(buf, &l) == 0) {
                char again[300];
                CHECK(puddle_link_format(l.address.address, l.invite, again, sizeof again) == 0 && parse(again, &l) == 0);
            }
        } else {
            (void)parse(t, &l);
            (void)addr(t, &a);
        }
    }
    return failures ? 1 : 0;
}
