#ifndef PUDDLE_DEVICES_H
#define PUDDLE_DEVICES_H

/* Phones paired with this Puddle (docs/identity.md, "Devices"). A phone is a thin client: it holds none of the
 * identity's keys, only a key of its own, made on the phone at pairing. It signs every request with it, as one Puddle
 * signs to another, and is let into the owner API once the owner has accepted it. Kept in <data_dir>/state/devices.json.
 *
 * Pairing: the owner asks for a one-time secret (ten minutes, one use, a new one replaces the old) and shows it to the
 * phone in a link; the phone sends its name and public key with the secret, signed by that key, and waits as `pending`
 * until the owner accepts it. Revoking or declining forgets it. */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PUDDLE_DEVICE_ID_LEN 52       /* base32 of a SHA-256, like a user id (it never is one) */
#define PUDDLE_DEVICE_NAME_MAX 64     /* bytes of UTF-8 */
#define PUDDLE_DEVICES_MAX 20         /* on the list at once, pending included */
#define PUDDLE_PAIR_SECRET_LEN 32     /* hex characters: 128 bits */
#define PUDDLE_PAIR_MS (10 * 60 * 1000LL)

/* ---- a device's own key (the phone's side, and tests) ---- */

typedef struct {
    unsigned char priv[32], pub[32];  /* Ed25519 */
    char id[PUDDLE_DEVICE_ID_LEN + 1];
} puddle_devkey;

/* A new random key, or one from a 32-byte seed kept by the phone. 0 or -1. */
int puddle_devkey_generate(puddle_devkey *k);
int puddle_devkey_from_seed(puddle_devkey *k, const unsigned char seed[32]);
/* Sign `data` (a puddle_sign_fn, see http/reqsig.h: `user` is the puddle_devkey). 0 or -1. */
int puddle_devkey_sign(void *user, const void *data, size_t len, unsigned char sig[64]);

/* base32(SHA-256("puddle-device-v1\n" || pub)) */
void puddle_device_id(const unsigned char pub[32], char out[PUDDLE_DEVICE_ID_LEN + 1]);
/* Six digits from the same hash, for the owner to compare with what the phone shows. */
void puddle_device_check(const unsigned char pub[32], char out[7]);

/* ---- the list, on the Puddle ---- */

typedef enum { PUDDLE_DEVICE_PENDING, PUDDLE_DEVICE_ACTIVE } puddle_device_state;

typedef struct {
    char id[PUDDLE_DEVICE_ID_LEN + 1];
    char name[PUDDLE_DEVICE_NAME_MAX + 1];
    unsigned char pub[32];
    puddle_device_state state;
    int64_t created_ms;
    int64_t last_seen_ms;     /* its last request since the Puddle started; 0 if none (not kept on disk) */
} puddle_device;

typedef struct puddle_devices puddle_devices;

/* Read <data_dir>/state/devices.json (none yet is an empty list). NULL + `err` if it is damaged or cannot be read. */
puddle_devices *puddle_devices_open(const char *data_dir, char *err, size_t errlen);
void puddle_devices_close(puddle_devices *d);

/* A new pairing secret, replacing any other; it lasts until `*expires_ms`. 0 or -1. */
int puddle_devices_new_secret(puddle_devices *d, int64_t now_ms, char out[PUDDLE_PAIR_SECRET_LEN + 1], int64_t *expires_ms);
/* Is `secret` the current one, unspent and in time? 1 or 0 (constant time in the secret). Spends nothing. */
int puddle_devices_secret_ok(puddle_devices *d, const char *secret, int64_t now_ms);

/* The phone asks: spend the secret and put it on the list as pending. 0 added; 1 the secret is no longer good;
 * 2 that key is already on the list (it is left as it was; `*out` says how); 3 the list is full; -1 could not save. */
int puddle_devices_pair(puddle_devices *d, const char *secret, const unsigned char pub[32], const char *name, int64_t now_ms,
                        puddle_device *out, char *err, size_t errlen);

/* 0 and `*out`, or 1 if there is no such device. */
int puddle_devices_get(puddle_devices *d, const char *id, puddle_device *out);
/* Every device, newest first; free() the array. 0 or -1. */
int puddle_devices_list(puddle_devices *d, puddle_device **out, size_t *count);

/* Accept a pending device: 0, 1 no such device, 2 not pending, -1 could not save. */
int puddle_devices_accept(puddle_devices *d, const char *id, char *err, size_t errlen);
/* Forget a device (decline, revoke). With `only_pending`, an accepted one is left alone and 2 is returned.
 * 0, 1 no such device, 2, -1 could not save. */
int puddle_devices_remove(puddle_devices *d, const char *id, int only_pending, char *err, size_t errlen);

/* Note a request from it, for the list. */
void puddle_devices_seen(puddle_devices *d, const char *id, int64_t now_ms);

/* Request signature lookups (puddle_peer_key_fn; `devices` is the puddle_devices): the key of an ACCEPTED device, or of
 * any device on the list, pending too, if `kid` names it. */
int puddle_devices_key(void *devices, const char *id, const char *kid, unsigned char sign_pub[32]);
int puddle_devices_key_any(void *devices, const char *id, const char *kid, unsigned char sign_pub[32]);
/* Without a signature checked yet (to decide how much body to read): is `id` an accepted device? */
int puddle_devices_is_active(puddle_devices *d, const char *id);

#ifdef __cplusplus
}
#endif
#endif
