/* gdbstub_codec.c - wire framing codec (encode + decode) for t_d0968566.
 *
 * C99 (compiled -std=c99 -pedantic). No session state, no transport, no
 * backend. The two functions here are pure byte-in/byte-out, so the codec
 * can be tested in isolation from the 91 session core.
 *
 * Framing (both directions): 0x24 '$', body, 0x23 '#', two hex checksum
 * digits. Encode emits UPPERCASE digits and never emits RLE. Decode
 * accepts escape sequences and the assigned input-side RLE extension.
 *
 * Checksum rule (contract line 15; mirrors verify.py's frame()): it is the
 * sum of the TRANSMITTED body bytes -- escape markers (0x7D) and RLE
 * markers (0x2A) INCLUDED -- modulo 256. It is deliberately NOT the sum of
 * the decoded payload: the "decode-wrong-basis" fixture exists to reject a
 * decoder that uses the decoded basis for verification.
 *
 * Encode: reserved wire bytes 0x23 0x24 0x7D 0x2A become 0x7D + (b ^ 0x20);
 * all other bytes pass through. If the framed length would exceed cap,
 * return GDBSTUB_LIMIT with *n == 0 and write nothing (no partial output).
 *
 * Decode: 0x7D unescapes the next byte (X ^ 0x20). A raw 0x2A '*' is RLE:
 * "*C" repeats the preceding decoded byte (C - 29) MORE times, C an ASCII
 * byte 0x20..0x7E excluding 0x23/0x24, and a predecessor must already
 * exist. An escaped literal '*' (0x7D 0x0A) stays literal. Malformed
 * structure, malformed RLE, or a checksum mismatch returns GDBSTUB_FRAME
 * with *n == 0; a decode that would exceed cap returns GDBSTUB_LIMIT with
 * *n == 0. Checksum hex digits are case-insensitive on input (contract
 * line 15: requests accept either hex case).
 */
#include "gdbstub.h"

/* Reserved wire bytes that must be escaped on output. */
static int codec_reserved(uint8_t b)
{
  return b == 0x23u || b == 0x24u || b == 0x7Du || b == 0x2Au;
}

/* Hex digit value of a byte, accepting BOTH upper- and lower-case (0-9,
 * A-F, a-f); returns -1 if not a hex digit. */
static int codec_hexval(uint8_t b)
{
  if (b >= '0' && b <= '9')
    return (int)(b - '0');
  if (b >= 'A' && b <= 'F')
    return (int)(b - 'A' + 10);
  if (b >= 'a' && b <= 'f')
    return (int)(b - 'a' + 10);
  return -1;
}

/* Uppercase hex digit character for a nibble 0..15. */
static uint8_t codec_hexdig(int n)
{
  return (uint8_t)(n < 10 ? '0' + n : 'A' + n - 10);
}

gdbstub_status gdbstub_codec_encode(const uint8_t *payload, size_t len,
                                    uint8_t *out, size_t cap, size_t *n)
{
  size_t i;

  if (n)
    *n = 0u;
  if (out == 0 || (len > 0u && payload == 0))
    return GDBSTUB_ERROR;

  /* Minimum framed output is 4 bytes: 0x24 + 0x23 + 2 checksum digits.
   * Preflight the framed length with SUBTRACTION bounds and NO wider-type
   * assumption (on a 64-bit host size_t already equals unsigned long long,
   * so casting up does not prevent overflow of a huge len). Maintain the
   * invariant framed <= cap: a reserved byte contributes 2 body bytes, a
   * plain byte 1; reject the moment an incoming byte would push framed past
   * cap (this also bounds the scan once cap is already exceeded). Because
   * framed never exceeds cap (hence never exceeds SIZE_MAX), every
   * "framed += add" is overflow-free, and the later emit pass writes at
   * most cap bytes. */
  if (cap < 4u)
    return GDBSTUB_LIMIT;

  size_t framed = 4u;
  for (i = 0u; i < len; i++) {
    size_t add = codec_reserved(payload[i]) ? 2u : 1u;
    if (add > cap - framed) /* framed <= cap here, so cap - framed is safe */
      return GDBSTUB_LIMIT; /* no partial output */
    framed += add;
  }

  out[0] = 0x24u;
  size_t o = 1u;
  /* Sum in unsigned char: wraps mod 256 by definition, no signed-overflow.
   * This is the checksum basis (transmitted body bytes, escapes included). */
  unsigned char sum = 0u;

  for (i = 0u; i < len; i++) {
    uint8_t b = payload[i];
    if (codec_reserved(b)) {
      uint8_t x;
      out[o] = 0x7Du;
      o++;
      sum = (unsigned char)(sum + 0x7Du);
      x = (uint8_t)(b ^ 0x20u);
      out[o] = x;
      o++;
      sum = (unsigned char)(sum + (unsigned char)x);
    } else {
      out[o] = b;
      o++;
      sum = (unsigned char)(sum + (unsigned char)b);
    }
  }
  out[o] = 0x23u;
  o++;
  out[o] = codec_hexdig((int)((unsigned int)sum >> 4u));
  o++;
  out[o] = codec_hexdig((int)((unsigned int)sum & 0xFu));
  o++;

  if (n)
    *n = o;
  return GDBSTUB_OK;
}

