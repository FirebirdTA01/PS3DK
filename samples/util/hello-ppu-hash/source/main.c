/*
 * hello-ppu-hash - the cell/hash message digests: MD5, SHA-1, SHA-224,
 * SHA-256, SHA-384 and SHA-512.
 *
 * For each algorithm: hash the standard test messages ("abc", a pangram and
 * one million 'a') in one call and check the published digests; hash the
 * million in uneven pieces through BlockInit / BlockUpdate / BlockResult
 * and check it matches; check the refusals (NULL pointer, more data after
 * the result); then time 4 MB to show throughput on the PPU.
 *
 * The last TTY line is "hello-ppu-hash: PASS (<n> checks)" or names the
 * failures.
 */
#include <stdio.h>
#include <string.h>
#include <sys/process.h>
#include <sys/sys_time.h>
#include <cell/hash/libmd5.h>
#include <cell/hash/libsha1.h>
#include <cell/hash/libsha224.h>
#include <cell/hash/libsha256.h>
#include <cell/hash/libsha384.h>
#include <cell/hash/libsha512.h>

SYS_PROCESS_PARAM(1001, 0x10000);

#define MILLION 1000000u
#define BENCH   (4u * 1024u * 1024u)

static unsigned char data[BENCH];
static int checks, failures;

static void check(int ok, const char *algo, const char *what)
{
	checks++;
	if (!ok)
		failures++;
	printf("  %s %-7s %s\n", ok ? "ok  " : "FAIL", algo, what);
}

static void tohex(const unsigned char *d, unsigned int n, char *out)
{
	static const char digits[] = "0123456789abcdef";
	for (unsigned int i = 0; i < n; i++) {
		out[2 * i] = digits[d[i] >> 4];
		out[2 * i + 1] = digits[d[i] & 15];
	}
	out[2 * n] = '\0';
}

/* One algorithm's entry points, so the checks below run over all six. */
struct algo {
	const char *name;
	unsigned int size;
	int (*digest)(const void *, unsigned int, unsigned char *);
	int (*init)(void *);
	int (*update)(void *, const void *, unsigned int);
	int (*result)(void *, unsigned char *);
	const char *abc, *fox, *million;
};

