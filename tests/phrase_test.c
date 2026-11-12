#include "core/phrase.h"

#include "base/encoding.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static const char ALPHABET[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

/* the fixed phrase and the seed it must always give (cross-checked against an independent HKDF) */
#define KAT_PHRASE "0123-4567-89AB-CDEF-GHJK-MNPQ-RSTV-G406"
#define KAT_SEED "efbdebc558c3d67ecf521269e6a7d5c0a26753b7b079f1aa70c934f9b53202d2"

/* index in `p` of the n-th character that is part of the phrase (skipping hyphens) */
static int pos_of(int n)
{
    return n + n / 4;
}

int main(void)
{
    char p[PUDDLE_PHRASE_LEN + 1], canon[PUDDLE_PHRASE_LEN + 1];

    /* ---- shape ---- */
    CHECK(puddle_phrase_generate(p) == 0);
    CHECK(strlen(p) == PUDDLE_PHRASE_LEN);
    for (int i = 0; i < PUDDLE_PHRASE_LEN; i++) {
        if (i % 5 == 4)
            CHECK(p[i] == '-');
        else
            CHECK(strchr(ALPHABET, p[i]) != NULL && p[i] != '\0');
    }
    CHECK(puddle_phrase_parse(p, canon) == PUDDLE_PHRASE_OK && strcmp(p, canon) == 0);

    /* a thousand are valid and no two alike */
    static char all[1000][PUDDLE_PHRASE_LEN + 1];
    for (int i = 0; i < 1000; i++) {
        CHECK(puddle_phrase_generate(all[i]) == 0);
        CHECK(puddle_phrase_parse(all[i], NULL) == PUDDLE_PHRASE_OK);
        for (int j = 0; j < i; j++)
            if (strcmp(all[i], all[j]) == 0) { CHECK(0 && "duplicate phrase"); break; }
    }
    /* every one of the 28 random positions takes all 32 symbols (the generator is not biased to a subset) */
    int seen[28][32];
    memset(seen, 0, sizeof seen);
    for (int i = 0; i < 1000; i++)
        for (int k = 0; k < 28; k++)
            seen[k][strchr(ALPHABET, all[i][pos_of(k)]) - ALPHABET] = 1;
    for (int k = 0; k < 28; k++) {
        int distinct = 0;
        for (int v = 0; v < 32; v++) distinct += seen[k][v];
        CHECK(distinct == 32);
    }

    /* ---- typing it back in ---- */
    CHECK(puddle_phrase_parse(KAT_PHRASE, canon) == 0 && strcmp(canon, KAT_PHRASE) == 0);
    CHECK(puddle_phrase_parse("0123 4567 89ab cdef ghjk mnpq rstv g406", canon) == 0 && strcmp(canon, KAT_PHRASE) == 0); /* spaces, lower case */
    CHECK(puddle_phrase_parse("01234567 89ABCDEF\nGHJKMNPQ\tRSTVG406\n", canon) == 0 && strcmp(canon, KAT_PHRASE) == 0);
    CHECK(puddle_phrase_parse("0123-4567-89AB-CDEF-GHJK-MNPQ-RSTV-G4O6", canon) == 0 && strcmp(canon, KAT_PHRASE) == 0); /* O is 0 */
    CHECK(puddle_phrase_parse("O123-4567-89AB-CDEF-GHJK-MNPQ-RSTV-G406", canon) == 0 && strcmp(canon, KAT_PHRASE) == 0);
    CHECK(puddle_phrase_parse("0I23-4567-89AB-CDEF-GHJK-MNPQ-RSTV-G406", canon) == 0 && strcmp(canon, KAT_PHRASE) == 0); /* I is 1 */
    CHECK(puddle_phrase_parse("0l23-4567-89AB-CDEF-GHJK-MNPQ-RSTV-G406", canon) == 0 && strcmp(canon, KAT_PHRASE) == 0); /* so is l */
    CHECK(puddle_phrase_parse("0O23-4567-89AB-CDEF-GHJK-MNPQ-RSTV-G406", NULL) == PUDDLE_PHRASE_BAD_CHECKSUM);         /* O is 0: really a different phrase */
    CHECK(puddle_phrase_parse("  0123-4567-89AB-CDEF-GHJK-MNPQ-RSTV-G406  ", NULL) == 0);

    /* ---- what is wrong ---- */
    CHECK(puddle_phrase_parse("", NULL) == PUDDLE_PHRASE_BAD_LENGTH);
    CHECK(puddle_phrase_parse(NULL, NULL) == PUDDLE_PHRASE_BAD_LENGTH);
    CHECK(puddle_phrase_parse("0123-4567-89AB-CDEF-GHJK-MNPQ-RSTV", NULL) == PUDDLE_PHRASE_BAD_LENGTH);        /* a group short */
    CHECK(puddle_phrase_parse("0123-4567-89AB-CDEF-GHJK-MNPQ-RSTV-G406-0000", NULL) == PUDDLE_PHRASE_BAD_LENGTH); /* a group too many */
    CHECK(puddle_phrase_parse("0123-4567-89AB-CDEF-GHJK-MNPQ-RSTV-G40", NULL) == PUDDLE_PHRASE_BAD_LENGTH);
    CHECK(puddle_phrase_parse("0123-4567-89AB-CDEF-GHJK-MNPQ-RSTV-G406!", NULL) == PUDDLE_PHRASE_BAD_CHARACTER);
    CHECK(puddle_phrase_parse("0123-4567-89AB-CDEF-GHJK-MNPQ-RSTV-G40U", NULL) == PUDDLE_PHRASE_BAD_CHARACTER);    /* U is not in the alphabet */
    CHECK(puddle_phrase_parse("0123-4567-89AB-CDEF-GHJK-MNPQ-RSTV-G40\xc3\xa9", NULL) == PUDDLE_PHRASE_BAD_CHARACTER);
    CHECK(puddle_phrase_parse("0123-4567-89AB-CDEF-GHJK-MNPQ-RSTV-G407", NULL) == PUDDLE_PHRASE_BAD_CHECKSUM);

    /* ---- typos are caught, every time ---- */
    int single = 0, swaps = 0;
    for (int i = 0; i < 200; i++) {
        const char *good = all[i];
        for (int k = 0; k < 32; k++) { /* every position, including the checksum group */
            for (int v = 0; v < 32; v++) {
                char bad[PUDDLE_PHRASE_LEN + 1];
                strcpy(bad, good);
                if (ALPHABET[v] == bad[pos_of(k)])
                    continue;
                bad[pos_of(k)] = ALPHABET[v];
                CHECK(puddle_phrase_parse(bad, NULL) == PUDDLE_PHRASE_BAD_CHECKSUM);
                single++;
            }
        }
        for (int k = 0; k < 31; k++) { /* neighbours swapped, within and across groups */
            char bad[PUDDLE_PHRASE_LEN + 1];
            strcpy(bad, good);
            char t = bad[pos_of(k)];
            bad[pos_of(k)] = bad[pos_of(k + 1)];
            bad[pos_of(k + 1)] = t;
            if (strcmp(bad, good) == 0)
                continue;
            CHECK(puddle_phrase_parse(bad, NULL) == PUDDLE_PHRASE_BAD_CHECKSUM);
            swaps++;
        }
    }
    CHECK(single > 190000 && swaps > 5000);

    /* ---- the seed ---- */
    unsigned char seed[32], again[32], other[32];
    char hex[65];
    CHECK(puddle_phrase_seed(KAT_PHRASE, seed) == 0);
    for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", seed[i]);
    CHECK(strcmp(hex, KAT_SEED) == 0);
    CHECK(puddle_phrase_seed("0123 4567 89ab cdef ghjk mnpq rstv g406", again) == 0 && memcmp(seed, again, 32) == 0); /* same secret, any spelling */
    CHECK(puddle_phrase_seed(all[0], seed) == 0 && puddle_phrase_seed(all[1], other) == 0 && memcmp(seed, other, 32) != 0);
    CHECK(puddle_phrase_seed("0123-4567-89AB-CDEF-GHJK-MNPQ-RSTV-G407", seed) == PUDDLE_PHRASE_BAD_CHECKSUM); /* no seed from a bad phrase */
    CHECK(puddle_phrase_seed("nonsense", seed) != 0);

    return failures ? 1 : 0;
}
