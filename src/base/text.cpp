#include "base/text.h"

#include <stddef.h>
#include <string.h>

/* length of the valid UTF-8 sequence at s, or 0 */
static size_t seq_len(const unsigned char *s, size_t n)
{
    if (s[0] < 0x80) return 1;
    if (s[0] >= 0xC2 && s[0] <= 0xDF) return n >= 2 && (s[1] & 0xC0) == 0x80 ? 2 : 0;
    if (s[0] >= 0xE0 && s[0] <= 0xEF) {
        if (n < 3 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) return 0;
        if (s[0] == 0xE0 && s[1] < 0xA0) return 0;
        if (s[0] == 0xED && s[1] >= 0xA0) return 0;
        return 3;
    }
    if (s[0] >= 0xF0 && s[0] <= 0xF4) {
        if (n < 4 || (s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 || (s[3] & 0xC0) != 0x80) return 0;
        if (s[0] == 0xF0 && s[1] < 0x90) return 0;
        if (s[0] == 0xF4 && s[1] >= 0x90) return 0;
        return 4;
    }
    return 0;
}

static unsigned decode_cp(const unsigned char *s, size_t k)
{
    if (k == 1) return s[0];
    if (k == 2) return ((unsigned)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
    if (k == 3) return ((unsigned)(s[0] & 0x0F) << 12) | ((unsigned)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    return ((unsigned)(s[0] & 0x07) << 18) | ((unsigned)(s[1] & 0x3F) << 12) | ((unsigned)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
}

/* An address a person can be sent to: http or https, a host, nothing that could break out of where it is shown. */
int puddle_url_ok(const char *u, size_t max)
{
    size_t n = strlen(u);
    if (n == 0 || n > max)
        return 0;
    size_t start;
    if (n > 8 && puddle_ascii_ncasecmp(u, "https://", 8) == 0)
        start = 8;
    else if (n > 7 && puddle_ascii_ncasecmp(u, "http://", 7) == 0)
        start = 7;
    else
        return 0;
    if (u[start] == '/' || u[start] == '?' || u[start] == '#' || u[start] == '@')
        return 0; /* no host */
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)u[i];
        if (c <= 0x20 || c >= 0x7f || c == '"' || c == '<' || c == '>' || c == '\\' || c == '`' || c == '{' || c == '}' || c == '|' || c == '^')
            return 0;
    }
    return 1;
}

void puddle_text_cut(char *s, size_t max_chars)
{
    size_t chars = 0, i = 0;
    while (s[i] && chars < max_chars) {
        i++;
        while ((s[i] & 0xC0) == 0x80) /* the rest of this character */
            i++;
        chars++;
    }
    while (i > 0 && (s[i - 1] == ' ' || s[i - 1] == '\n' || s[i - 1] == '\t' || s[i - 1] == '\r'))
        i--;
    s[i] = '\0';
}

int puddle_clean_text(const char *in, size_t len, size_t max_chars, int multiline, char *out, size_t cap, size_t *chars)
{
    const unsigned char *u = (const unsigned char *)in;
    size_t o = 0, n = 0, i = 0;
    int last_kept = 0; /* output length that ends in something other than trailing blanks */
    while (i < len) {
        size_t k = seq_len(u + i, len - i);
        const char *emit = (const char *)u + i;
        size_t el = k;
        unsigned cp;
        if (k == 0) {
            emit = "\xEF\xBF\xBD";
            el = 3;
            k = 1;
            cp = 0xFFFD;
        } else {
            cp = decode_cp(u + i, k);
        }
        i += k;
        if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp < 0xA0) || cp == 0x2028 || cp == 0x2029) {
            if (cp == '\n' && multiline) {
                /* kept */
            } else if (cp == '\t' || cp == '\n' || cp == '\r' || cp == 0x2028 || cp == 0x2029) {
                emit = " ";
                el = 1;
            } else {
                continue; /* other control characters just go */
            }
        }
        if ((cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2066 && cp <= 0x2069) || cp == 0x200E || cp == 0x200F || cp == 0x061C)
            continue; /* direction overrides and marks */
        if (o == 0 && (emit[0] == ' ' || emit[0] == '\n') && el == 1)
            continue; /* leading blanks */
        if (o + el >= cap || (n > max_chars && n - max_chars > 63)) /* hard stop well past the limit; the exact check is below */
            return -1;
        memcpy(out + o, emit, el);
        o += el;
        n++;
        if (!(el == 1 && (emit[0] == ' ' || emit[0] == '\n')))
            last_kept = (int)o;
    }
    /* trailing blanks: drop them and the characters they were */
    for (size_t j = (size_t)last_kept; j < o; j++)
        n--;
    o = (size_t)last_kept;
    out[o] = '\0';
    if (n > max_chars)
        return -1;
    *chars = n;
    return 0;
}

int puddle_ascii_ncasecmp(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned char x = (unsigned char)a[i], y = (unsigned char)b[i];
        if (x >= 'A' && x <= 'Z') x = (unsigned char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (unsigned char)(y - 'A' + 'a');
        if (x != y || !x)
            return x - y;
    }
    return 0;
}
