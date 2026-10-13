/* Host driver for hash-vectors-test.sh: builds the sdk/libhash sources
 * natively and prints, for every algorithm, the digests of a set of
 * messages fed whole and in uneven pieces, plus one line per error-path
 * check.  The shell side compares the digests with Python's hashlib. */
#include <stdio.h>
#include <string.h>
#include <cell/hash/libmd5.h>
#include <cell/hash/libsha1.h>
#include <cell/hash/libsha224.h>
#include <cell/hash/libsha256.h>
#include <cell/hash/libsha384.h>
#include <cell/hash/libsha512.h>

/* Message of length n: bytes of a fixed linear congruential sequence. */
static unsigned char msg[1000000];

static void fill(void)
{
	unsigned int x = 12345;
	for (size_t i = 0; i < sizeof msg; i++) {
		x = x * 1103515245u + 12345u;
		msg[i] = (unsigned char)(x >> 16);
	}
}

static void hex(const char *algo, unsigned int len, const char *how, const unsigned char *d, int n)
{
	printf("%s %u %s ", algo, len, how);
	for (int i = 0; i < n; i++)
		printf("%02x", d[i]);
	printf("\n");
}

static int fails;
static void expect(const char *what, int got, int want)
{
	if (got != want) {
		printf("ERR %s: rc 0x%08x, want 0x%08x\n", what, (unsigned)got, (unsigned)want);
		fails++;
	}
}

static const unsigned int lengths[] = {
	0, 1, 2, 3, 55, 56, 57, 63, 64, 65, 111, 112, 113, 119, 120, 127, 128, 129,
	191, 192, 255, 256, 257, 1000, 4096, 65535, 65536, 1000000,
};

#define RUN(Name, Tag, DS) do {                                                          \
	Cell##Name##WorkArea w;                                                              \
	unsigned char d[DS], e[DS];                                                          \
	for (size_t li = 0; li < sizeof lengths / sizeof lengths[0]; li++) {                 \
		unsigned int len = lengths[li];                                                  \
		expect(Tag " digest", cell##Name##Digest(msg, len, d), 0);                       \
		hex(Tag, len, "whole", d, DS);                                                   \
		/* Pieces of 1..13 bytes, then a zero-length update. */                          \
		expect(Tag " init", cell##Name##BlockInit(&w), 0);                               \
		for (unsigned int s = 0, k = 1; s < len; s += k, k = k % 13 + 1) {               \
			unsigned int n = len - s < k ? len - s : k;                                  \
			expect(Tag " update", cell##Name##BlockUpdate(&w, msg + s, n), 0);           \
		}                                                                                \
		expect(Tag " update0", cell##Name##BlockUpdate(&w, msg, 0), 0);                  \
		expect(Tag " result", cell##Name##BlockResult(&w, d), 0);                        \
		hex(Tag, len, "pieces", d, DS);                                                  \
		/* A repeated BlockResult writes the same digest; more data is refused. */       \
		memset(e, 0xee, DS);                                                             \
		expect(Tag " result2", cell##Name##BlockResult(&w, e), 0);                       \
		if (memcmp(d, e, DS) != 0) { printf("ERR %s result2 differs\n", Tag); fails++; } \
		expect(Tag " update-after-result", cell##Name##BlockUpdate(&w, msg, 1),          \
		       (int)CELL_ERROR_HASH_INVALID_STATE);                                      \
	}                                                                                    \
	expect(Tag " digest null plain", cell##Name##Digest(NULL, 0, d),                     \
	       (int)CELL_ERROR_HASH_NULL_POINTER);                                           \
	expect(Tag " digest null out", cell##Name##Digest(msg, 1, NULL),                     \
	       (int)CELL_ERROR_HASH_NULL_POINTER);                                           \
	expect(Tag " init null", cell##Name##BlockInit(NULL), (int)CELL_ERROR_HASH_NULL_POINTER); \
	cell##Name##BlockInit(&w);                                                           \
	expect(Tag " update null wa", cell##Name##BlockUpdate(NULL, msg, 1),                 \
	       (int)CELL_ERROR_HASH_NULL_POINTER);                                           \
	expect(Tag " update null plain", cell##Name##BlockUpdate(&w, NULL, 1),               \
	       (int)CELL_ERROR_HASH_NULL_POINTER);                                           \
	expect(Tag " result null wa", cell##Name##BlockResult(NULL, d),                      \
	       (int)CELL_ERROR_HASH_NULL_POINTER);                                           \
	expect(Tag " result null out", cell##Name##BlockResult(&w, NULL),                    \
	       (int)CELL_ERROR_HASH_NULL_POINTER);                                           \
	/* After a reinit the same area hashes from scratch. */                              \
	cell##Name##BlockUpdate(&w, "xyz", 3);                                               \
	cell##Name##BlockResult(&w, d);                                                      \
	cell##Name##BlockInit(&w);                                                           \
	cell##Name##BlockUpdate(&w, "abc", 3);                                               \
	cell##Name##BlockResult(&w, d);                                                      \
	hex(Tag, 3, "reinit-abc", d, DS);                                                    \
} while (0)

int main(void)
{
	fill();
	RUN(Md5, "md5", 16);
	RUN(Sha1, "sha1", 20);
	RUN(Sha224, "sha224", 28);
	RUN(Sha256, "sha256", 32);
	RUN(Sha384, "sha384", 48);
	RUN(Sha512, "sha512", 64);
	printf("checks-failed %d\n", fails);
	return fails != 0;
}
