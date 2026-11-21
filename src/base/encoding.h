#ifndef PUDDLE_ENCODING_H
#define PUDDLE_ENCODING_H

/* base32, base64 and hex, as the protocol writes them: libpuddle-proto's (third_party/protocol/lib), under Puddle's names.
 *
 *   puddle_base32_encode(in, len, out, cap)   lowercase, unpadded; the length written, or 0 if `cap` is too small
 *   puddle_base32_decode(in, out, len)        exactly `len` bytes; 0 or -1
 *   puddle_base64_encode(in, len, out, cap)   standard, padded; the length, or 0
 *   puddle_base64_decode(in, out, cap, &n)    strict; 0 and the count, or -1
 *   puddle_hex_decode(hex, out, n)            exactly `n` bytes from 2n lowercase hex characters; 0 or -1 */

#include <puddle_proto.h>

#define puddle_base32_encode pp_base32_encode
#define puddle_base32_decode pp_base32_decode
#define puddle_base64_encode pp_base64_encode
#define puddle_base64_decode pp_base64_decode
#define puddle_hex_decode pp_hex_decode

#endif
