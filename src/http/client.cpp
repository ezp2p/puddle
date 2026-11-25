#include "http/client.h"

#include "core/levee.h"
#include "base/log.h"

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

/* An address written out as numbers (SNI may only name a host): IPv6 has a ':', IPv4 is digits and dots. */
static int is_ip(const char *host)
{
    if (strchr(host, ':'))
        return 1;
    for (const char *p = host; *p; p++)
        if (!(*p == '.' || (*p >= '0' && *p <= '9')))
            return 0;
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

/* The value of the header `name` ("x-name:", lower case) in the head [raw, raw+hend), trimmed, into `out`; "" if absent. */
static void header_value(const char *raw, size_t hend, const char *name, char *out, size_t cap)
{
    size_t nl = strlen(name);
    for (const char *l = strchr(raw, '\n'); l && l < raw + hend; l = strchr(l + 1, '\n'))
        if (!puddle_ascii_ncasecmp(l + 1, name, nl)) {
            const char *v = l + 1 + nl;
            while (*v == ' ' || *v == '\t')
                v++;
            size_t k = 0;
            while (v < raw + hend && *v != '\r' && *v != '\n' && k + 1 < cap)
                out[k++] = *v++;
            out[k] = '\0';
            return;
        }
}

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

    puddle_sealed_request sealed;
    memset(&sealed, 0, sizeof sealed);
    char sig[800];
    puddle_reqsig mine;
    if (o.seal_pub) {
        if (puddle_seal_request(me, method, target, body, body_len, peer_id, o.seal_pub, 0, &sealed) != 0) {
            why(out, PUDDLE_NET_FAILED, "cannot seal the request");
            return;
        }
        snprintf(sig, sizeof sig, "%s", sealed.signature);
        body = sealed.body;
        body_len = sealed.body_len;
    } else if (puddle_reqsig_sign(me, method, target, body, body_len, peer_id, 0, sig, sizeof sig) != 0) {
        why(out, PUDDLE_NET_FAILED, "cannot sign the request");
        return;
    }
    if (puddle_reqsig_parse(sig, &mine) != 0) {
        puddle_sealed_request_free(&sealed);
        why(out, PUDDLE_NET_FAILED, "cannot sign the request");
        return;
    }
    char receipt[PUDDLE_RECEIPT_MAX], custody[PP_CUSTODY_MAX], levee_status[8], levee_error[40];
    receipt[0] = custody[0] = levee_status[0] = levee_error[0] = '\0';

    /* behind a Levee, everything goes out through it (spec/06, "Going out"), signed for it on the outside */
    puddle_levee_state lv;
    int via = !o.direct && puddle_levee_current(&lv);
    char outer[PP_REQSIG_MAX];
    if (via && puddle_reqsig_sign(me, "POST", "/levee/v1/send", body, body ? body_len : 0, lv.id, 0, outer, sizeof outer) != 0) {
        puddle_sealed_request_free(&sealed);
        why(out, PUDDLE_NET_FAILED, "cannot sign the request for our Levee");
        return;
    }
    const puddle_address *to = via ? &lv.address : addr;

    puddle_sock sock = dial(to, connect_s, io_s, deadline, out);
    if (sock == PUDDLE_SOCK_NONE) {
        puddle_sealed_request_free(&sealed);
        return;
    }
    if (o.on_socket)
        o.on_socket(o.user, sock);

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
    /* Name the host we want (SNI) when it is a name, not a number: a reverse proxy serving several sites on one address picks the
     * site by it, and refuses one that doesn't match the Host header (Apache's 421). It only routes; nothing is checked. */
    if (!is_ip(to->host) && SSL_set_tlsext_host_name(ssl, to->host) != 1) {
        why(out, PUDDLE_NET_FAILED, "cannot start TLS");
        goto done;
    }
    if (SSL_connect(ssl) != 1) {
        unsigned long e = ERR_get_error();
        char eb[120] = "";
        if (e)
            ERR_error_string_n(e, eb, sizeof eb);
        why(out, PUDDLE_NET_FAILED, "something at %s answered, but it isn't a Puddle (no TLS handshake%s%s)", to->address, e ? ": " : "", eb);
        goto done;
    }

    /* whatever certificate it showed: TLS only carries this, and nothing here depends on which one (spec/09) */

    {
        char head[4000], seal_line[200] = "", levee_lines[1600] = "";
        if (o.seal_pub)
            snprintf(seal_line, sizeof seal_line, "X-Puddle-Seal: %s\r\n", sealed.seal);
        if (via)
            snprintf(levee_lines, sizeof levee_lines,
                     "X-Levee-Signature: %s\r\nX-Levee-To: %s\r\nX-Levee-Method: %s\r\nX-Levee-Target: %s\r\nX-Levee-Timeout: %d\r\n", outer,
                     addr->address, method, target, total_s > 60 ? 60 : total_s);
        int hn = snprintf(head, sizeof head,
                          "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: puddle\r\n%s%s: %s\r\n%s%s"
                          "Content-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                          via ? "POST" : method, via ? "/levee/v1/send" : target, to->address, levee_lines,
                          o.sig_header ? o.sig_header : "X-Puddle-Signature", sig, seal_line, o.extra_headers ? o.extra_headers : "",
                          o.content_type ? o.content_type : o.seal_pub ? PUDDLE_SEALED_TYPE : "application/json", body ? body_len : 0);
        if (hn <= 0 || (size_t)hn >= sizeof head || ssl_send_all(ssl, head, (size_t)hn) != 0 ||
            (body && body_len && ssl_send_all(ssl, (const char *)body, body_len) != 0)) {
            why(out, PUDDLE_NET_FAILED, "could not send the request to %s", addr->address);
            goto done;
        }
    }

    /* the answer: headers, then a body of the length it promises, or until the other side closes */
    size_t max_body; max_body = o.max_body ? o.max_body : PUDDLE_NET_BODY_MAX;
    if (o.seal_pub)
        max_body = puddle_seal_answer_len(max_body); /* sealed, it is a little longer */
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
                header_value(raw, hend, "x-puddle-receipt:", receipt, sizeof receipt);
                header_value(raw, hend, "x-puddle-custody:", custody, sizeof custody);
                if (via) {
                    header_value(raw, hend, "x-levee-status:", levee_status, sizeof levee_status);
                    header_value(raw, hend, "x-levee-error:", levee_error, sizeof levee_error);
                }
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
        why(out, PUDDLE_NET_FAILED, "something at %s answered, but it isn't a Puddle (no HTTP)", addr->address);
        goto done;
    }
    if (via) { /* the Levee's own answer says what theirs was */
        int theirs = atoi(levee_status);
        if (status != 200 || !levee_status[0]) {
            why(out, PUDDLE_NET_OFFLINE, "our Levee at %s couldn't take it (HTTP %d)", lv.address.address, status);
            goto done;
        }
        if (theirs == 0) {
            why(out, PUDDLE_NET_OFFLINE, "cannot reach %s through our Levee (%s)", addr->address, levee_error[0] ? levee_error : "unreachable");
            goto done;
        }
        if (theirs < 100 || theirs > 599) {
            why(out, PUDDLE_NET_FAILED, "our Levee at %s answered oddly", lv.address.address);
            goto done;
        }
        status = theirs;
    }
    out->result = PUDDLE_NET_OK;
    out->status = status;
    puddle_log(PUDDLE_LOG_DEBUG, "%s %s at %s%s: %d%s%s", method, target, addr->address, via ? " (through our Levee)" : "", status,
               custody[0] ? ", with a custody receipt" : "", receipt[0] ? ", signed" : "");
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
    if (o.peer_pub) { /* only their signature makes this an answer from them */
        out->checked = 1;
        out->proof = puddle_receipt_verify(receipt, peer_id, o.peer_pub, mine.nonce, status, raw + hend, bl);
        /* away, and their Levee took it for them: delivered (spec/06), if it is the Levee their address record names */
        if (out->proof == PUDDLE_RECEIPT_MISSING && status == 202 && custody[0] && o.peer_levee_id && o.peer_levee_key) {
            char bh[65];
            pp_sha256_hex(body ? body : "", body ? body_len : 0, bh);
            pp_receipt_check cc = pp_custody_verify(custody, peer_id, o.peer_levee_id, o.peer_levee_key, mine.nonce, bh);
            if (cc == PP_RECEIPT_OK) {
                out->proof = PUDDLE_RECEIPT_OK;
                out->custody = 1;
                snprintf(out->custody_levee, sizeof out->custody_levee, "%s", o.peer_levee_id);
                memcpy(out->custody_key, o.peer_levee_key, 32);
                snprintf(out->custody_nonce, sizeof out->custody_nonce, "%s", mine.nonce);
                memcpy(out->custody_hash, bh, sizeof out->custody_hash);
            } else {
                puddle_log(PUDDLE_LOG_WARN, "a custody receipt from %s didn't check out (%d), so it isn't the Levee %.12s… names", addr->address, (int)cc,
                           peer_id);
            }
        } else if (status == 202 || custody[0]) {
            puddle_log(PUDDLE_LOG_DEBUG, "202 from %s: custody %s, their Levee %s", addr->address, custody[0] ? "given" : "none",
                       o.peer_levee_id ? "known" : "unknown");
        }
        if (out->proof != PUDDLE_RECEIPT_OK) {
            out->result = PUDDLE_NET_NOT_PROVEN;
            switch (out->proof) {
            case PUDDLE_RECEIPT_MISSING:
                if (status == 404)
                    why(out, PUDDLE_NET_NOT_PROVEN, "the Puddle at %s didn't recognise us: it isn't them, or they no longer have a stream with us",
                        addr->address);
                else
                    why(out, PUDDLE_NET_NOT_PROVEN, "something at %s answered (HTTP %d) without signing for it, so it isn't them", addr->address,
                        status);
                break;
            case PUDDLE_RECEIPT_WRONG_KEY:
                why(out, PUDDLE_NET_NOT_PROVEN, "the Puddle at %s couldn't prove it's them: it signed with a key we don't hold for them",
                    addr->address);
                break;
            default:
                why(out, PUDDLE_NET_NOT_PROVEN, "the Puddle at %s couldn't prove it's them: its signature doesn't check out", addr->address);
                break;
            }
        }
    }
    /* a custody receipt's 202 is the Levee's, with nothing in it to seal (spec/06) */
    if (o.seal_pub && (out->result == PUDDLE_NET_OK || out->result == PUDDLE_NET_NOT_PROVEN) && out->body && status != 204 && !out->custody) {
        /* Opened whether or not it was signed for (an answer to everyone alike, such as access/status, never is; `result` still
         * says so). Something sealed to us that won't open is not an answer at all. A 204 has no body, so nothing to seal. */
        if (!strcmp(out->content_type, PUDDLE_SEALED_TYPE)) {
            size_t plain = puddle_seal_plain_len(out->body_len), got = 0;
            unsigned char *pt = (unsigned char *)malloc(plain + 1);
            if (!pt || puddle_open_answer(sealed.answer_key, status, (const unsigned char *)out->body, out->body_len, pt, &got) != 0) {
                free(pt);
                free(out->body);
                out->body = NULL;
                out->body_len = 0;
                why(out, PUDDLE_NET_FAILED, "the answer from %s was sealed, but not for us", addr->address);
                goto done;
            }
            pt[got] = '\0';
            free(out->body);
            out->body = (char *)pt;
            out->body_len = got;
            out->opened = 1;
            out->content_type[0] = '\0'; /* what is inside is what the route always answers */
        } else if (status < 400 && out->result == PUDDLE_NET_OK) {
            /* refusals that can't be sealed (404 to a stranger, a seal that wouldn't open) come plain; nothing else may */
            why(out, PUDDLE_NET_FAILED, "%s answered without sealing it (HTTP %d, %s)", addr->address, status, out->content_type);
            goto done;
        }
    }
