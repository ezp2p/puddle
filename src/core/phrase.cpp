#include "core/phrase.h"

#include "base/hash.h"

#include <string.h>

#include <openssl/rand.h>

#define DATA_SYMBOLS 28
#define CHECK_SYMBOLS 4
#define ALL_SYMBOLS (DATA_SYMBOLS + CHECK_SYMBOLS)

static const char ALPHABET[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

/* 0-31 for a symbol, -1 for anything else. Lower case is accepted and the look-alikes are read as
 * Crockford says: O is 0, I and L are 1. */
static int symbol_value(char c)
{
    if (c >= 'a' && c <= 'z')
        c = (char)(c - 'a' + 'A');
    switch (c) {
    case 'O': c = '0'; break;
    case 'I': case 'L': c = '1'; break;
    default: break;
    }
    const char *p = c ? strchr(ALPHABET, c) : NULL;
    return p ? (int)(p - ALPHABET) : -1;
}

/* A 20-bit CRC over the 140 data bits (x^20 + x^3 + 1, started from all ones). Any error confined to a
 * 20-bit stretch is detected: every single wrong symbol, and every pair of neighbours swapped. */
static unsigned crc20(const int *data)
{
    unsigned crc = 0xFFFFF;
    for (int i = 0; i < DATA_SYMBOLS; i++)
        for (int b = 4; b >= 0; b--) {
            unsigned bit = ((unsigned)data[i] >> b) & 1;
            unsigned top = (crc >> 19) & 1;
            crc = (crc << 1) & 0xFFFFF;
            if (top ^ bit)
                crc ^= 0x00009;
        }
    return crc;
}

static void check_symbols(const int *data, int out[CHECK_SYMBOLS])
{
    unsigned crc = crc20(data);
    for (int i = 0; i < CHECK_SYMBOLS; i++)
        out[i] = (int)((crc >> (5 * (CHECK_SYMBOLS - 1 - i))) & 31);
}

static void format(const int *symbols, char out[PUDDLE_PHRASE_LEN + 1])
{
    size_t o = 0;
    for (int i = 0; i < ALL_SYMBOLS; i++) {
        if (i && i % 4 == 0)
            out[o++] = '-';
        out[o++] = ALPHABET[symbols[i]];
    }
    out[o] = '\0';
}

int puddle_phrase_generate(char out[PUDDLE_PHRASE_LEN + 1])
{
    unsigned char raw[DATA_SYMBOLS];
    int symbols[ALL_SYMBOLS];
    if (RAND_bytes(raw, sizeof raw) != 1)
        return -1;
    for (int i = 0; i < DATA_SYMBOLS; i++)
        symbols[i] = raw[i] & 31; /* 32 symbols, so the low five bits are uniform */
    memset(raw, 0, sizeof raw);
    check_symbols(symbols, symbols + DATA_SYMBOLS);
    format(symbols, out);
    memset(symbols, 0, sizeof symbols);
    return 0;
}

/* symbols[0..31] from what was typed, ignoring separators */
static int read_symbols(const char *text, int symbols[ALL_SYMBOLS])
{
    int n = 0;
    for (const char *p = text; *p; p++) {
        if (*p == '-' || *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '_')
            continue;
        int v = symbol_value(*p);
        if (v < 0)
            return PUDDLE_PHRASE_BAD_CHARACTER;
        if (n >= ALL_SYMBOLS)
            return PUDDLE_PHRASE_BAD_LENGTH;
        symbols[n++] = v;
    }
    return n == ALL_SYMBOLS ? PUDDLE_PHRASE_OK : PUDDLE_PHRASE_BAD_LENGTH;
}

int puddle_phrase_parse(const char *text, char canonical[PUDDLE_PHRASE_LEN + 1])
{
    int symbols[ALL_SYMBOLS];
    if (!text)
        return PUDDLE_PHRASE_BAD_LENGTH;
    int rc = read_symbols(text, symbols);
    if (rc != PUDDLE_PHRASE_OK)
        return rc;
    int want[CHECK_SYMBOLS];
    check_symbols(symbols, want);
    for (int i = 0; i < CHECK_SYMBOLS; i++)
        if (want[i] != symbols[DATA_SYMBOLS + i])
            return PUDDLE_PHRASE_BAD_CHECKSUM;
    if (canonical)
        format(symbols, canonical);
    return PUDDLE_PHRASE_OK;
}

int puddle_phrase_seed(const char *text, unsigned char seed[32])
{
    char canon[PUDDLE_PHRASE_LEN + 1];
    int rc = puddle_phrase_parse(text, canon);
    if (rc != PUDDLE_PHRASE_OK)
        return rc;
    /* the 28 data symbols only: the checksum adds nothing to the secret */
    unsigned char data[DATA_SYMBOLS];
    int n = 0;
    for (const char *p = canon; *p; p++)
        if (*p != '-' && n < DATA_SYMBOLS)
            data[n++] = (unsigned char)symbol_value(*p);
    int ok = puddle_hkdf_sha256(data, sizeof data, "puddle-phrase-v1", "seed", seed, 32);
    memset(data, 0, sizeof data);
    memset(canon, 0, sizeof canon);
    return ok == 0 ? PUDDLE_PHRASE_OK : -1;
}
