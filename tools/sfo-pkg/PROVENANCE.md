# tools/sfo-pkg — provenance and local changes

Native `sfo` and `pkg` host tools. They replace `sfo.py` / `pkg.py` plus the
`pkgcrypt` C extension, which together put a Python interpreter on the Windows
first-run path (board task `t_3b2b85e3`).

## Where the code came from

| File | Origin |
|---|---|
| `pkg.c` | ps3dev/PSL1GHT, `tools/sfo_pkg/pkg.c`, commit `73e34af` — "Convert sfo.py and pkg.py to pure C (#162)". Modified, see below. |
| `sfo.c` | ps3dev/PSL1GHT, `tools/sfo_pkg/sfo.c`, commit `73e34af`. Unmodified. |
| `sfo.xml` | ps3dev/PSL1GHT, `tools/sfo_pkg/sfo.xml`, commit `73e34af`. Unmodified. |
| `sha1.c`, `sha1.h` | **Ours.** Written for this project from FIPS PUB 180-4. |
| `sha1-test.c` | **Ours.** |

`73e34af` is the only commit that has ever touched `tools/sfo_pkg`; upstream
master was `f649a08` when we took this snapshot (2026-08-29).

## Licensing

`pkg.c`, `sfo.c` and `sfo.xml` carry no per-file licence header, so they fall
under PSL1GHT's repository `LICENSE`: MIT, "Copyright (c) 2011 PSL1GHT
Development Team". Compatible with this repository's MIT licence; recorded in
the root `NOTICE`.

**Upstream's `sha1.c` / `sha1.h` are deliberately not vendored.** They are
"Copyright (C) 1998 Paul E. Jones, All Rights Reserved" under a bespoke notice
that reads:

> This software is licensed as "freeware." Permission to distribute this
> software in source and binary forms is hereby granted without a fee.

That grants distribution and nothing else — no explicit right to modify and no
sublicensing — which is weaker than MIT and awkward to carry inside an MIT tree
that builds it into shipped binaries. Rather than swap it for another
third-party SHA-1 (public-domain dedications vary in force by jurisdiction, so
that only shrinks the question), `sha1.c` here is written from the published
standard and validated against the standard's own test vectors.

Note the contrast with the GPL PS3 OpenGraphics Toolkit, which we may *run* as
a separate binary without its licence attaching: this code we copy into our
tree and link, so provenance actually matters.

## Local changes to `pkg.c`

Each is marked in the source with a `LOCAL ... (PS3 Custom Toolchain)` comment.

### 1. SHA-1 backend

Upstream's `SHA1Finalize` helper is removed and `sha1_hash` plus the QA-digest
block now call `ps3_sha1*` from our `sha1.c`. Behaviourally identical: verified
by building pristine upstream `pkg.c` against upstream's own `sha1.c` and
confirming byte-identical `.pkg` output.

### 2. Relative-path handling — an upstream bug, Windows-specific

`collect_dir` derives each entry's archive-relative path as
`full_path + strlen(original)`. Two defects:

* The **POSIX** branch checks whether `folder` already ends in a separator
  before joining; the **Win32** branch did not, and joined unconditionally with
  `"%s\\%s"`. CMake's `ps3_add_pkg` passes the directory *with* a trailing
  slash, so on Windows the join produced `<dir>/\<name>` and the stripped
  relative path kept a leading slash.
* Independently, on **both** platforms, passing the folder *without* a trailing
  separator also leaves a leading slash on the relative path.

That is not cosmetic. A few lines further down, the boot executable is
identified with an exact `strcmp(newpath, "USRDIR/EBOOT.BIN")`. With a leading
slash the comparison fails, so the entry is written as `TYPE_RAW` instead of
`TYPE_NPDRMSELF` and the NPDRM `0x30`/64-byte size rounding is skipped — a
`.pkg` whose boot executable is not flagged as an NPDRM SELF, and 16 bytes
larger than it should be.

Fixed by guarding the Win32 join the way POSIX already does, and by skipping any
leading separator at all four sites that compute a relative path.

**This should go upstream.** #162 is merged, so anyone building PKGs on Windows
with current PSL1GHT master is affected.

## How we know the output is right

`.pkg` and `PARAM.SFO` are compared byte-for-byte against the Python tools they
replace, using a real fixture: an `EBOOT.BIN` produced by `make_self_npdrm`, a
real `ICON0.PNG`, and a generated `PARAM.SFO`, so the NPDRM SELF path is
actually exercised.

Reference (Python 3.12 + `pkgcrypt.cp312-win_amd64.pyd`):
`5aaa5275934f37d2a5cfc52ab7ec4bc012c754e4ea6b51813ef12e213018cff6`

Matched by all four combinations: Linux and Windows builds, folder argument
given with and without a trailing separator. `PARAM.SFO` matches at
`3fdf389ab823c767a35739dca6700de520262151e78bdea3ab4e981a67f1f7dd`.

`sha1-test` covers the four published SHA-1 vectors plus block-boundary and
length-field padding cases and multi-call streaming; the padding digests were
computed independently with coreutils `sha1sum`.

## Known upstream warnings

`pkg.c` produces `-Wstringop-truncation` and `-Wformat-truncation` warnings
under `-Wall -Wextra`, all from upstream's fixed-size path buffers. Left alone
to keep the diff against upstream small and reviewable.
