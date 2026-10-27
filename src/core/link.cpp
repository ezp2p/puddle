#include "core/link.h"

#include "core/config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void seterr(char *err, size_t n, const char *msg)
{
    if (err && n)
        snprintf(err, n, "%s", msg);
}

static int base32_char(char c) { return (c >= 'a' && c <= 'z') || (c >= '2' && c <= '7'); }

int puddle_id_valid(const char *s)
{
    if (!s || strlen(s) != PUDDLE_USER_ID_LEN)
        return 0;
    for (size_t i = 0; i < PUDDLE_USER_ID_LEN; i++)
        if (!base32_char(s[i]))
            return 0;
    /* 52 characters carry 260 bits for a 256-bit hash, so the last one holds a single bit: only 'a' or 'q'.
     * Anything else is a second spelling of some id, and an id has to have exactly one. */
    return s[PUDDLE_USER_ID_LEN - 1] == 'a' || s[PUDDLE_USER_ID_LEN - 1] == 'q';
}

int puddle_invite_token_valid(const char *s)
{
    if (!s || strlen(s) != PUDDLE_INVITE_TOKEN_LEN)
        return 0;
    for (size_t i = 0; i < PUDDLE_INVITE_TOKEN_LEN; i++)
        if (!base32_char(s[i]))
            return 0;
    return 1;
}

static int ipv4_valid(const char *h)
{
    int parts = 0;
    const char *p = h;
    while (1) {
        if (!isdigit((unsigned char)*p))
            return 0;
        int v = 0, digits = 0;
        while (isdigit((unsigned char)*p)) {
            v = v * 10 + (*p - '0');
            if (++digits > 3 || v > 255)
                return 0;
            p++;
        }
        parts++;
        if (*p == '.') {
            p++;
            continue;
        }
        return *p == '\0' && parts == 4;
    }
}

static int ipv6_valid(const char *h)
{
    /* hex groups and colons, at most one "::", at most 8 groups; an embedded IPv4 tail is not accepted */
    size_t n = strlen(h);
    if (n < 2 || n > 45)
        return 0;
    int groups = 0, dbl = 0;
    size_t i = 0;
    if (h[0] == ':') {
        if (h[1] != ':')
            return 0;
        dbl = 1;
        i = 2;
    }
    while (i < n) {
        size_t start = i;
        while (i < n && isxdigit((unsigned char)h[i]))
            i++;
        if (i == start || i - start > 4)
            return 0;
        groups++;
        if (i == n)
            break;
        if (h[i] != ':')
            return 0;
        i++;
        if (i < n && h[i] == ':') {
            if (dbl)
                return 0;
            dbl = 1;
            i++;
            if (i == n)
                break;
        } else if (i == n) {
            return 0; /* a single trailing colon */
        }
    }
    return dbl ? groups <= 7 : groups == 8;
}

static int dns_valid(const char *h)
{
    size_t n = strlen(h);
    if (n == 0 || n > 253)
        return 0;
    size_t label = 0;
    for (size_t i = 0; i < n; i++) {
        char c = h[i];
        if (c == '.') {
            if (label == 0 || h[i - 1] == '-')
                return 0;
            label = 0;
        } else if (isalnum((unsigned char)c) || (c == '-' && label > 0)) {
            if (++label > 63)
                return 0;
        } else {
            return 0;
        }
    }
    return label > 0 && h[n - 1] != '-';
}

int puddle_address_parse(const char *text, puddle_address *out)
{
    if (!text || !*text || strlen(text) >= PUDDLE_ADDR_MAX)
        return -1;
    char host[PUDDLE_ADDR_MAX];
    const char *portp = NULL;
    int v6 = 0;
    if (text[0] == '[') {
        const char *close = strchr(text, ']');
        if (!close || close == text + 1)
            return -1;
        size_t hl = (size_t)(close - text - 1);
        memcpy(host, text + 1, hl);
        host[hl] = '\0';
        v6 = 1;
        if (close[1] == ':')
            portp = close + 2;
        else if (close[1] != '\0')
            return -1;
        if (!ipv6_valid(host))
            return -1;
    } else {
        const char *colon = strchr(text, ':');
        size_t hl = colon ? (size_t)(colon - text) : strlen(text);
        if (colon && strchr(colon + 1, ':'))
            return -1; /* a bare IPv6 literal: must be in brackets, or its last group looks like a port */
        memcpy(host, text, hl);
        host[hl] = '\0';
        if (colon)
            portp = colon + 1;
        int numeric = host[0] && isdigit((unsigned char)host[0]) && strspn(host, "0123456789.") == strlen(host);
        if (numeric ? !ipv4_valid(host) : !dns_valid(host))
            return -1;
    }
    unsigned port = PUDDLE_DEFAULT_PORT;
    if (portp) {
        size_t pl = strlen(portp);
        if (pl == 0 || pl > 5 || strspn(portp, "0123456789") != pl)
            return -1;
        port = (unsigned)atoi(portp);
        if (port < 1 || port > 65535)
            return -1;
    }
    memset(out, 0, sizeof *out);
    for (char *c = host; *c; c++)
        *c = (char)tolower((unsigned char)*c);
    snprintf(out->host, sizeof out->host, "%s", host);
    out->port = port;
    snprintf(out->address, sizeof out->address, v6 ? "[%s]:%u" : "%s:%u", host, port);
    return 0;
}

