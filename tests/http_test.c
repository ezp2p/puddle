#include "http/http.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static int parse(const char *raw, puddle_request *req) { return puddle_http_parse(raw, strlen(raw), req); }

int main(void)
{
    puddle_request req;
    char out[2048];

    /* responses */
    size_t n = puddle_http_response(200, "X-A: b\r\n", "{\"a\":1}", 7, out, sizeof out);
    CHECK(n > 0 && strstr(out, "HTTP/1.1 200 OK\r\n") && strstr(out, "Content-Length: 7\r\n") && strstr(out, "X-A: b\r\n") &&
          memcmp(out + n - 7, "{\"a\":1}", 7) == 0);
    CHECK(puddle_http_response(200, "", "abc", 3, out, 10) == 0); /* does not fit */
    n = puddle_http_response(200, "", "a\0b", 3, out, sizeof out);
    CHECK(n > 0 && memcmp(out + n - 3, "a\0b", 3) == 0 && strstr(out, "Content-Length: 3\r\n"));
    puddle_http_error(404, out, sizeof out);
    CHECK(strstr(out, "404 Not Found") && strstr(out, "{\"error\":\"Not Found\"}"));
    {
        int codes[] = {202, 401, 503, 413, 408, 431, 400, 405};
        for (size_t i = 0; i < sizeof codes / sizeof codes[0]; i++) {
            puddle_http_response(codes[i], "", "", 0, out, sizeof out);
            CHECK(!strstr(out, "Internal Server Error"));
        }
    }

    CHECK(parse("garbage\r\n\r\n", &req) == -1);
    CHECK(parse("GET /health SPDY/3\r\n\r\n", &req) == -1);

    /* the two headers we read */
    CHECK(parse("GET /a/b?x=1&y=2 HTTP/1.1\r\nHost: h\r\nAuthorization: Bearer abc\r\nx-puddle-signature:  v1 id=z  \r\n\r\n", &req) == 0);
    CHECK(strcmp(req.path, "/a/b") == 0 && strcmp(req.target, "/a/b?x=1&y=2") == 0);
    CHECK(strcmp(req.authorization, "Bearer abc") == 0); /* name matching ignores case; the value is trimmed */
    CHECK(strcmp(req.signature, "v1 id=z") == 0);
    CHECK(parse("GET /a#frag HTTP/1.1\r\n\r\n", &req) == 0 && strcmp(req.target, "/a") == 0);
    CHECK(parse("GET / HTTP/1.1\r\n\r\n", &req) == 0 && req.authorization[0] == '\0' && req.signature[0] == '\0');

    /* things that could make two parsers disagree are refused */
    CHECK(parse("GET / HTTP/1.1\r\nAuthorization: a\r\nAuthorization: b\r\n\r\n", &req) == -1);
    CHECK(parse("GET / HTTP/1.1\r\nX-Puddle-Signature: a\r\nx-puddle-signature: b\r\n\r\n", &req) == -1);
    CHECK(parse("GET / HTTP/1.1\r\nAuthorization: a\r\n  folded\r\n\r\n", &req) == -1);
    CHECK(parse("GET / HTTP/1.1\r\nno colon here\r\n\r\n", &req) == -1);
    char big[1500];
    memset(big, 'x', 700);
    big[700] = '\0';
    char raw[1800];
    snprintf(raw, sizeof raw, "GET / HTTP/1.1\r\nAuthorization: %s\r\n\r\n", big);
    CHECK(parse(raw, &req) == -1); /* longer than we keep */
    snprintf(raw, sizeof raw, "GET / HTTP/1.1\r\nX-Other: %s\r\n\r\n", big); /* other headers may be long */
    CHECK(parse(raw, &req) == 0);

    /* body headers */
    CHECK(parse("POST /x HTTP/1.1\r\nContent-Length: 42\r\n\r\n", &req) == 0 && req.content_length == 42 && !req.expect_continue);
    CHECK(parse("POST /x HTTP/1.1\r\ncontent-length:  7 \r\nEXPECT: 100-Continue\r\n\r\n", &req) == 0 && req.content_length == 7 && req.expect_continue);
    CHECK(parse("GET /x HTTP/1.1\r\n\r\n", &req) == 0 && req.content_length == 0 && !req.expect_continue);
    CHECK(parse("POST /x HTTP/1.1\r\nContent-Length: 4294967295\r\n\r\n", &req) == 0 && req.content_length == 4294967295u);
    CHECK(parse("POST /x HTTP/1.1\r\nContent-Length: 4294967296\r\n\r\n", &req) == -1);
    CHECK(parse("POST /x HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\n", &req) == -1);
    CHECK(parse("POST /x HTTP/1.1\r\nContent-Length: 1_0\r\n\r\n", &req) == -1);
    CHECK(parse("POST /x HTTP/1.1\r\nTransfer-Encoding: identity\r\n\r\n", &req) == -1);
    CHECK(parse("POST /x HTTP/1.1\r\nExpect: 100-continue\r\nExpect: 100-continue\r\n\r\n", &req) == -1);

    return failures ? 1 : 0;
}