/* Typed adapters: each work area type converts from void * implicitly. */
#define WRAP(Name)                                                                  \
	static int Name##_init(void *w) { return cell##Name##BlockInit(w); }            \
	static int Name##_update(void *w, const void *p, unsigned int n)                \
	{ return cell##Name##BlockUpdate(w, p, n); }                                    \
	static int Name##_result(void *w, unsigned char *d) { return cell##Name##BlockResult(w, d); }
WRAP(Md5)
WRAP(Sha1)
WRAP(Sha224)
WRAP(Sha256)
WRAP(Sha384)
WRAP(Sha512)

#define ALGO(Name, NAME, abc, fox, million)                                         \
	{ #Name, CELL_##NAME##_DIGEST_SIZE, cell##Name##Digest,                         \
	  Name##_init, Name##_update, Name##_result, abc, fox, million }

static const struct algo algos[] = {
	ALGO(Md5, MD5, "900150983cd24fb0d6963f7d28e17f72",
	     "9e107d9d372bb6826bd81d3542a419d6",
	     "7707d6ae4e027c70eea2a935c2296f21"),
	ALGO(Sha1, SHA1, "a9993e364706816aba3e25717850c26c9cd0d89d",
	     "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12",
	     "34aa973cd4c4daa4f61eeb2bdbad27316534016f"),
	ALGO(Sha224, SHA224, "23097d223405d8228642a477bda255b32aadbce4bda0b3f7e36c9da7",
	     "730e109bd7a8a32b1cb9d9a09aa2325d2430587ddbc0c38bad911525",
	     "20794655980c91d8bbb4c1ea97618a4bf03f42581948b2ee4ee7ad67"),
	ALGO(Sha256, SHA256, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
	     "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592",
	     "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"),
	ALGO(Sha384, SHA384,
	     "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7",
	     "ca737f1014a48f4c0b6dd43cb177b0afd9e5169367544c494011e3317dbf9a509cb1e5dc1e85a941bbee3d7f2afbc9b1",
	     "9d0e1809716474cb086e834e310a4a1ced149e9c00f248527972cec5704c2a5b07b8b3dc38ecc4ebae97ddd87f3d8985"),
	ALGO(Sha512, SHA512,
	     "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
	     "07e547d9586f6a73f73fbac0435ed76951218fb7d0c8d788a309d785436bbb642e93a252a954f23912547d1e8a3b5ed6e1bfd7097821233fa0538f3db854fee6",
	     "e718483d0ce769644e2e42c7bc15b4638e1f98b13b2044285632a803afa973ebde0ff244877ea60a4cb0432ce577c31beb009c5c2c49aa2e4eadb217ad8cc09b"),
};

/* Big enough for any work area (SHA-512's is the largest). */
static union {
	CellSha512WorkArea sha512;
	CellMd5WorkArea md5;
	unsigned long long align;
} wa;

static int digest_is(const struct algo *a, const unsigned char *d, const char *want)
{
	char hex[2 * CELL_SHA512_DIGEST_SIZE + 1];
	tohex(d, a->size, hex);
	return strcmp(hex, want) == 0;
}

static void run(const struct algo *a)
{
	static const char fox[] = "The quick brown fox jumps over the lazy dog";
	unsigned char d[CELL_SHA512_DIGEST_SIZE];
	char hex[2 * CELL_SHA512_DIGEST_SIZE + 1];
	int rc;

	printf("%s\n", a->name);
	rc = a->digest("abc", 3, d);
	check(rc == CELL_OK && digest_is(a, d, a->abc), a->name, "digest of \"abc\"");
	tohex(d, a->size, hex);
	printf("         = %s\n", hex);
	rc = a->digest(fox, sizeof fox - 1, d);
	check(rc == CELL_OK && digest_is(a, d, a->fox), a->name, "digest of the pangram");

	memset(data, 'a', MILLION);
	rc = a->digest(data, MILLION, d);
	check(rc == CELL_OK && digest_is(a, d, a->million), a->name, "digest of one million 'a'");

	/* The same million in pieces of 1 to 997 bytes. */
	a->init(&wa);
	rc = CELL_OK;
	for (unsigned int done = 0, k = 1; done < MILLION && rc == CELL_OK; done += k, k = k * 7 % 997 + 1) {
		unsigned int n = MILLION - done < k ? MILLION - done : k;
		rc = a->update(&wa, data + done, n);
	}
	if (rc == CELL_OK)
		rc = a->result(&wa, d);
	check(rc == CELL_OK && digest_is(a, d, a->million), a->name, "block API in uneven pieces");

	check(a->update(&wa, "x", 1) == (int)CELL_ERROR_HASH_INVALID_STATE, a->name,
	      "more data after the result is refused");
	check(a->digest(NULL, 0, d) == (int)CELL_ERROR_HASH_NULL_POINTER, a->name,
	      "NULL input is refused");

	/* Throughput over 4 MB. */
	for (unsigned int i = 0; i < BENCH; i++)
		data[i] = (unsigned char)(i * 131u + 7u);
	system_time_t t0 = sys_time_get_system_time();
	rc = a->digest(data, BENCH, d);
	system_time_t us = sys_time_get_system_time() - t0;
	check(rc == CELL_OK, a->name, "4 MB digest");
	if (us > 0)
		printf("         4 MB in %llu us = %llu KB/s\n", (unsigned long long)us,
		       (unsigned long long)BENCH * 1000000ull / 1024ull / (unsigned long long)us);
}

int main(void)
{
	printf("hello-ppu-hash: MD5, SHA-1, SHA-224/256/384/512\n");
	for (unsigned int i = 0; i < sizeof algos / sizeof algos[0]; i++)
		run(&algos[i]);
	if (failures)
		printf("hello-ppu-hash: FAIL (%d of %d checks)\n", failures, checks);
	else
		printf("hello-ppu-hash: PASS (%d checks)\n", checks);
	return failures != 0;
}
