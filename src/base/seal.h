#ifndef PUDDLE_SEAL_H
#define PUDDLE_SEAL_H

/* Sealing (the Puddle Protocol, spec/09-sealing.md): what keeps bodies private whatever carries them, so TLS is transport only.
 * libpuddle-proto's (third_party/protocol/lib/include/puddle_proto.h, where each call is described), under Puddle's names.
 *
 * HPKE (RFC 9180), base mode, one suite: DHKEM(X25519, HKDF-SHA256), HKDF-SHA256, ChaCha20-Poly1305. A request body is sealed to
 * the recipient's key with an HPKE context; the answer comes back in records under a key exported from that same context. */

#include <puddle_proto.h>

#define PUDDLE_SEAL_KEY_LEN PP_SEAL_KEY_LEN
#define PUDDLE_SEAL_ENC_LEN PP_SEAL_ENC_LEN
#define PUDDLE_SEAL_TAG_LEN PP_SEAL_TAG_LEN
#define PUDDLE_SEAL_RECORD PP_SEAL_RECORD

typedef pp_hpke puddle_hpke;

#define puddle_x25519_derive pp_x25519_derive
#define puddle_x25519_public pp_x25519_public
#define puddle_hpke_sender pp_hpke_sender
#define puddle_hpke_sender_with pp_hpke_sender_with
#define puddle_hpke_receiver pp_hpke_receiver
#define puddle_hpke_seal pp_hpke_seal
#define puddle_hpke_open pp_hpke_open
#define puddle_hpke_export pp_hpke_export
#define puddle_seal_answer_key pp_seal_answer_key
#define puddle_seal_answer_len pp_seal_answer_len
#define puddle_seal_plain_len pp_seal_plain_len
#define puddle_seal_record pp_seal_record
#define puddle_open_record pp_open_record
#define puddle_seal_answer pp_seal_answer
#define puddle_open_answer pp_open_answer

#endif
