#ifndef PUDDLE_IDENTITY_H
#define PUDDLE_IDENTITY_H

/* This Puddle's identity: a signing key pair (Ed25519), an encryption key pair (X25519) and a stable
 * user id, with a signed chain of records that vouches for them. Everything comes from a recovery
 * phrase (phrase.h) shown once at creation. See docs/roadmap.md, phase 1.
 *
 * A phrase makes three keys: the two "daily" keys above, kept on this machine, and a **roll key** that
 * is never stored; it only authorizes the next change of keys. The chain commits to the hash of the roll
 * key in advance. (Rolling is step 1.3; until then an identity has one generation.)
 *
 * Files in <data_dir>/identity/:
 *   keys         the daily private keys (mode 0600); no phrase, no roll key
 *   chain.jsonl  the signed records, one per line */

#include "core/phrase.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PUDDLE_USER_ID_LEN 52 /* base32 of a SHA-256 */

typedef struct puddle_identity puddle_identity;

/* Is there an identity in this data directory? (1 or 0; says nothing about whether it is usable.) */
int puddle_identity_exists(const char *data_dir);

/* Make a new identity from a fresh recovery phrase, which is copied to `phrase_out` and appears nowhere
 * else: not on disk, not in a log. Show it to the owner once. Refuses if one already exists. All or
 * nothing: a crash leaves no half-made identity. NULL + message in `err` on failure.
 *
 * The new identity is **unconfirmed** until puddle_identity_confirm() says its phrase has been written
 * down. An unconfirmed identity can't be opened, so nothing ever runs on a phrase nobody saw. */
puddle_identity *puddle_identity_create(const char *data_dir, char phrase_out[PUDDLE_PHRASE_LEN + 1],
                                        char *err, size_t errlen);

/* Rebuild an identity on a new machine (the old one lost or replaced) from the owner's **latest** recovery
 * phrase and a copy of the chain, which is not secret: it is `identity/chain.jsonl` from a backup, or
 * what a friend's Puddle holds. The chain must be valid, and the phrase must be the one its newest record
 * announces: that phrase is what the daily keys and roll key derive from. Nothing is made if anything is
 * wrong. No new phrase is made, and no confirmation is needed, because the owner already has theirs.
 *
 * An older phrase is refused with a message saying so: its keys were retired by a later roll. Refuses if
 * an identity already exists. NULL + message in `err` on failure. */
puddle_identity *puddle_identity_restore(const char *data_dir, const char *phrase, const char *chain_text,
                                         size_t chain_len, char *err, size_t errlen);

/* Is there an identity whose phrase was shown but never confirmed (the owner quit part way)? 1 or 0. */
int puddle_identity_unconfirmed(const char *data_dir);

/* The owner has written the phrase down. 0 (also if already confirmed), or -1 if there is no identity. */
int puddle_identity_confirm(const char *data_dir);

/* Throw away an identity that was never confirmed so setup can start again. Safe because nothing can
 * have used it. Refuses, and leaves a confirmed identity untouched. 0 discarded, 1 nothing to discard,
 * -1 failed. */
int puddle_identity_discard_unconfirmed(const char *data_dir, char *err, size_t errlen);

/* Load the existing identity. NULL + message if there is none, it is unconfirmed, it is damaged, or it
 * was made by an older development version that had no phrase. */
puddle_identity *puddle_identity_open(const char *data_dir, char *err, size_t errlen);
void puddle_identity_close(puddle_identity *id);

/* ---- changing the keys (rolling) ----
 *
 * Optional, never automatic, and in two phases so it can't strand anyone:
 *
 *   1. prepare: the owner types the CURRENT phrase. The Puddle checks it is the current one, makes a NEW
 *      phrase and the keys that go with it, and hands the new phrase back to be shown. Nothing on disk
 *      has changed: if the owner stops here, the old phrase still works.
 *   2. commit: the owner has written the new phrase down. Only now are the keys replaced, the old private
 *      key deleted from this machine, and a record signed three times appended to the chain: by the roll
 *      key (authority), by the new signing key (possession) and by the old signing key (the old key
 *      announcing its successor).
 *
 * The roll is journaled: if the process dies part way, the next puddle_identity_open() finishes it. */
typedef struct puddle_roll puddle_roll;

/* Phase 1. NULL + a reason in `err` if `current_phrase` is not a phrase, has a typo, or is not this
 * identity's current phrase (an older one, which has been spent, is refused the same way). */
puddle_roll *puddle_identity_roll_prepare(puddle_identity *id, const char *current_phrase,
                                          char new_phrase_out[PUDDLE_PHRASE_LEN + 1], char *err,
                                          size_t errlen);

/* Phase 2: the new phrase has been written down. Consumes `roll`. 0 or -1; on -1 nothing is lost and
 * the old phrase still works unless the message says the roll will finish on the next start. */
int puddle_identity_roll_commit(puddle_identity *id, puddle_roll *roll, char *err, size_t errlen);

/* Give up a prepared roll (also after commit has failed). Nothing changes. */
void puddle_roll_abandon(puddle_roll *roll);

/* The user id: base32(SHA-256(first signing public key)). It never changes, however often keys roll. */
void puddle_identity_id(puddle_identity *id, char out[PUDDLE_USER_ID_LEN + 1]);

/* Current public keys and how many times they have rolled (0 = never). Any pointer may be NULL. */
void puddle_identity_public(puddle_identity *id, unsigned char sign_pub[32], unsigned char enc_pub[32],
                            uint64_t *seq);

/* Ed25519 signature (64 bytes) by the current signing key. 0 or -1. */
int puddle_identity_sign(puddle_identity *id, const void *data, size_t len, unsigned char sig[64]);

/* The whole chain as text (one record per line), for sending to a peer. malloc'd; NULL on error. */
char *puddle_identity_chain(puddle_identity *id, size_t *len);

/* The public keys a phrase gives. Used to restore (1.4) and to check what was derived. */
typedef struct {
    unsigned char sign_pub[32];
    unsigned char enc_pub[32];
    unsigned char roll_pub[32];
} puddle_phrase_keys;
int puddle_identity_keys_from_phrase(const char *phrase, puddle_phrase_keys *out);

/* ---- other people's identities ---- */

typedef struct {
    char id[PUDDLE_USER_ID_LEN + 1];
    uint64_t seq;
    unsigned char sign_pub[32];  /* their current keys */
    unsigned char enc_pub[32];
    char next[65];               /* commitment to their roll key: SHA-256 of its public key, hex */
    char head_hash[65];          /* SHA-256 of the last record, hex */
} puddle_chain_head;

/* Check a chain someone sent. The first record is signed by the key the id is the hash of. Each later
 * record must follow the one before it, reveal a roll key that hashes to what the previous record
 * committed to, and carry three good signatures (roll key, new signing key, previous signing key). If
 * `expected_id` is non-NULL the chain must belong to it (this is how a link's id pins a person's key).
 * On success the current keys are in `out`. 0 or -1 with the reason in `err`. */
int puddle_chain_verify(const char *text, size_t len, const char *expected_id, puddle_chain_head *out,
                        char *err, size_t errlen);

/* Verify an Ed25519 signature. 1 valid, 0 not. */
int puddle_verify_signature(const unsigned char sign_pub[32], const void *data, size_t len,
                            const unsigned char sig[64]);

/* Short stable name for a signing key: hex SHA-256 of the public key. Requests say which key signed
 * them with this. */
void puddle_key_id(const unsigned char sign_pub[32], char out[65]);

#ifdef __cplusplus
}
#endif
#endif
