#include "wire/wireprofile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

#define H1 "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
#define H2 "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210"

static char notes[PUDDLE_WIRE_NOTES_MAX];
static int clean(const char *in, char *out, size_t cap) { return puddle_profile_wire_clean(in, strlen(in), out, cap, notes, sizeof notes); }

int main(void)
{
    char out[PUDDLE_WIRE_PROFILE_MAX + 8];

    /* the whole thing, in one spelling */
    CHECK(clean("{\"pronouns\":\"they/them\",\"display_name\":\"  Ann\\u202e  \",\"unknown\":{\"x\":[1,2]},\"about\":\"line1\\nline2\","
                "\"blurb\":\"hi\",\"color\":{\"angle\":45,\"colors\":[\"#AABBCC\",\"#001122\"]},"
                "\"background\":{\"size\":3,\"mime\":\"image/webp\",\"hash\":\"" H2 "\"},"
                "\"avatar\":{\"hash\":\"" H1 "\",\"mime\":\"image/png\",\"size\":1234},\"banner\":{\"hash\":\"" H1 "\",\"mime\":\"image/gif\",\"size\":9},"
                "\"links\":[{\"url\":\"https://steamcommunity.com/id/ann\",\"text\":\"Steam\"},{\"text\":\"Battle.net: Ann#1234\"}]}",
                out, sizeof out) == 0);
    CHECK(!strcmp(out, "{\"display_name\":\"Ann\",\"blurb\":\"hi\",\"about\":\"line1\\nline2\",\"pronouns\":\"they/them\","
                       "\"color\":{\"colors\":[\"#aabbcc\",\"#001122\"],\"angle\":45},"
                       "\"avatar\":{\"hash\":\"" H1 "\",\"mime\":\"image/png\",\"size\":1234},"
                       "\"banner\":{\"hash\":\"" H1 "\",\"mime\":\"image/gif\",\"size\":9},"
                       "\"background\":{\"hash\":\"" H2 "\",\"mime\":\"image/webp\",\"size\":3},"
                       "\"links\":[{\"text\":\"Steam\",\"url\":\"https://steamcommunity.com/id/ann\"},{\"text\":\"Battle.net: Ann#1234\"}]}"));
    CHECK(notes[0] == '\0'); /* nothing was wrong: an unknown field is a newer Puddle's, not a mistake */
    /* what it comes to is valid, and cleaning it again changes nothing */
    {
        char again[PUDDLE_WIRE_PROFILE_MAX + 8];
        CHECK(clean(out, again, sizeof again) == 0 && !strcmp(again, out) && !notes[0]);
    }
    CHECK(clean("{\"display_name\":\"Just a name\"}", out, sizeof out) == 0 && !strcmp(out, "{\"display_name\":\"Just a name\"}"));
    CHECK(clean("{}", out, sizeof out) == 0 && !strcmp(out, "{}") && !notes[0]);                        /* no profile fields: no name, shown by id */
    CHECK(clean("{\"display_name\":\"x\",\"color\":{\"colors\":[\"#ABCDEF\"],\"angle\":90}}", out, sizeof out) == 0 &&
          !strcmp(out, "{\"display_name\":\"x\",\"color\":{\"colors\":[\"#abcdef\"]}}"));           /* one color has no direction */
    CHECK(clean("{\"display_name\":\"x\",\"color\":{\"colors\":[\"#000000\",\"#111111\"],\"angle\":360}}", out, sizeof out) == 0 && strstr(out, "\"angle\":360"));
    CHECK(clean("{\"display_name\":\"x\",\"avatar\":{\"hash\":\"" H1 "\",\"mime\":\"image/png\",\"size\":26214400}}", out, sizeof out) == 0 &&
          strstr(out, "avatar") && !notes[0]);                                                                  /* exactly the cap */
    CHECK(clean("{\"display_name\":\"x\",\"avatar\":null,\"links\":null}", out, sizeof out) == 0 && !strcmp(out, "{\"display_name\":\"x\"}") && !notes[0]);

    /* only what is not a JSON object is refused */
    const char *refused[] = { "", "[]", "{", "7", "\"x\"", "{\"a\":}" };
    for (size_t i = 0; i < sizeof refused / sizeof *refused; i++)
        if (clean(refused[i], out, sizeof out) == 0) { fprintf(stderr, "FAIL accepted: %s\n", refused[i]); failures++; }
    CHECK(clean("{\"display_name\":\"x\"}", out, 10) != 0); /* what it comes to does not fit */

    /* anything else wrong is left out, with a note naming it, and the rest is kept */
    static const struct { const char *in, *gone, *noted; } bad[] = {
        { "{\"display_name\":7}", "display_name", "display_name: not text" },
        { "{\"display_name\":\"x\",\"blurb\":7}", "blurb", "blurb: not text" },
        { "{\"display_name\":\"x\",\"about\":[]}", "about", "about: not text" },
        { "{\"display_name\":\"x\",\"pronouns\":{}}", "pronouns", "pronouns: not text" },
        { "{\"display_name\":\"x\",\"blurb\":\"a\",\"blurb\":\"b\"}", "blurb", "blurb: given more than once" },
        { "{\"display_name\":\"x\",\"color\":7}", "color", "color: not a list" },
        { "{\"display_name\":\"x\",\"color\":{}}", "color", "color: not a list" },
        { "{\"display_name\":\"x\",\"color\":{\"colors\":[]}}", "color", "color: no colour" },
        { "{\"display_name\":\"x\",\"color\":{\"colors\":\"#000000\"}}", "color", "color: not a list" },
        { "{\"display_name\":\"x\",\"color\":{\"colors\":[7]}}", "color", "color: 7 is not a colour" },
        { "{\"display_name\":\"x\",\"color\":{\"colors\":[\"red\"]}}", "color", "color: \"red\" is not a colour" },
        { "{\"display_name\":\"x\",\"color\":{\"colors\":[\"#12345g\"]}}", "color", "color: \"#12345g\" is not a colour like \"#3366ff\"; left out" },
        { "{\"display_name\":\"x\",\"color\":{\"colors\":[\"#00000\"]}}", "color", "is not a colour" },
        { "{\"display_name\":\"x\",\"color\":{\"colors\":[\"000000\"]}}", "color", "is not a colour" },
        { "{\"display_name\":\"x\",\"avatar\":7}", "avatar", "avatar: not a picture" },
        { "{\"display_name\":\"x\",\"avatar\":{}}", "avatar", "avatar: not a picture" },
        { "{\"display_name\":\"x\",\"avatar\":{\"hash\":\"" H1 "\",\"size\":5}}", "avatar", "avatar: not a picture" },
        { "{\"display_name\":\"x\",\"avatar\":{\"hash\":\"zz\",\"mime\":\"image/png\",\"size\":5}}", "avatar", "avatar: not a picture" },
        { "{\"display_name\":\"x\",\"avatar\":{\"hash\":\"0123456789ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef\",\"mime\":\"image/png\",\"size\":5}}", "avatar", "avatar:" },
        { "{\"display_name\":\"x\",\"banner\":{\"hash\":\"" H1 "\",\"mime\":\"image/svg+xml\",\"size\":5}}", "banner", "banner: not a picture" },
        { "{\"display_name\":\"x\",\"background\":{\"hash\":\"" H1 "\",\"mime\":\"image/png\",\"size\":26214401}}", "background", "background:" },
        { "{\"display_name\":\"x\",\"banner\":\"" H1 "\"}", "banner", "banner:" },
        { "{\"display_name\":\"x\",\"links\":\"https://x.example\"}", "links", "links: not a list" },
    };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        char key[40];
        snprintf(key, sizeof key, "\"%s\"", bad[i].gone);
        int ok = clean(bad[i].in, out, sizeof out) == 0 && !strstr(out, key) && strstr(notes, bad[i].noted);
        if (!ok) { fprintf(stderr, "FAIL %s -> %s [%s]\n", bad[i].in, out, notes); failures++; }
        if (strcmp(bad[i].gone, "display_name") != 0)
            CHECK(strstr(out, "\"display_name\":\"x\"")); /* the rest is kept */
    }

    /* colours: the good ones are kept, the bad ones and any past five left out, a bad angle dropped */
    CHECK(clean("{\"color\":{\"colors\":[\"#000000\",\"nope\",\"#111111\"],\"angle\":90}}", out, sizeof out) == 0 &&
          !strcmp(out, "{\"color\":{\"colors\":[\"#000000\",\"#111111\"],\"angle\":90}}") && strstr(notes, "\"nope\" is not a colour"));
    CHECK(clean("{\"color\":{\"colors\":[\"#000000\",\"#111111\",\"#222222\",\"#333333\",\"#444444\",\"#555555\"]}}", out, sizeof out) == 0 &&
          strstr(out, "#444444") && !strstr(out, "#555555") && strstr(notes, "more than 5 colours"));
    CHECK(clean("{\"color\":{\"colors\":[\"#000000\",\"#111111\"],\"angle\":361}}", out, sizeof out) == 0 &&
          strstr(out, "#111111") && !strstr(out, "angle") && strstr(notes, "color: the angle"));
    CHECK(clean("{\"color\":{\"colors\":[\"#000000\",\"#111111\"],\"angle\":\"90\"}}", out, sizeof out) == 0 && !strstr(out, "angle") && notes[0]);

    /* links */
    {
        CHECK(clean("{\"links\":[{\"text\":\"ok\",\"url\":\"javascript:alert(1)\"}]}", out, sizeof out) == 0 &&
              !strcmp(out, "{\"links\":[{\"text\":\"ok\"}]}") && strstr(notes, "link 1's address is not an http or https address"));
        CHECK(clean("{\"links\":[{\"text\":\"ok\",\"url\":\"https://a.example/x y\"}]}", out, sizeof out) == 0 && !strstr(out, "url"));
        CHECK(clean("{\"links\":[{\"text\":\"ok\",\"url\":\"https://\\\"><b>\"}]}", out, sizeof out) == 0 && !strstr(out, "url"));
        CHECK(clean("{\"links\":[{\"text\":\"ok\",\"url\":\"HTTPS://Steam.example/Me\"}]}", out, sizeof out) == 0 && strstr(out, "HTTPS://Steam.example/Me") && !notes[0]);
        CHECK(clean("{\"links\":[{\"url\":\"https://a.example\"},{\"text\":\"\\u0007\"},7,{\"text\":\"kept\"}]}", out, sizeof out) == 0 &&
              !strcmp(out, "{\"links\":[{\"text\":\"kept\"}]}") && strstr(notes, "link 1 has no text") && strstr(notes, "link 3 has no text"));
        char many[2000] = "{\"links\":[";
        for (int i = 0; i < 12; i++)
            snprintf(many + strlen(many), sizeof many - strlen(many), "%s{\"text\":\"t%d\"}", i ? "," : "", i);
        strcat(many, "]}");
        CHECK(clean(many, out, sizeof out) == 0 && strstr(out, "t9") && !strstr(out, "t10") && strstr(notes, "2 more than the 10 allowed"));
        char longtext[200];
        snprintf(longtext, sizeof longtext, "{\"links\":[{\"text\":\"%080d\"}]}", 7);
        CHECK(clean(longtext, out, sizeof out) == 0 && strstr(notes, "cut short") && strlen(out) < 100);
    }

    /* text that is too long is cut to its limit, not lost */
    {
        char big[40000], j[41000];
        memset(big, 'n', 64);
        big[64] = 0;
        snprintf(j, sizeof j, "{\"display_name\":\"%s\"}", big);
        CHECK(clean(j, out, sizeof out) == 0 && !notes[0]);
        memset(big, 'n', 65);
        big[65] = 0;
        snprintf(j, sizeof j, "{\"display_name\":\"%s\"}", big);
        CHECK(clean(j, out, sizeof out) == 0 && strlen(out) == strlen("{\"display_name\":\"\"}") + 64 &&
              strstr(notes, "display_name: longer than 64 characters; cut short"));
        for (int i = 0; i < 2000; i++) memcpy(big + i * 4, "\xF0\x9F\x99\x82", 4);                                         /* the longest an `about` can be */
        big[8000] = 0;
        snprintf(j, sizeof j, "{\"display_name\":\"x\",\"about\":\"%s\"}", big);
        CHECK(clean(j, out, sizeof out) == 0 && strlen(out) < PUDDLE_WIRE_PROFILE_MAX && !notes[0]);
        memcpy(big + 8000, "\xF0\x9F\x99\x82", 4);
        big[8004] = 0;
        snprintf(j, sizeof j, "{\"display_name\":\"x\",\"about\":\"%s\"}", big);
        CHECK(clean(j, out, sizeof out) == 0 && strstr(notes, "about: longer than 2000"));
        CHECK(strlen(strstr(out, "\"about\":\"") + 9) == 8000 + 2);                                                      /* 2000 characters, the quote and the brace */
    }

    /* the notes never overflow: the first that fit are kept, NUL-terminated */
    {
        char small[60];
        CHECK(puddle_profile_wire_clean("{\"blurb\":7,\"about\":7,\"pronouns\":7,\"color\":7}", 44, out, sizeof out, small, sizeof small) == 0);
        CHECK(strlen(small) < sizeof small && !strncmp(small, "blurb: not text", 15));
        CHECK(puddle_profile_wire_clean("{\"blurb\":7}", 11, out, sizeof out, NULL, 0) == 0 && !strcmp(out, "{}")); /* no notes wanted */
    }

    /* the pictures a profile points at */
    {
        puddle_media_ref refs[3];
        CHECK(puddle_profile_wire_media("{\"display_name\":\"x\"}", 20, refs) == 0);
        const char *p = "{\"display_name\":\"x\",\"avatar\":{\"hash\":\"" H1 "\",\"mime\":\"image/png\",\"size\":7},"
                        "\"background\":{\"hash\":\"" H2 "\",\"mime\":\"image/jpeg\",\"size\":8}}";
        CHECK(puddle_profile_wire_media(p, strlen(p), refs) == 2);
        CHECK(!strcmp(refs[0].hash, H1) && !strcmp(refs[0].mime, "image/png") && refs[0].size == 7 && !strcmp(refs[1].hash, H2) && refs[1].size == 8);
        p = "{\"display_name\":\"x\",\"avatar\":{\"hash\":\"bad\",\"mime\":\"image/png\",\"size\":7}}";
        CHECK(puddle_profile_wire_media(p, strlen(p), refs) == 0);                                                      /* a bad one is no reference */
    }
    return failures ? 1 : 0;
}
