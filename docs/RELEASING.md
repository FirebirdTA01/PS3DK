# Pre-release gate

A release candidate is tested before its tag is pushed, so a regression is
found in private rather than after the release is public.  Every step below
must pass on the candidate zip built by the release workflow from the commit
the tag will point at.  The release steps themselves (changelog, version
stamp, tag) are in [VERSIONING.md](VERSIONING.md#cutting-a-release).

## 1. Archive diff against the previous release

Diff every installed archive member by member against the previous release
(`ar` member lists and per-member sha256).  Every changed object must map to
an intended change in the changelog.  A whole-file sha256 is not enough: two
archives with identical members used to differ in their headers, and a
library can change behaviour through a header it did not own.

For each ABI (`ppu/lib` and `ppu/lib/lp64`), the four profiler archives
`libps3dkprof.a`, `libps3prof.a`, `libps3dkprof_ext.a` and
`libps3dkprof_ext_live.a` must hold exactly the members listed in
`scripts/check-release-tree.sh`, which rejects missing, extra or duplicate
members. That script is the only copy of the lists; this page does not
repeat them. In outline: the manual archive (`libps3dkprof.a`) holds the core,
drain, version, platform, rolling-capture and live-link members but not the
automatic lifecycle or the arc serializer, which only `libps3prof.a` adds; the
extension archive adds the SPU/RSX runtime and its generated identity member;
the live archive holds the live-link members alone. Include these members in
the diff and hash record, and update the script's lists when compiler adapters
or profiler members are added, alongside their build and acceptance tests.

The shared required-artifact manifest also requires both archives for both
ABIs and `ppu/include/ps3dk/prof.h`. Windows packaging copies the installed
SDK tree, then validates it with that manifest; no separate profiler payload
list is maintained. Archive membership proves composition, not startup
retention: retain the installed-library anchor/no-anchor RPCS3 controls in
`tests/sdk/ps3dk-prof-auto`, with `--gc-sections`, for both ABIs.

## 2. Reproducibility

- `scripts/leakscan.py` on the extracted candidate, with a `--prefix` for
  each actual checkout, build and staging directory the candidate was built
  from (its built-in list covers only the default layouts).  Keep the
  command line and the report with the gate record.  Exit 0 means the
  scanner's checks found nothing (PE timestamps, the listed path prefixes,
  date strings); it is not a proof that no build path remains anywhere,
  since some files are not scanned.  Exit 3 means the scan was incomplete
  and is a failure.
- Two builds of one commit, with the same version inputs, are
  byte-identical, with no exceptions: `scripts/compare-builds.py <A> <B>`
  on the two zips exits 0.  The two runs can land on different runner
  images, so a build tool that writes its own version into shipped files is
  pinned in the workflow: the release jobs that run CMake use the exact
  version in `scripts/ci-pin-cmake.sh` and fail if another one is on PATH,
  and the jobs that build the Rust tools install the exact Rust in
  `scripts/ci-pin-rust.sh` and check it with `ci-pin-rust.sh --check`.
- Separately, a deliberate version-only comparison (the same tree with only
  the version changed) shows the expected stamp differences and nothing
  else: `scripts/compare-builds.py <A> <B> --version-string <old>=<new>`
  exits 0.  For the Rust tools those are the version bytes and the PE
  checksum.

## 3. SDK, samples and RPCS3

- The full SDK builds, and every sample builds against the candidate.
- The runnable samples boot in RPCS3 (visible window) without a fatal.
- The full shader-compiler suite runs on the candidate and on the previous
  release; no row may go from passing to failing.
- The release-gate tests that skip in CI without a PPU compiler run here
  with `PS3DEV` set (for example `tests/sdk/large-toc-test.sh`,
  `tests/sdk/data-model-link-test.sh`, `tests/sdk/gcm-link-surface-test.sh`).

## 4. EMP acceptance

The EMP Engine team builds and runs their engine on the candidate and posts
a PASS record.  The tag waits for it.

1. **Install.**  The candidate zip replaces the installed PS3DK, as a
   clean extract, after EMP has been told and with no EMP build or RPCS3
   run in flight.  The installed tree is first copied to a rollback
   directory and that copy is verified (a file inventory with sha256s),
   so the previous SDK can be restored exactly.  EMP's pinned configure
   (W0/G7) must pass.  Its pins are per archive member.
2. **Build** (native Windows).  From the engine's rapid-development head,
   with `PS3DK=PS3DEV=<scratch>` and `EMP_PS3_PORT_DIR=<plugin>/Port`:

   ```sh
   cmake --preset PS3-Debug
   cmake --build out/build/PS3-Debug -j 4
   ```

   This builds the engine library, the SPU jobs and the test application,
   and packages it.  PASS is rc 0 plus the plugin's source guards
   (`Tools/test_ps3_draw_counts.py --self-test`, `test_ps3_fog_guards.py`,
   `test_ps3_occlusion_fade.py --self-test`).
3. **Run** in RPCS3, as the installed title EMPP00001:
   1. The test application's normal scene reaches `[PS3Draws] frame 1202`
      with no FATAL, with the draw counts of the previous release.
   2. The instancing fixture's on/off pair keeps the previous release's
      draw counts, and the two screens differ only in the animated
      particles.
   3. A current game debug export reaches the same point, with the same
      outcome, as a control boot on the previous release the same day.

The PASS record is one message with the candidate zip's sha256, the engine
and plugin commits, the build rc, the guard results, the three RPCS3
outcomes with their `[PS3Draws]` lines, and the run-folder paths, which are
kept until the release ships.

## Publishing

The tag and the GitHub release are published once every step above has
passed and the release reviewers have signed off.  After the release, the
installed SDK is replaced with a clean extract of the published zip.
