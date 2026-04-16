# Sony SDK Reference Policy

The `reference/sony-sdk/` path is a **read-only mount** of the official Sony
PlayStation 3 SDK, used privately as an API coverage oracle for the open-source
stack in this repository.

## Hard rules

1. **Never commit.** `reference/sony-sdk/` is in the top-level `.gitignore` and
   must remain so. Pre-commit hooks reject any diff touching this path.
2. **Never copy.** Do not copy Sony SDK headers, source files, binaries, or
   documentation into any other path in this repository.
3. **Never ship.** No CI artifact, release tarball, or Docker image may contain
   any portion of the Sony SDK.
4. **Never upload.** Do not paste Sony SDK content into chat tools, pastebins,
   gists, or any external system.
5. **Clean-room reimplementation only.** Reference signatures, constants, and
   NID values may be consulted, but implementations are written from scratch.

## How to mount

On Windows, create a directory junction pointing at the user's private SDK
install:

```bat
cmd /c mklink /J reference\sony-sdk "D:\path\to\your\Sony\SDK\475.001"
icacls reference\sony-sdk /deny %USERNAME%:(WD,DC)
```

The `icacls` deny is belt-and-suspenders: it blocks write/delete on the junction
contents, reducing the chance of accidental modification.

## What the SDK is used for

- API coverage matrix: `tools/coverage-report/` walks `target/ppu/include/` and
  `target/spu/include/` and compares against PSL1GHT's surface.
- NID/FNID verification: `tools/nidgen/src/verify.rs` extracts symbol tables
  from Sony stub `.a` archives (via `ar t`) and cross-checks against the
  computed FNID values in the YAML database.
- Syscall numbering reference: when PSDevWiki is ambiguous, consult Sony's
  `lv2` headers for authoritative syscall numbers.
- Gap finding: list subsystems the open stack does not yet cover.

## What the SDK is not used for

- Source code. Ever.
- Stub .a files beyond symbol-table enumeration.
- Documentation beyond high-level orientation (the SCE manuals contain material
  that must not be redistributed).

## If you are reviewing this repo

`reference/sony-sdk/` will not be present in any clone of this repository. That
is by design. The tooling detects its absence and falls back to PSL1GHT-only
analysis, with reduced coverage confidence.
