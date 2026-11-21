/* fnv1a.h - FNV-1a 64-bit hash (Fowler-Noll-Vo).
 *
 * Used by red_test.c to verify, at startup, that the fixture input files
 * match the immutable contract bundle (cross-checked independently via
 * SHA-256 provenance embedded in red_data.h). FNV-1a is simple and the
 * values in red_data.h have been verified against a Node.js reference to
 * the same constants, making the C implementation independently auditable.
 */
#ifndef RDAC_FNV1A_H
#define RDAC_FNV1A_H

#include <stddef.h>
#include <stdint.h>

#define FNV1A_64_OFFSET_BASIS 14695981039346656037ULL /* 0xcbf29ce484222325 */
#define FNV1A_64_PRIME        1099511628211ULL         /* 0x00000100000001b3 */

static inline uint64_t rdac_fnv1a64(const uint8_t *data, size_t len)
{
  uint64_t h = FNV1A_64_OFFSET_BASIS;
  for (size_t i = 0; i < len; i++) {
    h ^= (uint64_t)data[i];
    h *= FNV1A_64_PRIME;
  }
  return h;
}

/* Hex-encode a 64-bit FNV-1a value into 16 lowercase chars + NUL. */
static inline void rdac_fnv1a64_hex(uint64_t h, char out[17])
{
  static const char hxd[16] = "0123456789abcdef";
  for (int i = 15; i >= 0; i--) {
    out[i] = hxd[h & 0x0f];
    h >>= 4;
  }
  out[16] = 0;
}

#endif /* RDAC_FNV1A_H */
