#include "base/encoding.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void b32(const char *in, const char *want)
{
    char out[100];
    CHECK(puddle_base32_encode((const unsigned char *)in, strlen(in), out, sizeof out) == strlen(want));
    CHECK(strcmp(out, want) == 0);
}

static void b64(const char *in, const char *want)
{
    char out[100];
    unsigned char back[100];
    size_t n = 0;
    CHECK(puddle_base64_encode((const unsigned char *)in, strlen(in), out, sizeof out) == strlen(want));
    CHECK(strcmp(out, want) == 0);
    CHECK(puddle_base64_decode(want, back, sizeof back, &n) == 0 && n == strlen(in) && memcmp(back, in, n) == 0);
}

static int dec(const char *s)
{
    unsigned char out[100];
    size_t n;
    return puddle_base64_decode(s, out, sizeof out, &n);
}

int main(void)
{
    /* RFC 4648 test vectors (base32 lowercased, unpadded) */
    b32("f", "my"); b32("fo", "mzxq"); b32("foo", "mzxw6"); b32("foob", "mzxw6yq");
    b32("fooba", "mzxw6ytb"); b32("foobar", "mzxw6ytboi");

    unsigned char ff[32], zero[32] = { 0 };
    memset(ff, 0xff, sizeof ff);
    char out[100], want[60];
    CHECK(puddle_base32_encode(zero, 32, out, sizeof out) == 52);
    memset(want, 'a', 52); want[52] = '\0';
    CHECK(strcmp(out, want) == 0);
    CHECK(puddle_base32_encode(ff, 32, out, sizeof out) == 52);
    memset(want, '7', 51); want[51] = 'q'; want[52] = '\0';
    CHECK(strcmp(out, want) == 0);
    CHECK(puddle_base32_encode(zero, 32, out, 52) == 0); /* no room for the NUL */

    b64("f", "Zg=="); b64("fo", "Zm8="); b64("foo", "Zm9v"); b64("foob", "Zm9vYg==");
    b64("fooba", "Zm9vYmE="); b64("foobar", "Zm9vYmFy");

    /* strict decoding */
    CHECK(dec("Zm9v") == 0);
    CHECK(dec("") == -1);
    CHECK(dec("Zm9") == -1);          /* length */
    CHECK(dec("Zg=") == -1);          /* padding short */
    CHECK(dec("Zh==") == -1);         /* stray bits under the padding */
    CHECK(dec("Zm9=") == -1);
    CHECK(dec("Zm 9") == -1);         /* whitespace */
    CHECK(dec("Z===") == -1);
    CHECK(dec("Zg==Zm9v") == -1);     /* padding before the end */
    CHECK(dec("=m9v") == -1);
    CHECK(dec("Zm9v\n") == -1);
    CHECK(dec("Zm-v") == -1);         /* url-safe alphabet is not accepted */
    unsigned char small[2]; size_t n;
    CHECK(puddle_base64_decode("Zm9vYmFy", small, sizeof small, &n) == -1); /* too big for the buffer */

    unsigned char h[4];
    CHECK(puddle_hex_decode("00ff10ab", h, 4) == 0 && h[0] == 0 && h[1] == 0xff && h[2] == 0x10 && h[3] == 0xab);
    CHECK(puddle_hex_decode("00FF10AB", h, 4) == -1); /* lowercase only */
    CHECK(puddle_hex_decode("00ff10", h, 4) == -1);
    CHECK(puddle_hex_decode("00ff10ag", h, 4) == -1);
    return failures ? 1 : 0;
}
