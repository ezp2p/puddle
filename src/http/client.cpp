#include "http/client.h"

#include "platform/clock.h"
#include "base/hash.h"
#include "http/reqsig.h"
#include "platform/net.h"
#include "base/text.h"

#include <ctype.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void why(puddle_net_reply *r, puddle_net_result res, const char *fmt, ...)
{
    r->result = res;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->reason, sizeof r->reason, fmt, ap);
    va_end(ap);
}

static int64_t mono_ms(void)
{
    return puddle_mono_ms();
}

/* ---- which addresses we will not dial ---- */

static int v4_dialable(const unsigned char a[4])
{
    if (a[0] == 0) return 0;                          /* 0.0.0.0/8: "this host" */
    if (a[0] >= 224) return 0;                        /* multicast, reserved, broadcast */
    if (a[0] == 169 && a[1] == 254) return 0;         /* link-local, where cloud metadata services live */
    return 1;
}

static int ip_dialable(const puddle_ip *ip)
{
    if (!ip->v6)
        return v4_dialable(ip->b);
    const unsigned char *a = ip->b;
    static const unsigned char zero[16] = { 0 };
    if (!memcmp(a, zero, 16)) return 0;                                  /* :: */
    if (a[0] == 0xff) return 0;                                          /* multicast */
    if (a[0] == 0xfe && (a[1] & 0xC0) == 0x80) return 0;                 /* fe80::/10 */
    static const unsigned char mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff };
    if (!memcmp(a, mapped, 12)) return v4_dialable(a + 12);              /* ::ffff:a.b.c.d */
    return 1;
}

static int all_dialable(const char *host, int numeric_only)
{
    puddle_ip ips[16];
    int n = puddle_resolve(host, numeric_only, ips, 16, NULL, 0);
    for (int i = 0; i < n; i++)
        if (!ip_dialable(&ips[i]))
            return 0;
    return 1; /* not resolvable is not the same as not allowed: dialling will say so; a name is not a numeric address */
}

int puddle_net_host_dialable(const char *host)
{
    return all_dialable(host, 0);
}

int puddle_net_host_plausible(const char *host)
{
    return all_dialable(host, 1);
}

/* ---- connecting ---- */

/* a connected blocking socket, or PUDDLE_SOCK_NONE with the result and a reason in `r` */
static puddle_sock dial(const puddle_address *addr, int connect_s, int io_s, int64_t deadline, puddle_net_reply *r)
{
    puddle_sock s;
    char detail[200];
    switch (puddle_sock_dial(addr->host, addr->port, ip_dialable, connect_s * 1000, deadline, io_s, &s, detail, sizeof detail)) {
    case PUDDLE_DIAL_OK:
        return s;
    case PUDDLE_DIAL_UNKNOWN_HOST:
        why(r, PUDDLE_NET_OFFLINE, "cannot find %s: %s", addr->host, detail);
        break;
    case PUDDLE_DIAL_NOT_ALLOWED:
        why(r, PUDDLE_NET_FAILED, "%s is not an address we will connect to", addr->address);
        break;
    case PUDDLE_DIAL_FAILED:
        why(r, PUDDLE_NET_FAILED, "cannot connect to %s: %s", addr->address, detail);
        break;
    case PUDDLE_DIAL_OFFLINE:
        if (detail[0])
            why(r, PUDDLE_NET_OFFLINE, "cannot reach %s: %s", addr->address, detail);
        else
            why(r, PUDDLE_NET_OFFLINE, "cannot reach %s", addr->address);
        break;
    }
    return PUDDLE_SOCK_NONE;
}

/* ---- the call ---- */

