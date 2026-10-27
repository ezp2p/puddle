#include "base/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static int member(const char *json, const char *key, puddle_span *v)
{
    return puddle_json_member(json, strlen(json), key, v);
}

static int valid(const char *json) { return puddle_json_valid_object(json, strlen(json)); }

int main(void)
{
    puddle_span v;
    char buf[64];
    int64_t n;

    /* validity */
    CHECK(valid("{}"));
    CHECK(valid(" {\"a\":[1,2.5e3,-0,true,false,null,{\"b\":\"c\"}]} "));
    CHECK(!valid("[]"));
    CHECK(!valid("{\"a\":1,}"));
    CHECK(!valid("{\"a\":01}"));
    CHECK(!valid("{\"a\":\"x}"));
    CHECK(!valid("{\"a\":\"\\q\"}"));
    CHECK(!valid("{\"a\":1} x"));
    CHECK(!valid("{\"a\":\"line\nbreak\"}")); /* raw control char in string */

    /* depth limit */
    char deep[400];
    int d = 0;
    for (int i = 0; i < 100; i++) deep[d++] = '[';
    for (int i = 0; i < 100; i++) deep[d++] = ']';
    char wrapped[500];
    snprintf(wrapped, sizeof wrapped, "{\"a\":%.*s}", d, deep);
    CHECK(!puddle_json_valid_object(wrapped, strlen(wrapped)));

    /* member lookup returns raw bytes and only matches top-level keys */
    const char *ev = "{\"body\":\"\\\"ts\\\":1 and \\\"chat\\\":\\\"x\\\"\",\"inner\":{\"ts\":5},\"ts\":1760000000000,\"chat\":\"c1\"}";
    CHECK(member(ev, "ts", &v) == 0);
    CHECK(puddle_json_int64(v, &n) == 0 && n == 1760000000000LL);
    CHECK(member(ev, "chat", &v) == 0);
    CHECK(puddle_json_string(v, buf, sizeof buf) == 0 && strcmp(buf, "c1") == 0);
    CHECK(member(ev, "inner", &v) == 0 && v.len == 8 && memcmp(v.p, "{\"ts\":5}", 8) == 0);
    CHECK(member(ev, "missing", &v) == 1);

    /* duplicate keys are rejected, not resolved */
    CHECK(member("{\"ts\":1,\"ts\":2}", "ts", &v) == -1);
    CHECK(member("{\"ts\":1", "ts", &v) == -1);

    /* escaped keys */
    CHECK(member("{\"t\\u0073\":7}", "ts", &v) == 0);

    /* strings */
    CHECK(member("{\"s\":\"a\\n\\u00e9\\ud83d\\ude00\"}", "s", &v) == 0);
    CHECK(puddle_json_string(v, buf, sizeof buf) == 0);
    CHECK(strcmp(buf, "a\n\xc3\xa9\xf0\x9f\x98\x80") == 0);
    CHECK(member("{\"s\":\"\\ud83d\"}", "s", &v) == 0 && puddle_json_string(v, buf, sizeof buf) == -1); /* lone surrogate */
    CHECK(member("{\"s\":\"\\u0000\"}", "s", &v) == 0 && puddle_json_string(v, buf, sizeof buf) == -1);
    CHECK(member("{\"s\":\"toolongvalue\"}", "s", &v) == 0 && puddle_json_string(v, buf, 5) == -1);
    CHECK(member("{\"s\":5}", "s", &v) == 0 && puddle_json_string(v, buf, sizeof buf) == -1);

    /* integers */
    CHECK(member("{\"n\":-42}", "n", &v) == 0 && puddle_json_int64(v, &n) == 0 && n == -42);
    CHECK(member("{\"n\":9223372036854775807}", "n", &v) == 0 && puddle_json_int64(v, &n) == 0 && n == INT64_MAX);
    CHECK(member("{\"n\":9223372036854775808}", "n", &v) == 0 && puddle_json_int64(v, &n) == -1);
    CHECK(member("{\"n\":1.5}", "n", &v) == 0 && puddle_json_int64(v, &n) == -1);
    CHECK(member("{\"n\":1e3}", "n", &v) == 0 && puddle_json_int64(v, &n) == -1);
    CHECK(member("{\"n\":\"7\"}", "n", &v) == 0 && puddle_json_int64(v, &n) == -1);

    /* ---- walking an array ---- */
    {
        puddle_span arr, it;
        size_t cur;
        int n;
        const char *cases[][2] = {
            { "[]", "0" }, { " [ ] ", "0" }, { "[1]", "1" }, { "[1,2,3]", "3" }, { " [ 1 , \"a,b]\" , {\"x\":[1,2]} , [ ] , null ] ", "5" },
            { "[\"]\",\"[\"]", "2" },
        };
        for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
            arr.p = cases[i][0];
            arr.len = strlen(arr.p);
            cur = 0;
            n = 0;
            while (puddle_json_array_next(arr, &cur, &it) == 1) n++;
            CHECK(n == atoi(cases[i][1]));
            CHECK(puddle_json_array_next(arr, &cur, &it) == 0);                    /* and it stays ended */
        }
        arr.p = " [ 1 , \"a,b]\" , {\"x\":[1,2]} ] ";
        arr.len = strlen(arr.p);
        cur = 0;
        CHECK(puddle_json_array_next(arr, &cur, &it) == 1 && it.len == 1 && it.p[0] == '1');
        CHECK(puddle_json_array_next(arr, &cur, &it) == 1 && it.len == 6 && !memcmp(it.p, "\"a,b]\"", 6));
        CHECK(puddle_json_array_next(arr, &cur, &it) == 1 && it.len == 11 && !memcmp(it.p, "{\"x\":[1,2]}", 11));
        CHECK(puddle_json_array_next(arr, &cur, &it) == 0);
        const char *bad[] = { "", "{}", "1", "[", "[1", "[1,", "[1,]", "[,1]", "[1 2]", "[1,,2]", "[\"a]", "[{]", "[1}", "]", "[01]" };
        for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
            arr.p = bad[i];
            arr.len = strlen(arr.p);
            cur = 0;
            int r, steps = 0, ended_ok = 1;
            while ((r = puddle_json_array_next(arr, &cur, &it)) == 1 && steps++ < 10) ;
            if (r != -1) ended_ok = 0;
            if (!ended_ok) { fprintf(stderr, "FAIL array %s was accepted\n", bad[i]); failures++; }
        }
        /* an array inside an object, found the usual way */
        const char *obj = "{\"events\":[{\"a\":1},{\"a\":2}],\"n\":3}";
        CHECK(puddle_json_member(obj, strlen(obj), "events", &arr) == 0);
        cur = 0;
        n = 0;
        while (puddle_json_array_next(arr, &cur, &it) == 1) {
            puddle_span a;
            int64_t v;
            CHECK(puddle_json_member(it.p, it.len, "a", &a) == 0 && puddle_json_int64(a, &v) == 0 && v == n + 1);
            n++;
        }
        CHECK(n == 2);
    }

    /* ---- writer ---- */
    char out[256];
    puddle_jw w;
    size_t n_out;

    puddle_jw_init(&w, out, sizeof out);
    puddle_jw_object(&w);
    puddle_jw_str(&w, "name", "Al \"the\" \\ice\n\t\x01");
    puddle_jw_num(&w, "n", -7);
    puddle_jw_flag(&w, "ok", 1);
    puddle_jw_key(&w, "list");
    puddle_jw_array(&w);
    puddle_jw_int(&w, 1);
    puddle_jw_null(&w);
    puddle_jw_object(&w);
    puddle_jw_end_object(&w);
    puddle_jw_array(&w);
    puddle_jw_end_array(&w);
    puddle_jw_end_array(&w);
    puddle_jw_key(&w, "raw");
    puddle_jw_raw(&w, "{\"a\":1}", 7);
    puddle_jw_key(&w, "u");
    puddle_jw_uint(&w, UINT64_MAX);
    puddle_jw_str(&w, "none", NULL);
    puddle_jw_end_object(&w);
    n_out = puddle_jw_finish(&w);
    CHECK(n_out == strlen(out) && n_out > 0);
    CHECK(strcmp(out, "{\"name\":\"Al \\\"the\\\" \\\\ice\\n\\t\\u0001\",\"n\":-7,\"ok\":true,\"list\":[1,null,{},[]],"
                      "\"raw\":{\"a\":1},\"u\":18446744073709551615,\"none\":null}") == 0);
    CHECK(valid(out));
    CHECK(member(out, "name", &v) == 0 && puddle_json_string(v, buf, sizeof buf) == 0 && strcmp(buf, "Al \"the\" \\ice\n\t\x01") == 0);

    /* a bare value is fine, and so is the empty object */
    puddle_jw_init(&w, out, sizeof out);
    puddle_jw_string(&w, "x");
    CHECK(puddle_jw_finish(&w) == 3 && strcmp(out, "\"x\"") == 0);
    puddle_jw_init(&w, out, sizeof out);
    puddle_jw_object(&w);
    puddle_jw_end_object(&w);
    CHECK(puddle_jw_finish(&w) == 2 && strcmp(out, "{}") == 0);

    /* every misuse is reported by finish, and none writes outside the buffer */
    puddle_jw_init(&w, out, sizeof out); puddle_jw_object(&w); puddle_jw_int(&w, 1); puddle_jw_end_object(&w);
    CHECK(puddle_jw_finish(&w) == 0); /* value with no key */
    puddle_jw_init(&w, out, sizeof out); puddle_jw_array(&w); puddle_jw_key(&w, "k"); puddle_jw_end_array(&w);
    CHECK(puddle_jw_finish(&w) == 0); /* key in an array */
    puddle_jw_init(&w, out, sizeof out); puddle_jw_object(&w); puddle_jw_key(&w, "a"); puddle_jw_key(&w, "b");
    CHECK(puddle_jw_finish(&w) == 0); /* two keys in a row */
    puddle_jw_init(&w, out, sizeof out); puddle_jw_object(&w); puddle_jw_key(&w, "a"); puddle_jw_end_object(&w);
    CHECK(puddle_jw_finish(&w) == 0); /* key with no value */
    puddle_jw_init(&w, out, sizeof out); puddle_jw_object(&w); puddle_jw_end_array(&w);
    CHECK(puddle_jw_finish(&w) == 0); /* wrong closer */
    puddle_jw_init(&w, out, sizeof out); puddle_jw_end_object(&w);
    CHECK(puddle_jw_finish(&w) == 0); /* closer with nothing open */
    puddle_jw_init(&w, out, sizeof out); puddle_jw_object(&w);
    CHECK(puddle_jw_finish(&w) == 0); /* left open */
    puddle_jw_init(&w, out, sizeof out);
    CHECK(puddle_jw_finish(&w) == 0); /* nothing written */
    puddle_jw_init(&w, out, sizeof out); puddle_jw_int(&w, 1); puddle_jw_int(&w, 2);
    CHECK(puddle_jw_finish(&w) == 0); /* two top-level values */
    puddle_jw_init(&w, out, sizeof out); puddle_jw_raw(&w, "", 0);
    CHECK(puddle_jw_finish(&w) == 0);
    puddle_jw_init(&w, out, 0); puddle_jw_int(&w, 1);
    CHECK(puddle_jw_finish(&w) == 0);
    puddle_jw_init(&w, out, sizeof out);
    for (int i = 0; i < 64; i++) puddle_jw_array(&w);
    puddle_jw_array(&w); /* the 65th */
    CHECK(w.err);
    puddle_jw_init(&w, out, sizeof out);
    for (int i = 0; i < 64; i++) puddle_jw_array(&w);
    for (int i = 0; i < 64; i++) puddle_jw_end_array(&w);
    CHECK(puddle_jw_finish(&w) == 128);

    /* a full buffer: exactly fitting works, one byte short fails, and nothing is written past the end */
    for (size_t cap = 1; cap <= 14; cap++) {
        char small[16];
        memset(small, 'Z', sizeof small);
        puddle_jw_init(&w, small, cap);
        puddle_jw_object(&w); puddle_jw_str(&w, "ab", "cd"); puddle_jw_end_object(&w);
        size_t r = puddle_jw_finish(&w); /* {"ab":"cd"} is 11 bytes, so 12 with the NUL */
        CHECK((r == 11) == (cap >= 12));
        for (size_t i = cap; i < sizeof small; i++) CHECK(small[i] == 'Z');
    }
    {
        char small[12];
        puddle_jw_init(&w, small, sizeof small);
        puddle_jw_object(&w); puddle_jw_str(&w, "ab", "cd"); puddle_jw_end_object(&w);
        CHECK(puddle_jw_finish(&w) == 11 && strcmp(small, "{\"ab\":\"cd\"}") == 0);
    }

    /* UTF-8 passes through; anything that is not valid becomes U+FFFD, so the output is always valid */
    struct { const char *in; const char *want; } u8[] = {
        {"h\xC3\xA9llo \xE2\x82\xAC \xF0\x9F\x99\x82", "\"h\xC3\xA9llo \xE2\x82\xAC \xF0\x9F\x99\x82\""},
        {"a\xFF" "b", "\"a\xEF\xBF\xBD" "b\""},
        {"\xC0\xAF", "\"\xEF\xBF\xBD\xEF\xBF\xBD\""},              /* overlong '/' */
        {"\xE0\x80\xAF", "\"\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\""},
        {"\xED\xA0\x80", "\"\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\""}, /* a surrogate */
        {"\xF4\x90\x80\x80", "\"\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD\""}, /* above U+10FFFF */
        {"\xE2\x82", "\"\xEF\xBF\xBD\xEF\xBF\xBD\""},              /* cut short */
        {"\x7f", "\"\x7f\""},
    };
    for (size_t i = 0; i < sizeof u8 / sizeof u8[0]; i++) {
        puddle_jw_init(&w, out, sizeof out);
        puddle_jw_string(&w, u8[i].in);
        CHECK(puddle_jw_finish(&w) > 0 && strcmp(out, u8[i].want) == 0);
    }
    /* every control character is escaped, including the last one before space */
    puddle_jw_init(&w, out, sizeof out);
    puddle_jw_string(&w, "\b\f\x1f ");
    CHECK(puddle_jw_finish(&w) > 0 && strcmp(out, "\"\\b\\f\\u001f \"") == 0);
    /* embedded NULs are escaped, not treated as the end */
    puddle_jw_init(&w, out, sizeof out);
    puddle_jw_string_n(&w, "a\0b", 3);
    CHECK(puddle_jw_finish(&w) == 10 && strcmp(out, "\"a\\u0000b\"") == 0);

    /* whatever bytes go in, a valid object comes out and the reader gets the same text back for valid input */
    srand(7);
    for (int round = 0; round < 3000; round++) {
        char junk[40];
        size_t jl = (size_t)(rand() % 30);
        for (size_t i = 0; i < jl; i++) junk[i] = (char)(rand() % 4 ? rand() % 128 : rand() % 256);
        char big[512];
        puddle_jw_init(&w, big, sizeof big);
        puddle_jw_object(&w);
        puddle_jw_key(&w, "k");
        puddle_jw_string_n(&w, junk, jl);
        puddle_jw_end_object(&w);
        size_t r = puddle_jw_finish(&w);
        CHECK(r > 0 && valid(big));
        char back[256];
        if (member(big, "k", &v) == 0 && puddle_json_string(v, back, sizeof back) == 0) {
            int has_nul_or_bad = 0;
            for (size_t i = 0; i < jl; i++) if (junk[i] == 0 || (unsigned char)junk[i] >= 0x80) has_nul_or_bad = 1;
            if (!has_nul_or_bad) CHECK(strlen(back) == jl && memcmp(back, junk, jl) == 0);
        }
    }

    return failures ? 1 : 0;
}
