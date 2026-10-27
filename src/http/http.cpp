#include "http/http.h"
#include "base/text.h"

#include <stdio.h>
#include <string.h>

static const char *reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 201: return "Created";
    case 409: return "Conflict";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 413: return "Payload Too Large";
    case 503: return "Service Unavailable";
    case 431: return "Request Header Fields Too Large";
    default:  return "Internal Server Error";
    }
}

size_t puddle_http_response(int status, const char *extra_headers, const char *body, size_t blen, char *out,
                            size_t cap)
{
    if (status == 204) { /* by the rules of HTTP, no length and no type */
        int m = snprintf(out, cap, "HTTP/1.1 204 No Content\r\nCache-Control: no-store\r\nConnection: close\r\n%s\r\n",
                         extra_headers ? extra_headers : "");
        return m > 0 && (size_t)m < cap ? (size_t)m : 0;
    }
    int n = snprintf(out, cap,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %zu\r\n"
                     "Cache-Control: no-store\r\n"
                     "Connection: close\r\n"
                     "%s"
                     "\r\n",
                     status, reason(status), blen, extra_headers ? extra_headers : "");
    if (n < 0 || (size_t)n >= cap || blen >= cap - (size_t)n)
        return 0;
    memcpy(out + n, body, blen); /* by length: the body is ours, but nothing here relies on it being text */
    return (size_t)n + blen;
}

size_t puddle_http_head(int status, const char *content_type, const char *extra_headers, size_t content_length, char *out, size_t cap)
{
    int n = snprintf(out, cap,
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "%s"
                     "\r\n",
                     status, reason(status), content_type, content_length, extra_headers ? extra_headers : "");
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

size_t puddle_http_error(int status, char *out, size_t cap)
{
    char body[96];
    snprintf(body, sizeof body, "{\"error\":\"%s\"}\n", reason(status));
    return puddle_http_response(status, "", body, strlen(body), out, cap);
}

int puddle_http_parse(const char *buf, size_t len, puddle_request *req)
{
    const char *eol = (const char *)memchr(buf, '\n', len);
    if (!eol)
        return -1;
    size_t n = (size_t)(eol - buf);
    if (n && buf[n - 1] == '\r')
        n--;

    char line[1100];
    if (n >= sizeof line)
        return -1;
    memcpy(line, buf, n);
    line[n] = '\0';

    char method[8], target[1024], version[16];
    if (sscanf(line, "%7s %1023s %15s", method, target, version) != 3)
        return -1;
    if (strncmp(version, "HTTP/1.", 7) != 0)
        return -1;

    memset(req, 0, sizeof *req);
    snprintf(req->target, sizeof req->target, "%s", target);
    char *frag = strchr(target, '#');
    if (frag) {
        *frag = '\0';
        char *h = strchr(req->target, '#');
        if (h)
            *h = '\0'; /* a fragment is never sent in a request target; drop it from what is signed too */
    }
    char *q = strchr(target, '?');
    if (q)
        *q = '\0';

    memcpy(req->method, method, sizeof req->method);
    memcpy(req->path, target, sizeof req->path);

    /* headers: only Authorization and X-Puddle-Signature matter */
    const char *p = eol + 1;
    const char *end = buf + len;
    int seen_auth = 0, seen_sig = 0, seen_len = 0, seen_expect = 0;
    while (p < end) {
        const char *le = (const char *)memchr(p, '\n', (size_t)(end - p));
        if (!le)
            break;
        size_t ll = (size_t)(le - p);
        if (ll && p[ll - 1] == '\r')
            ll--;
        if (ll == 0)
            break; /* blank line: end of headers */
        if (*p == ' ' || *p == '\t')
            return -1; /* folded headers are not accepted */
        const char *colon = (const char *)memchr(p, ':', ll);
        if (!colon)
            return -1;
        size_t nl = (size_t)(colon - p);
        const char *v = colon + 1;
        size_t vl = ll - nl - 1;
        while (vl && (*v == ' ' || *v == '\t')) { v++; vl--; }
        while (vl && (v[vl - 1] == ' ' || v[vl - 1] == '\t')) vl--;

        char *dst = NULL;
        size_t cap = 0;
        int *seen = NULL;
        if (nl == 13 && puddle_ascii_ncasecmp(p, "Authorization", 13) == 0) {
            dst = req->authorization; cap = sizeof req->authorization; seen = &seen_auth;
        } else if (nl == 18 && puddle_ascii_ncasecmp(p, "X-Puddle-Signature", 18) == 0) {
            dst = req->signature; cap = sizeof req->signature; seen = &seen_sig;
        }
        if (nl == 14 && puddle_ascii_ncasecmp(p, "Content-Length", 14) == 0) {
            if (seen_len || vl == 0 || vl > 10)
                return -1;
            unsigned long long n = 0;
            for (size_t i = 0; i < vl; i++) {
                if (v[i] < '0' || v[i] > '9')
                    return -1; /* no sign, no spaces, no hex: plain digits only */
                n = n * 10 + (unsigned)(v[i] - '0');
            }
            if (n > 0xFFFFFFFFULL)
                return -1;
            seen_len = 1;
            req->content_length = (size_t)n;
        } else if (nl == 17 && puddle_ascii_ncasecmp(p, "Transfer-Encoding", 17) == 0) {
            return -1;
        } else if (nl == 6 && puddle_ascii_ncasecmp(p, "Expect", 6) == 0) {
            if (seen_expect || vl != 12 || puddle_ascii_ncasecmp(v, "100-continue", 12) != 0)
                return -1;
            seen_expect = 1;
            req->expect_continue = 1;
        }
        if (dst) {
            if (*seen || vl >= cap)
                return -1;
            *seen = 1;
            memcpy(dst, v, vl);
            dst[vl] = '\0';
        }
        p = le + 1;
    }
    return 0;
}
