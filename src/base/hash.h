#ifndef PUDDLE_HASH_H
#define PUDDLE_HASH_H

/* SHA-256, HKDF and hex: libpuddle-proto's, under Puddle's names.
 *
 *   puddle_hex_encode(bytes, len, out)        lowercase hex, out holds 2*len+1
 *   puddle_sha256(data, len, out[32])
 *   puddle_sha256_hex(data, len, out[65])
 *   puddle_hkdf_sha256(ikm, len, salt, info, out, outlen)   RFC 5869; 0 or -1 */

#include <puddle_proto.h>

#define puddle_hex_encode pp_hex_encode
#define puddle_sha256 pp_sha256
#define puddle_sha256_hex pp_sha256_hex
#define puddle_hkdf_sha256 pp_hkdf_sha256

#endif
