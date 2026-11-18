/* Tests that drive the router send what a Puddle or a phone would: requests sealed to the Puddle under test (the protocol's
 * spec/09). Each signs with st_sign (or st_sign_as, for a phone) where it used to call puddle_reqsig_sign, and hands the
 * request to st_handle where it used to call puddle_router_handle; st_use says whose Puddle is under test. A request signed
 * this way goes sealed, and its answer comes back opened, so the test reads it as before. Anything else (an owner request,
 * something a test made by hand) goes as it is. `st_plain` set sends requests unsealed, as a bare address's first one is.
 *
 * What came back as sent, before opening, is kept in st_raw / st_raw_len, for tests that check receipts over it. */
#ifndef PUDDLE_TEST_SEALING_H
#define PUDDLE_TEST_SEALING_H

#include "base/seal.h"
#include "core/identity.h"
#include "http/reqsig.h"
#include "http/router.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char st_target[32];
static int st_have_target;
static int st_plain;
static puddle_sealed_request st_last;
static int st_last_on;
static unsigned char *st_last_plain;
static size_t st_last_plain_len;
static unsigned char st_raw[65536];
static size_t st_raw_len;

static void st_use(puddle_identity *under_test)
{
    puddle_identity_public(under_test, NULL, st_target, NULL);
    st_have_target = 1;
}

static int st_keep(int rc, const void *body, size_t body_len, char *out, size_t cap)
{
    if (rc != 0)
        return rc;
    free(st_last_plain);
    st_last_plain = (unsigned char *)malloc(body_len + 1);
    if (body_len)
        memcpy(st_last_plain, body, body_len);
    st_last_plain_len = body_len;
    if (strlen(st_last.signature) >= cap)
        return -1;
    memcpy(out, st_last.signature, strlen(st_last.signature) + 1);
    st_last_on = 1;
    return 0;
}

static int st_sign(puddle_identity *me, const char *method, const char *target, const void *body, size_t body_len, const char *recipient,
                   int64_t now_ms, char *out, size_t cap)
{
    if (st_plain || !st_have_target)
        return puddle_reqsig_sign(me, method, target, body, body_len, recipient, now_ms, out, cap);
    if (st_last_on)
        puddle_sealed_request_free(&st_last);
    st_last_on = 0;
    return st_keep(puddle_seal_request(me, method, target, body, body_len, recipient, st_target, now_ms, &st_last), body, body_len, out, cap);
}

static int st_sign_as(const char *signer_id, const unsigned char sign_pub[32], puddle_sign_fn sign, void *user, const char *method,
                      const char *target, const void *body, size_t body_len, const char *recipient, int64_t now_ms, char *out, size_t cap)
{
    if (st_plain || !st_have_target)
        return puddle_reqsig_sign_as(signer_id, sign_pub, sign, user, method, target, body, body_len, recipient, now_ms, out, cap);
    if (st_last_on)
        puddle_sealed_request_free(&st_last);
    st_last_on = 0;
    return st_keep(puddle_seal_request_as(signer_id, sign_pub, sign, user, method, target, body, body_len, recipient, st_target, now_ms,
                                          &st_last),
                   body, body_len, out, cap);
}

/* `out` holds a whole answer: if its body is sealed, open it in place under `key` (the Content-Type becomes JSON). */
static size_t st_open(char *out, size_t n, size_t cap, const unsigned char key[32], int status)
{
    char *end = NULL;
    for (size_t i = 0; i + 4 <= n; i++)
        if (!memcmp(out + i, "\r\n\r\n", 4)) {
            end = out + i + 4;
            break;
        }
    if (!end)
        return n;
    size_t hl = (size_t)(end - out), bl = n - hl;
    st_raw_len = bl < sizeof st_raw ? bl : 0;
    if (st_raw_len)
        memcpy(st_raw, end, bl);
    char *type = NULL;
    for (char *p = out; p && p < end; p = strstr(p + 1, "\r\n"))
        if (!strncmp(p, "\r\nContent-Type: " PUDDLE_SEALED_TYPE "\r\n", 16 + strlen(PUDDLE_SEALED_TYPE) + 2))
            type = p;
    if (!type || !bl)
        return n;
    size_t plain = puddle_seal_plain_len(bl), got = 0;
    unsigned char *pt = (unsigned char *)malloc(plain + 1);
    if (!pt || puddle_open_answer(key, status, (const unsigned char *)end, bl, pt, &got) != 0) {
        free(pt);
        return n;
    }
    /* "Content-Type: application/puddle-sealed" becomes "Content-Type: application/json", and the body the plaintext */
    char *head = (char *)malloc(hl + 16);
    size_t at = (size_t)(type - out) + 2;
    memcpy(head, out, at);
    size_t w = at + (size_t)sprintf(head + at, "Content-Type: application/json");
    const char *rest = type + 2 + strlen("Content-Type: " PUDDLE_SEALED_TYPE);
    memcpy(head + w, rest, (size_t)(end - rest));
    w += (size_t)(end - rest);
    if (w + got < cap) {
        memcpy(out, head, w);
        memcpy(out + w, pt, got);
        out[w + got] = '\0';
        n = w + got;
    }
    free(head);
    free(pt);
    return n;
}

/* the router, with a request st_sign made sent sealed, and its answer opened */
static size_t st_handle(const puddle_router *r, const puddle_request *req, int owner_ok, const unsigned char *body, size_t body_len, char *out,
                        size_t cap, int *status)
{
    if (!st_last_on || strcmp(req->signature, st_last.signature) != 0)
        return puddle_router_handle(r, req, owner_ok, body, body_len, out, cap, status);
    puddle_request sealed_req = *req;
    snprintf(sealed_req.seal, sizeof sealed_req.seal, "%s", st_last.seal);
    /* the body the test hands over is the one it signed, unless the test changed it on the way: then that goes, as it is */
    int same = body_len == st_last_plain_len && (!body_len || !memcmp(body, st_last_plain, body_len));
    unsigned char none = 0;
    const unsigned char *b = same ? (st_last.body ? st_last.body : &none) : body;
    size_t bl = same ? st_last.body_len : body_len;
    sealed_req.content_length = bl;
    size_t n = puddle_router_handle(r, &sealed_req, owner_ok, b, bl, out, cap, status);
    return st_open(out, n, cap, st_last.answer_key, *status);
}

#endif
