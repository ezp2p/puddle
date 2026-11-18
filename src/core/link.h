#ifndef PUDDLE_LINK_H
#define PUDDLE_LINK_H

/* Links and addresses. What one owner gives another so their Puddle can ask for a stream is an address:
 *
 *     203.0.113.7            (the port defaults to 443)
 *     home.example.org:8443
 *     [2001:db8::1]
 *
 * optionally written with the scheme, `puddle://home.example.org:8443`, and optionally carrying the Puddle's sealing key and an
 * invite, `home.example.org?k=<key>&invite=<token>`. No user id: the Puddle at an address says who it is when it answers. The
 * key (spec/09, "First contact") is what the first request is sealed to, and the chain in the answer must have it.
 * Nothing is looked up or resolved here; this only decides whether the text is well-formed, and the strictness
 * is the point: it comes from outside, and what it names is where we will connect. */

#include "core/config.h"
#include "core/identity.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PUDDLE_ADDR_MAX 128          /* "host:port", with brackets for IPv6 */
#define PUDDLE_INVITE_TOKEN_LEN 26   /* base32 of 16 random bytes */
#define PUDDLE_DEFAULT_PORT 443

typedef struct {
    char host[PUDDLE_ADDR_MAX];      /* without brackets */
    unsigned port;
    char address[PUDDLE_ADDR_MAX];   /* canonical "host:port" / "[v6]:port", as it is stored and dialled */
} puddle_address;

#define PUDDLE_LINK_KEY_LEN 52        /* base32 of a 32-byte sealing key */

typedef struct {
    puddle_address address;
    char invite[PUDDLE_INVITE_TOKEN_LEN + 1]; /* "" if the link has none */
    int has_key;                              /* the link gave a sealing key (`k=`) */
    unsigned char key[32];
} puddle_link;

/* Is `s` a user id (52 characters of lowercase base32)? Used before an id is allowed anywhere near a path. */
int puddle_id_valid(const char *s);

/* Is `s` an invite token as we make them? */
int puddle_invite_token_valid(const char *s);

/* Parse "host:port" (or just "host"). 0 or -1. Host is an IPv4 literal, a bracketed IPv6 literal, or a
 * DNS name (letters, digits, dots, hyphens; no empty labels). */
int puddle_address_parse(const char *text, puddle_address *out);

/* Parse what the owner typed or pasted. 0 or -1 with a short reason in `err`. Anything unexpected is refused: other
 * schemes, spaces, a path, a fragment, unknown query parameters, an `@`. */
int puddle_link_parse(const char *text, puddle_link *out, char *err, size_t errlen);

/* A Pond invite, `pond://host[:port]?invite=<token>&k=<key>` (the scheme and the invite are both needed; `k`, the Pond's sealing
 * key, too). As puddle_link_parse. */
int puddle_pond_link_parse(const char *text, puddle_link *out, char *err, size_t errlen);

/* The text to give someone for `address` ("host:port"): the port is left off when it is the default. `key` (a sealing key) and
 * `invite` may be NULL / "". 0 or -1 if it does not fit. */
int puddle_link_format(const char *address, const unsigned char *key, const char *invite, char *out, size_t cap);

/* host:port others reach this Puddle at, from the config; 0 if it cannot be known (see puddle_make_link). */
int puddle_my_address(const puddle_config *cfg, char *out, size_t cap);

/* The address to give someone for this Puddle (host[:port], the default port left off), with `?invite=<token>` when there is one,
 * and our sealing key (`k=`) when `me` is given, or 0 if we cannot say where that is:
 * listening on every interface says nothing about which address the outside world should use, so the owner
 * has to set `advertise:` in the config. */
int puddle_make_link(const puddle_config *cfg, puddle_identity *me, const char *invite, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
