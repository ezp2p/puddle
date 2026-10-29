#include "base/encoding.h"

#include <string.h>

size_t puddle_base32_encode(const unsigned char *in, size_t len, char *out, size_t cap)
{
    static const char alpha[] = "abcdefghijklmnopqrstuvwxyz234567";
    size_t need = (len * 8 + 4) / 5;
    if (cap < need + 1)
        return 0;
    unsigned buf = 0;
    int bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        buf = (buf << 8) | in[i];
        bits += 8;
        while (bits >= 5) {
            out[o++] = alpha[(buf >> (bits - 5)) & 31];
            bits -= 5;
        }
    }
    if (bits)
        out[o++] = alpha[(buf << (5 - bits)) & 31];
    out[o] = '\0';
    return o;
}

size_t puddle_base64_encode(const unsigned char *in, size_t len, char *out, size_t cap)
{
    static const char alpha[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t need = (len + 2) / 3 * 4;
    if (cap < need + 1)
        return 0;
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        unsigned v = (unsigned)in[i] << 16;
        if (i + 1 < len) v |= (unsigned)in[i + 1] << 8;
        if (i + 2 < len) v |= in[i + 2];
        out[o++] = alpha[(v >> 18) & 63];
        out[o++] = alpha[(v >> 12) & 63];
        out[o++] = i + 1 < len ? alpha[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < len ? alpha[v & 63] : '=';
    }
    out[o] = '\0';
    return o;
}

static int b64val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int puddle_base64_decode(const char *in, unsigned char *out, size_t cap, size_t *outlen)
{
    size_t n = strlen(in);
    if (n == 0 || n % 4 != 0)
        return -1;
    size_t o = 0;
    for (size_t i = 0; i < n; i += 4) {
        int v[4], pad = 0;
        for (int j = 0; j < 4; j++) {
            char c = in[i + j];
            if (c == '=') {
                if (i + 4 != n || j < 2)
                    return -1; /* padding only in the last quad, at most two */
                v[j] = 0;
                pad++;
            } else if (pad || (v[j] = b64val(c)) < 0) {
                return -1;
            }
        }
        unsigned x = (unsigned)v[0] << 18 | (unsigned)v[1] << 12 | (unsigned)v[2] << 6 | (unsigned)v[3];
        size_t take = 3 - (size_t)pad;
        if ((pad == 2 && (x & 0xFFFF)) || (pad == 1 && (x & 0xFF)))
            return -1; /* non-zero bits where padding says there are none */
        if (o + take > cap)
            return -1;
        out[o++] = (unsigned char)(x >> 16);
        if (take > 1) out[o++] = (unsigned char)(x >> 8);
        if (take > 2) out[o++] = (unsigned char)x;
    }
    *outlen = o;
    return 0;
}

int puddle_hex_decode(const char *hex, unsigned char *out, size_t n)
{
    if (strlen(hex) != 2 * n)
        return -1;
    for (size_t i = 0; i < n; i++) {
        int hi, lo;
        char a = hex[2 * i], b = hex[2 * i + 1];
        hi = (a >= '0' && a <= '9') ? a - '0' : (a >= 'a' && a <= 'f') ? a - 'a' + 10 : -1;
        lo = (b >= '0' && b <= '9') ? b - '0' : (b >= 'a' && b <= 'f') ? b - 'a' + 10 : -1;
        if (hi < 0 || lo < 0)
            return -1;
        out[i] = (unsigned char)(hi << 4 | lo);
    }
    return 0;
}