gdbstub_status gdbstub_codec_decode(const uint8_t *frame, size_t len,
                                    uint8_t *out, size_t cap, size_t *n)
{
  size_t i;

  if (n)
    *n = 0u;
  if (frame == 0 || out == 0)
    return GDBSTUB_FRAME;

  /* Minimum valid frame is '$' '#' hex hex (empty body) = 4 bytes. */
  if (len < 4u)
    return GDBSTUB_FRAME;
  if (frame[0] != 0x24u)
    return GDBSTUB_FRAME;
  /* '#', then the two checksum digits must occupy the last two bytes. */
  if (frame[len - 3u] != 0x23u)
    return GDBSTUB_FRAME;
  {
    int dh = codec_hexval(frame[len - 2u]);
    int dl = codec_hexval(frame[len - 1u]);
    int declared;
    if (dh < 0 || dl < 0)
      return GDBSTUB_FRAME;
    declared = dh * 16 + dl;

    /* Body is frame[1 .. len-4] inclusive: bodyn = len - 4 bytes. */
    const uint8_t *body = frame + 1u;
    size_t bodyn = len - 4u;

    /* Checksum is over the raw transmitted body bytes. */
    unsigned char sum = 0u;
    for (i = 0u; i < bodyn; i++)
      sum = (unsigned char)(sum + (unsigned char)body[i]);
    if ((int)sum != declared)
      return GDBSTUB_FRAME;

    /* Decode body -> out. k = decoded bytes produced so far. */
    size_t k = 0u;
    for (i = 0u; i < bodyn; i++) {
      uint8_t b = body[i];

      if (b == 0x7Du) {
        /* Escape: next byte unescapes to (x ^ 0x20). */
        if (i + 1u >= bodyn)
          return GDBSTUB_FRAME;
        /* Raw delimiters stay structural even after an escape marker. */
        if (body[i + 1u] == 0x23u || body[i + 1u] == 0x24u)
          return GDBSTUB_FRAME;
        if (k >= cap)
          return GDBSTUB_LIMIT;
        out[k] = (uint8_t)(body[i + 1u] ^ 0x20u);
        k++;
        i++; /* consume the escaped payload byte */
        continue;
      }

      if (b == 0x2Au) {
        /* RLE: '*' repeats the PRECEDING decoded byte (C - 29) more times. */
        size_t more;
        if (k == 0u) /* no predecessor */
          return GDBSTUB_FRAME;
        if (i + 1u >= bodyn) /* dangling RLE marker */
          return GDBSTUB_FRAME;
        {
          uint8_t c = body[i + 1u];
          if (c < 0x20u || c > 0x7Eu || c == 0x23u || c == 0x24u)
            return GDBSTUB_FRAME; /* count must be printable, not #/$ */
          more = (size_t)(c - 0x1Du); /* C - 29 */
        }
        /* Cap check without wrapping k + more: (more > cap - k) is safe
         * because k <= cap here. */
        if (more > (cap >= k ? cap - k : 0u))
          return GDBSTUB_LIMIT;
        if (more > 0u) {
          /* Save the predecessor into a local first, then perform the
           * (more) assignment stores and a single k += more as separately
           * sequenced operations -- this is what -Werror=sequence-point
           * requires (out[k++] = out[k-1] was the offending pattern). */
          uint8_t prev = out[k - 1u];
          size_t r;
          for (r = 0u; r < more; r++)
            out[k + r] = prev;
          k += more;
        }
        i++; /* consume the RLE count byte */
        continue;
      }

      /* Plain byte. A raw reserved byte (0x23 '#' or 0x24 '$') must have
       * been escaped (0x7D + (b ^ 0x20)) in the payload; 0x7D and 0x2A were
       * already consumed by their respective branches above, so this is the
       * only remaining reserved byte that can reach here. Reject it. */
      if (b == 0x23u || b == 0x24u)
        return GDBSTUB_FRAME;
      if (k >= cap)
        return GDBSTUB_LIMIT;
      out[k] = b;
      k++;
    }

    if (n)
      *n = k;
    return GDBSTUB_OK;
  }
}
