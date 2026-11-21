#ifndef PUDDLE_LEVEE_H
#define PUDDLE_LEVEE_H

/* This Puddle's Levee (docs/levee.md; the protocol's spec/06): the join link the owner gave, and what joining told us, kept in
 * <data_dir>/state/levee.json. While joined, the Levee's address is this Puddle's address (puddle_my_address says it), and
 * every request this Puddle makes goes out through the Levee (http/client.cpp asks puddle_levee_current).
 *
 * The network side (attaching, the inbox, peers) is federation/levee.cpp's; this only keeps. Thread-safe. */

#include "core/link.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PUDDLE_LEVEE_TOKEN_MAX 40

/* A join link: levee://host[:port]?id=<levee id>&k=<base32 sealing key>&token=<token> */
typedef struct {
    puddle_address address;
    char id[53];
    unsigned char seal_key[32];
    char token[PUDDLE_LEVEE_TOKEN_MAX + 1];
} puddle_levee_link;

/* 0, or -1 if `text` is not such a link. */
int puddle_levee_link_parse(const char *text, puddle_levee_link *out);

typedef struct {
    int joined;
    puddle_address address;        /* where the Levee is, and so where others reach us */
    char id[53];                   /* the Levee's id */
    unsigned char key[32];         /* its signing key, which custody receipts are checked against */
    unsigned char seal_key[32];    /* its sealing key, for what is its own to read */
    unsigned turn_port;            /* the relay for calls, 0 for none */
    char turn_user[48], turn_pass[48];
    uint64_t after;                /* the inbox: everything up to here was taken */
    int64_t joined_ms;
} puddle_levee_state;

/* Read state/levee.json at start: 1 joined, 0 not, -1 damaged (treated as not joined). Sets the current state. */
int puddle_levee_load(const char *data_dir);
/* Save `st` (and make it current); `st->joined` 0 removes the file. 0 or -1. */
int puddle_levee_save(const char *data_dir, const puddle_levee_state *st);

/* The current state: 1 and filled while joined, else 0. */
int puddle_levee_current(puddle_levee_state *out);

/* A test seam: when set, this Puddle never routes through a Levee even while joined (in-process tests with several Puddles
 * that share these globals). Off by default. */
void puddle_levee_set_local_only(int on);

#ifdef __cplusplus
}
#endif
#endif
