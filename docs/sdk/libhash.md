# libhash (cellMd5 / cellSha* — message digests) — MD5 and SHA-1/224/256/384/512

| Property | Value |
|---|---|
| Header | `<cell/hash/libmd5.h>`, `<cell/hash/libsha1.h>`, `<cell/hash/libsha224.h>`, `<cell/hash/libsha256.h>`, `<cell/hash/libsha384.h>`, `<cell/hash/libsha512.h>`; shared codes in `<cell/hash/libhash.h>` |
| Link name | `-lmd5 -lsha1 -lsha224 -lsha256 -lsha384 -lsha512` (one archive per algorithm) |
| Availability | PPU static library, not a SPRX — no `cellSysmoduleLoad` needed. Both ABIs (ILP32 and LP64). |

The six `cell/hash` libraries are self-contained PPU implementations:
MD5 (RFC 1321) and the SHA family as FIPS 180-4 (SHA-1, SHA-224, SHA-256,
SHA-384, SHA-512). Each header declares four entry points with the same
shape (one one-shot digest plus the `BlockInit`/`BlockUpdate`/`BlockResult`
trio), and all of them share the code set in `<cell/hash/libhash.h>`.

## Surface

For each algorithm `<A>` (Md5, Sha1, Sha224, Sha256, Sha384, Sha512) the
header declares:

| Function | Signature |
|---|---|
| `cell<A>Digest` | `int(const void *plain, unsigned int len, unsigned char *digest)` |
| `cell<A>BlockInit` | `int(Cell<A>WorkArea *workarea)` |
| `cell<A>BlockUpdate` | `int(Cell<A>WorkArea *workarea, const void *plain, unsigned int len)` |
| `cell<A>BlockResult` | `int(Cell<A>WorkArea *workarea, unsigned char *digest)` |

`cell<A>Digest` hashes one buffer in a single call and writes the digest
into `digest`, which must hold `CELL_<A>_DIGEST_SIZE` bytes.

The `Block*` trio is for data that arrives in pieces: `BlockInit` starts a
work area, `BlockUpdate` may be called any number of times against that
same work area, and `BlockResult` finalises it and writes the digest. Each
algorithm's context is a caller-allocated `Cell<A>WorkArea` (MD5, SHA-1,
SHA-224, SHA-256, SHA-384, SHA-512 respectively); treat its members as
private.

### Digest sizes

| Algorithm | Digest bytes | Block bytes |
|---|---|---|
| MD5 | `CELL_MD5_DIGEST_SIZE` 16 | 64 |
| SHA-1 | `CELL_SHA1_DIGEST_SIZE` 20 | 64 |
| SHA-224 | `CELL_SHA224_DIGEST_SIZE` 28 | 64 |
| SHA-256 | `CELL_SHA256_DIGEST_SIZE` 32 | 64 |
| SHA-384 | `CELL_SHA384_DIGEST_SIZE` 48 | 128 |
| SHA-512 | `CELL_SHA512_DIGEST_SIZE` 64 | 128 |

## Error codes

The shared `<cell/hash/libhash.h>` declares the codes every one of the six
returns. Each call returns `CELL_OK` or one of:

| Error | Value |
|---|---|
| `CELL_ERROR_HASH_NULL_POINTER` | 0x80f00001 |
| `CELL_ERROR_HASH_QUEUE_CREATE_FAILED` | 0x80f00002 |
| `CELL_ERROR_HASH_FLAG_CREATE_FAILED` | 0x80f00003 |
| `CELL_ERROR_HASH_INVALID_ALIGN` | 0x80f00004 |
| `CELL_ERROR_HASH_INVALID_NOMEM` | 0x80f00005 |
| `CELL_ERROR_HASH_TASK_CREATE_FAILED` | 0x80f00006 |
| `CELL_ERROR_HASH_INVALID_STATE` | 0x80f00007 |
| `CELL_ERROR_HASH_INVALID_BUFFER_SIZE` | 0x80f00008 |
| `CELL_ERROR_HASH_INVALID_STREAM_ID` | 0x80f00009 |

A NULL pointer argument yields `CELL_ERROR_HASH_NULL_POINTER`. Feeding data
to a work area after `BlockResult` has run yields
`CELL_ERROR_HASH_INVALID_STATE`.

## Notable ABI quirks

- **Work-area layout is fixed.** The `Cell<A>WorkArea` structs use no
  pointers and no `size_t`, and the headers note this makes the layout the
  same in ILP32 and LP64, so a work area built against one data model is a
  valid one against the other.
- **No global state.** The functions keep no state across work areas, so
  separate `Cell<A>WorkArea`s may be driven from separate threads.
- **`BlockResult` is idempotent per start.** After `BlockResult` the work
  area accepts no more `BlockUpdate` data until `BlockInit` starts it
  again, but `BlockResult` itself may be repeated and writes the same
  digest.
- **One archive per algorithm.** Every algorithm links as its own archive
  (`-lmd5`, `-lsha1`, `-lsha224`, `-lsha256`, `-lsha384`, `-lsha512`).
  SHA-224 and SHA-384 each compile their own copy of the SHA-256 / SHA-512
  compression core, so no archive links against another.

## Sample

`samples/util/hello-ppu-hash/` — for each of the six algorithms digests the
standard test vectors ("abc", the alphabet pangram, and one million `a`)
against the published digests, repeats the million through
`BlockInit`/`BlockUpdate`/`BlockResult` in uneven pieces and confirms it
matches, checks the two refusals, and times a 4 MB run.
