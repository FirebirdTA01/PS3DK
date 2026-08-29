/* sha1-test.c - test vectors for our SHA-1.
 *
 * Part of the PS3 Custom Toolchain. MIT.
 *
 * This is the evidence that replacing upstream's SHA-1 was safe. The four
 * published vectors ("abc", the 448-bit two-block message, one million 'a',
 * and the empty string) come from the standard. The padding cases are not
 * published; their expected digests were computed independently with
 * coreutils sha1sum, which is the point - they exercise the buffering and
 * padding paths a published vector does not reach: a message that lands
 * exactly on a block boundary, and the 56..63 byte window where the 8-byte
 * length field no longer fits and forces a second block.
 *
 * Built and run by scripts/build-host-tools-windows.sh before sfo/pkg are
 * staged; a failure there stops the build rather than shipping a tool that
 * writes wrong PKG digests.
 */

#include "sha1.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int failures;

static void to_hex(const uint8_t *d, char *out)
{
	static const char hex[] = "0123456789abcdef";
	for (int i = 0; i < PS3_SHA1_DIGEST_SIZE; i++) {
		out[i * 2] = hex[d[i] >> 4];
		out[i * 2 + 1] = hex[d[i] & 0xf];
	}
	out[PS3_SHA1_DIGEST_SIZE * 2] = '\0';
}

static void check(const char *what, const uint8_t *digest, const char *expect)
{
	char got[PS3_SHA1_DIGEST_SIZE * 2 + 1];
	to_hex(digest, got);
	if (strcmp(got, expect) == 0) {
		printf("  ok    %s\n", what);
	} else {
		printf("  FAIL  %s\n        got      %s\n        expected %s\n",
		       what, got, expect);
		failures++;
	}
}

static void one_shot(const char *what, const char *msg, const char *expect)
{
	uint8_t d[PS3_SHA1_DIGEST_SIZE];
	ps3_sha1(msg, strlen(msg), d);
	check(what, d, expect);
}

int main(void)
{
	uint8_t d[PS3_SHA1_DIGEST_SIZE];

	printf("ps3 sha1 self-test\n");

	/* Published vectors. */
	one_shot("empty string", "",
	         "da39a3ee5e6b4b0d3255bfef95601890afd80709");
	one_shot("\"abc\"", "abc",
	         "a9993e364706816aba3e25717850c26c9cd0d89d");
	one_shot("448-bit two-block message",
	         "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
	         "84983e441c3bd26ebaae4aa1f95129e5e54670f1");

	/* One million 'a' - the third published vector, and the only one that
	   exercises a long streamed message. */
	{
		ps3_sha1_ctx ctx;
		char chunk[1000];
		memset(chunk, 'a', sizeof(chunk));
		ps3_sha1_init(&ctx);
		for (int i = 0; i < 1000; i++) {
			ps3_sha1_update(&ctx, chunk, sizeof(chunk));
		}
		ps3_sha1_final(&ctx, d);
		check("one million 'a'", d,
		      "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
	}

	/* Padding edge cases. A message of exactly 64 bytes fills a block with
	   nothing left over; 55, 56 and 63 bracket the point where the 8-byte
	   length field no longer fits and forces a second block. */
	{
		char buf[64];
		memset(buf, 'a', sizeof(buf));

		ps3_sha1(buf, 55, d);
		check("55 bytes (length just fits)", d,
		      "c1c8bbdc22796e28c0e15163d20899b65621d65a");

		ps3_sha1(buf, 56, d);
		check("56 bytes (length forces a block)", d,
		      "c2db330f6083854c99d4b5bfb6e8f29f201be699");

		ps3_sha1(buf, 63, d);
		check("63 bytes", d,
		      "03f09f5b158a7a8cdad920bddc29b81c18a551f5");

		ps3_sha1(buf, 64, d);
		check("64 bytes (exact block)", d,
		      "0098ba824b5c16427bd7a1122a5a442a25ec644d");
	}

	/* Streaming must agree with the one-shot path no matter where the
	   caller splits - pkg.c feeds the QA digest a file at a time. */
	{
		char buf[200];
		for (size_t i = 0; i < sizeof(buf); i++) {
			buf[i] = (char)(i & 0xff);
		}

		uint8_t whole[PS3_SHA1_DIGEST_SIZE];
		ps3_sha1(buf, sizeof(buf), whole);

		static const size_t splits[] = { 1, 63, 64, 65, 100, 127, 128, 199 };
		for (size_t s = 0; s < sizeof(splits) / sizeof(splits[0]); s++) {
			ps3_sha1_ctx ctx;
			size_t at = splits[s];
			ps3_sha1_init(&ctx);
			ps3_sha1_update(&ctx, buf, at);
			ps3_sha1_update(&ctx, buf + at, sizeof(buf) - at);
			ps3_sha1_final(&ctx, d);
			if (memcmp(d, whole, sizeof(d)) != 0) {
				printf("  FAIL  streamed split at %zu differs from one-shot\n", at);
				failures++;
			}
		}
		if (failures == 0) {
			printf("  ok    streamed updates match one-shot at 8 split points\n");
		}
	}

	if (failures) {
		printf("%d failure(s)\n", failures);
		return 1;
	}
	printf("all vectors pass\n");
	return 0;
}
