#include "core/auth.h"

#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static int rm_cb(const char *p, const struct stat *sb, int t, struct FTW *f) { (void)sb; (void)t; (void)f; remove(p); return 0; }

int main(void)
{
    char root[512], err[300], hdr[200], path[700];
    const char *base = getenv("TMPDIR");
    snprintf(root, sizeof root, "%s/puddle-auth-XXXXXX", base ? base : "/tmp");
    if (!mkdtemp(root))
        return 2;

    puddle_auth *a = puddle_auth_open(root, err, sizeof err);
    CHECK(a != NULL);
    const char *tok = puddle_auth_token(a);
    CHECK(strlen(tok) == 64);
    for (int i = 0; i < 64; i++) CHECK((tok[i] >= '0' && tok[i] <= '9') || (tok[i] >= 'a' && tok[i] <= 'f'));

    snprintf(path, sizeof path, "%s/state/owner-token", root);
    CHECK(strcmp(puddle_auth_token_path(a), path) == 0);
    struct stat st;
    CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600);

    char first[65];
    memcpy(first, tok, 65);
    snprintf(hdr, sizeof hdr, "Bearer %s", first);
    CHECK(puddle_auth_check_bearer(a, hdr));
    snprintf(hdr, sizeof hdr, "bearer %s", first);
    CHECK(puddle_auth_check_bearer(a, hdr)); /* the scheme name is case-insensitive */

    /* everything else is no */
    CHECK(!puddle_auth_check_bearer(a, NULL));
    CHECK(!puddle_auth_check_bearer(a, ""));
    CHECK(!puddle_auth_check_bearer(a, "Bearer"));
    CHECK(!puddle_auth_check_bearer(a, "Bearer "));
    snprintf(hdr, sizeof hdr, "%s", first);
    CHECK(!puddle_auth_check_bearer(a, hdr));             /* no scheme */
    snprintf(hdr, sizeof hdr, "Basic %s", first);
    CHECK(!puddle_auth_check_bearer(a, hdr));
    snprintf(hdr, sizeof hdr, "Bearer %s ", first);
    CHECK(!puddle_auth_check_bearer(a, hdr));             /* trailing space */
    snprintf(hdr, sizeof hdr, "Bearer %s0", first);
    CHECK(!puddle_auth_check_bearer(a, hdr));             /* too long */
    snprintf(hdr, sizeof hdr, "Bearer %.63s", first);
    CHECK(!puddle_auth_check_bearer(a, hdr));             /* too short */
    snprintf(hdr, sizeof hdr, "Bearer %s", first);
    hdr[7 + 63] = (hdr[7 + 63] == 'a') ? 'b' : 'a';
    CHECK(!puddle_auth_check_bearer(a, hdr));             /* one character wrong, at the end */
    snprintf(hdr, sizeof hdr, "Bearer %s", first);
    hdr[7] = (hdr[7] == 'a') ? 'b' : 'a';
    CHECK(!puddle_auth_check_bearer(a, hdr));             /* ...and at the start */
    puddle_auth_close(a);

    /* the same token next time */
    a = puddle_auth_open(root, err, sizeof err);
    CHECK(a && strcmp(puddle_auth_token(a), first) == 0);
    puddle_auth_close(a);

    /* a damaged file is replaced, and the old token stops working */
    FILE *f = fopen(path, "w");
    fputs("not a token\n", f);
    fclose(f);
    a = puddle_auth_open(root, err, sizeof err);
    CHECK(a && strlen(puddle_auth_token(a)) == 64 && strcmp(puddle_auth_token(a), first) != 0);
    snprintf(hdr, sizeof hdr, "Bearer %s", first);
    CHECK(!puddle_auth_check_bearer(a, hdr));
    puddle_auth_close(a);

    /* a different Puddle gets a different token */
    char root2[600];
    snprintf(root2, sizeof root2, "%s/other", root);
    mkdir(root2, 0700);
    puddle_auth *b = puddle_auth_open(root2, err, sizeof err);
    a = puddle_auth_open(root, err, sizeof err);
    CHECK(a && b && strcmp(puddle_auth_token(a), puddle_auth_token(b)) != 0);
    puddle_auth_close(a);
    puddle_auth_close(b);

    nftw(root, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    return failures ? 1 : 0;
}