done:
    puddle_sealed_request_free(&sealed);
    if (ssl) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
    }
    SSL_CTX_free(ctx);
    free(raw);
    if (o.on_socket)
        o.on_socket(o.user, PUDDLE_SOCK_NONE);
    puddle_sock_close(sock);
}

/* ---- the owner, at a terminal on this computer ---- */

void puddle_owner_request(const puddle_config *cfg, const char *method, const char *target, const char *body, puddle_net_reply *out)
{
    memset(out, 0, sizeof *out);
    char token[256] = "", path[PUDDLE_PATH_MAX + 32];
    snprintf(path, sizeof path, "%s/state/owner-token", cfg->data_dir);
    FILE *f = fopen(path, "rb");
    if (!f) {
        why(out, PUDDLE_NET_FAILED, "cannot read %s: is this Puddle set up and running, and are you its user?", path);
        return;
    }
    size_t tn = fread(token, 1, sizeof token - 1, f);
    fclose(f);
    while (tn && (token[tn - 1] == '\n' || token[tn - 1] == '\r' || token[tn - 1] == ' '))
        tn--;
    token[tn] = '\0';

    /* "every interface" is reached on loopback */
    const char *host = !strcmp(cfg->address, "0.0.0.0") || !cfg->address[0] ? "127.0.0.1" : !strcmp(cfg->address, "::") ? "::1" : cfg->address;
    puddle_address addr;
    memset(&addr, 0, sizeof addr);
    snprintf(addr.host, sizeof addr.host, "%s", host);
    addr.port = cfg->port;
    snprintf(addr.address, sizeof addr.address, strchr(host, ':') ? "[%s]:%u" : "%s:%u", host, (unsigned)cfg->port);
    int64_t deadline = mono_ms() + 15000;
    puddle_sock sock = dial(&addr, 5, 10, deadline, out);
    if (sock == PUDDLE_SOCK_NONE) {
        char detail[sizeof out->reason];
        snprintf(detail, sizeof detail, "%s", out->reason);
        why(out, PUDDLE_NET_OFFLINE, "this Puddle isn't running at %s (%s)", addr.address, detail);
        return;
    }
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    SSL *ssl = ctx ? SSL_new(ctx) : NULL;
    char *raw = NULL;
    size_t len = 0, cap = PUDDLE_NET_BODY_MAX + 8192;
    if (!ssl || SSL_set_fd(ssl, puddle_sock_fd(sock)) != 1 || SSL_connect(ssl) != 1) {
        why(out, PUDDLE_NET_FAILED, "cannot start TLS with %s", addr.address);
        goto done;
    }
    {
        char head[1024];
        size_t bl = body ? strlen(body) : 0;
        int hn = snprintf(head, sizeof head,
                          "%s %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: puddled\r\nAuthorization: Bearer %s\r\n"
                          "Content-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                          method, target, addr.address, token, bl);
        if (hn <= 0 || (size_t)hn >= sizeof head || ssl_send_all(ssl, head, (size_t)hn) != 0 || (bl && ssl_send_all(ssl, body, bl) != 0)) {
            why(out, PUDDLE_NET_FAILED, "could not send the request to %s", addr.address);
            goto done;
        }
    }
    raw = (char *)malloc(cap + 1);
    while (raw && len < cap && mono_ms() < deadline) {
        int n = SSL_read(ssl, raw + len, (int)(cap - len));
        if (n <= 0)
            break; /* Connection: close: the end of the answer */
        len += (size_t)n;
    }
    if (!raw) {
        why(out, PUDDLE_NET_FAILED, "out of memory");
        goto done;
    }
    raw[len] = '\0';
    {
        int status = 0;
        char *e = strstr(raw, "\r\n\r\n");
        if (!e || sscanf(raw, "HTTP/1.%*d %d", &status) != 1) {
            why(out, PUDDLE_NET_FAILED, "%s did not answer as a Puddle does", addr.address);
            goto done;
        }
        out->result = PUDDLE_NET_OK;
        out->status = status;
        out->body_len = len - (size_t)(e + 4 - raw);
        out->body = (char *)malloc(out->body_len + 1);
        if (out->body) {
            memcpy(out->body, e + 4, out->body_len);
            out->body[out->body_len] = '\0';
        }
    }
done:
    free(raw);
    if (ssl) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
    }
    if (ctx)
        SSL_CTX_free(ctx);
    puddle_sock_close(sock);
}
