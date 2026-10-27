#ifndef PUDDLE_HTTP_H
#define PUDDLE_HTTP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char method[8];
    char path[1024];    /* query string and fragment stripped */
    char target[1100];  /* path and query exactly as sent (what request signatures cover) */
    char authorization[512];   /* "" if absent */
    char signature[1024];      /* X-Puddle-Signature, "" if absent */
    size_t content_length;     /* 0 if absent */
    int expect_continue;       /* the client asked "100-continue" before sending its body */
} puddle_request;

/* Parse the request line and the headers we use of a (complete) header block. Anything that could make two
 * parsers disagree about where a request ends is malformed: a repeated Authorization, X-Puddle-Signature or
 * Content-Length, a Content-Length that is not plain digits, any Transfer-Encoding (chunked bodies are not
 * supported), or an Expect other than 100-continue. 0 = ok, -1 = malformed. */
int puddle_http_parse(const char *buf, size_t len, puddle_request *req);

/* A body too big for the response buffer: malloc'd, sent after the headers, then freed by whoever sends it. */
typedef struct {
    unsigned char *data;
    size_t len;
} puddle_big;

/* The status line and headers for a body of `content_length` bytes of `content_type`, ending with the blank line (the body
 * itself is sent separately). `extra_headers` is "" or lines each ending \r\n. Returns the length, or 0 if it does not fit. */
size_t puddle_http_head(int status, const char *content_type, const char *extra_headers, size_t content_length, char *out, size_t cap);

/* A full HTTP/1.1 response with a JSON body of `blen` bytes (0 = none). `extra_headers` is "" or header lines
 * each ending \r\n. Returns the byte length, or 0 if it does not fit in `cap`. */
size_t puddle_http_response(int status, const char *extra_headers, const char *body, size_t blen, char *out,
                            size_t cap);

/* Build a bodyless-routing error response (e.g. 408, 431). */
size_t puddle_http_error(int status, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