int puddle_link_parse(const char *text, puddle_link *out, char *err, size_t errlen)
{
    static const char SCHEME[] = "puddle://";
    if (!text || strlen(text) > 400) {
        seterr(err, errlen, "that is not an address");
        return -1;
    }
    for (const char *c = text; *c; c++)
        if ((unsigned char)*c <= ' ' || (unsigned char)*c >= 0x7f) {
            seterr(err, errlen, "an address has no spaces or special characters");
            return -1;
        }
    const char *a = text;
    if (strncmp(a, SCHEME, sizeof SCHEME - 1) == 0)
        a += sizeof SCHEME - 1;
    else if (strstr(a, "://")) {
        seterr(err, errlen, "that is not a Puddle address");
        return -1;
    }
    const char *q = strchr(a, '?');
    char addr[PUDDLE_ADDR_MAX + 8];
    size_t al = q ? (size_t)(q - a) : strlen(a);
    if (al > 0 && a[al - 1] == '/' && !q)
        al--; /* a trailing slash is harmless */
    if (al == 0 || al >= PUDDLE_ADDR_MAX || memchr(a, '@', al) || memchr(a, '/', al) || memchr(a, '#', al)) {
        seterr(err, errlen, "that address is not valid; it should look like host or host:port");
        return -1;
    }
    memcpy(addr, a, al);
    addr[al] = '\0';

    puddle_link l;
    memset(&l, 0, sizeof l);
    if (puddle_address_parse(addr, &l.address) != 0) {
        seterr(err, errlen, "that address is not valid; it should look like host or host:port");
        return -1;
    }
    if (q) {
        static const char KEY[] = "invite=";
        const char *v = q + 1;
        if (strncmp(v, KEY, sizeof KEY - 1) != 0) {
            seterr(err, errlen, "the only thing an address can carry is ?invite=");
            return -1;
        }
        v += sizeof KEY - 1;
        size_t vl = strlen(v);
        if (vl > 0 && v[vl - 1] == '/')
            vl--;
        if (vl != PUDDLE_INVITE_TOKEN_LEN) {
            seterr(err, errlen, "that invite is not valid");
            return -1;
        }
        memcpy(l.invite, v, vl);
        l.invite[vl] = '\0';
        if (!puddle_invite_token_valid(l.invite)) {
            seterr(err, errlen, "that invite is not valid");
            return -1;
        }
    }
    *out = l;
    return 0;
}

int puddle_link_format(const char *address, const char *invite, char *out, size_t cap)
{
    char shown[PUDDLE_ADDR_MAX];
    snprintf(shown, sizeof shown, "%s", address);
    size_t n = strlen(shown);
    char def[8];
    snprintf(def, sizeof def, ":%u", PUDDLE_DEFAULT_PORT);
    size_t dl = strlen(def);
    if (n > dl && !strcmp(shown + n - dl, def))
        shown[n - dl] = '\0'; /* the port everyone assumes is not written */
    int w = (invite && *invite) ? snprintf(out, cap, "%s?invite=%s", shown, invite) : snprintf(out, cap, "%s", shown);
    return w > 0 && (size_t)w < cap ? 0 : -1;
}

/* ---- this Puddle's own address ---- */

int puddle_my_address(const puddle_config *cfg, char *out, size_t cap)
{
    char where[PUDDLE_ADDR_MAX];
    if (cfg->advertise[0]) {
        snprintf(where, sizeof where, "%s", cfg->advertise);
    } else if (!strcmp(cfg->address, "::") || !strcmp(cfg->address, "0.0.0.0")) {
        return 0;
    } else if (strchr(cfg->address, ':')) {
        snprintf(where, sizeof where, "[%s]:%u", cfg->address, cfg->port);
    } else {
        snprintf(where, sizeof where, "%s:%u", cfg->address, cfg->port);
    }
    puddle_address a;
    if (puddle_address_parse(where, &a) != 0 || strlen(a.address) >= cap)
        return 0;
    memcpy(out, a.address, strlen(a.address) + 1);
    return 1;
}

int puddle_make_link(const puddle_config *cfg, const char *invite, char *out, size_t cap)
{
    char where[PUDDLE_ADDR_MAX];
    return puddle_my_address(cfg, where, sizeof where) && puddle_link_format(where, invite, out, cap) == 0;
}