static int ssl_send_all(SSL *ssl, const char *p, size_t n)
{
    while (n) {
        int w = SSL_write(ssl, p, (int)n);
        if (w <= 0)
            return -1;
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

void puddle_net_request(const puddle_address *addr, const char *method, const char *target, const void *body,
                        size_t body_len, puddle_identity *me, const char *peer_id, const puddle_net_opts *opts,
                        puddle_net_reply *out)
{
    memset(out, 0, sizeof *out);
    puddle_net_opts o = opts ? *opts : puddle_net_opts{};
    int connect_s = o.connect_timeout_s > 0 ? o.connect_timeout_s : 8;
    int io_s = o.io_timeout_s > 0 ? o.io_timeout_s : 10;
    int total_s = o.total_timeout_s > 0 ? o.total_timeout_s : 30;
    int64_t deadline = mono_ms() + total_s * 1000LL;

    char sig[800];
    if (puddle_reqsig_sign(me, method, target, body, body_len, peer_id, 0, sig, sizeof sig) != 0) {
        why(out, PUDDLE_NET_FAILED, "cannot sign the request");
        return;
    }

    puddle_sock sock = dial(addr, connect_s, io_s, deadline, out);
    if (sock == PUDDLE_SOCK_NONE)
        return;

    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    SSL *ssl = NULL;
    char *raw = NULL;
    if (!ctx) {
        why(out, PUDDLE_NET_FAILED, "cannot start TLS");
        goto done;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL); /* see the top of client.h: the signature is what authenticates */
    ssl = SSL_new(ctx);
    if (!ssl || SSL_set_fd(ssl, puddle_sock_fd(sock)) != 1) {
        why(out, PUDDLE_NET_FAILED, "cannot start TLS");
        goto done;
    }
    if (SSL_connect(ssl) != 1) {
        unsigned long e = ERR_get_error();
        char eb[120] = "";
        if (e)
            ERR_error_string_n(e, eb, sizeof eb);
        why(out, PUDDLE_NET_FAILED, "%s did not complete a TLS handshake%s%s", addr->address, e ? ": " : "", eb);
        goto done;
    }

    X509 *cert; cert = SSL_get1_peer_certificate(ssl);
    if (!cert) {
        why(out, PUDDLE_NET_FAILED, "%s presented no certificate", addr->address);
        goto done;
    }
    unsigned char md[32];
    unsigned mdlen; mdlen = 0;
    int dok; dok = X509_digest(cert, EVP_sha256(), md, &mdlen) == 1 && mdlen == 32;
    X509_free(cert);
    if (!dok) {
        why(out, PUDDLE_NET_FAILED, "cannot read the certificate");
        goto done;
    }
    puddle_hex_encode(md, 32, out->fingerprint);
    if (o.fingerprint_ok && !o.fingerprint_ok(o.user, out->fingerprint)) {
        why(out, PUDDLE_NET_CERT_CHANGED, "%s has a different certificate from before", addr->address);
        goto done;
    }

    {
        char head[1400];
        int hn = snprintf(head, sizeof head,
                          "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: puddle\r\nX-Puddle-Signature: %s\r\n"
                          "Content-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                          method, target, addr->address, sig, body ? body_len : 0);
        if (hn <= 0 || (size_t)hn >= sizeof head || ssl_send_all(ssl, head, (size_t)hn) != 0 ||
            (body && body_len && ssl_send_all(ssl, (const char *)body, body_len) != 0)) {
            why(out, PUDDLE_NET_FAILED, "could not send the request to %s", addr->address);
            goto done;
        }
    }

    /* the answer: headers, then a body of the length it promises, or until the other side closes */
    size_t max_body; max_body = o.max_body ? o.max_body : PUDDLE_NET_BODY_MAX;
    size_t cap, len; cap = max_body + 8192; len = 0;
    raw = (char *)malloc(cap + 1);
    if (!raw) {
        why(out, PUDDLE_NET_FAILED, "out of memory");
        goto done;
    }
    size_t hend; hend = 0;
    long want; want = -1;
    for (;;) {
        if (mono_ms() > deadline) {
            why(out, PUDDLE_NET_FAILED, "%s took too long to answer", addr->address);
            goto done;
        }
        if (hend && want >= 0 && len >= hend + (size_t)want)
            break;
        if (len >= cap) {
            why(out, PUDDLE_NET_FAILED, "the answer from %s was too large", addr->address);
            goto done;
        }
        int n = SSL_read(ssl, raw + len, (int)(cap - len));
        if (n <= 0) {
            int se = SSL_get_error(ssl, n);
            if (se == SSL_ERROR_ZERO_RETURN || (se == SSL_ERROR_SYSCALL && n == 0) || (hend && want < 0))
                break; /* closed: that is the end of the body */
            why(out, PUDDLE_NET_FAILED, "%s stopped answering", addr->address);
            goto done;
        }
        len += (size_t)n;
        raw[len] = '\0';
        if (!hend) {
            char *e = strstr(raw, "\r\n\r\n");
            if (e) {
                hend = (size_t)(e - raw) + 4;
                const char *cl = NULL;
                for (const char *l = strchr(raw, '\n'); l && l < raw + hend; l = strchr(l + 1, '\n'))
                    if (!puddle_ascii_ncasecmp(l + 1, "content-length:", 15))
                        cl = l + 16;
                for (const char *l = strchr(raw, '\n'); l && l < raw + hend; l = strchr(l + 1, '\n'))
                    if (!puddle_ascii_ncasecmp(l + 1, "content-type:", 13)) {
                        const char *v = l + 14;
                        while (*v == ' ' || *v == '\t')
                            v++;
                        size_t k = 0;
                        while (v < raw + hend && *v != ';' && *v != '\r' && *v != '\n' && *v != ' ' && k + 1 < sizeof out->content_type)
                            out->content_type[k++] = (char)tolower((unsigned char)*v++);
                        out->content_type[k] = '\0';
                    }
                if (cl) {
                    char *endp;
                    long v = strtol(cl, &endp, 10);
                    if (v < 0 || (unsigned long)v > max_body) {
                        why(out, PUDDLE_NET_FAILED, "the answer from %s was too large", addr->address);
                        goto done;
                    }
                    want = v;
                } else if (!strncmp(raw, "HTTP/1.1 204", 12) || !strncmp(raw, "HTTP/1.1 202", 12)) {
                    want = 0;
                }
            }
        }
    }
    raw[len] = '\0';
    int status; status = 0;
    if (!hend || sscanf(raw, "HTTP/1.%*d %d", &status) != 1 || status < 100 || status > 599) {
        why(out, PUDDLE_NET_FAILED, "%s did not answer with HTTP", addr->address);
        goto done;
    }
    out->result = PUDDLE_NET_OK;
    out->status = status;
    size_t bl; bl = len - hend;
    if (want >= 0 && (size_t)want < bl)
        bl = (size_t)want;
    if (bl > max_body) {
        why(out, PUDDLE_NET_FAILED, "the answer from %s was too large", addr->address);
        goto done;
    }
    out->body = (char *)malloc(bl + 1);
    if (out->body) {
        memcpy(out->body, raw + hend, bl);
        out->body[bl] = '\0';
        out->body_len = bl;
    }
done:
    if (ssl) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
    }
    SSL_CTX_free(ctx);
    free(raw);
    puddle_sock_close(sock);
}
