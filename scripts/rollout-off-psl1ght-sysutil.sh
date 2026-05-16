#!/bin/bash
#
# rollout-off-psl1ght-sysutil.sh — GATED one-action repo-wide cutover
# from PSL1GHT libsysutil/liblv2 to the native reference-NID stubs.
#
#   *** DRY-RUN BY DEFAULT. Mutates nothing unless invoked with
#   *** --execute. This script exists so the director can review the
#   *** exact plan and approve it with a single command. It does NOT
#   *** run autonomously and is NOT committed.
#
# WHAT IT DOES (and why each step is safe / reversible)
# -----------------------------------------------------
#  1. Back up the current PSL1GHT libsysutil.a / liblv2.a (ILP32) into
#     build/rollout-backup-<ts>/ so the flip is fully reversible.
#  2. make -C sdk install-headers — installs the cut-over cell headers
#     (cell/sysutil.h, cell/msg_dialog.h, cell/sysutil_sysparam.h —
#     now direct reference-NID externs, verified PSL1GHT-forwarder-free
#     and cpp-clean) plus the new sys/ppu-asm.h and the guarded
#     sys/lv2_syscall.h (byte-equivalent first-include-wins guards;
#     proven zero codegen change for the validated suite).
#  3. Symlink-flip libsysutil.a -> libsysutil_stub.a in BOTH
#     ppu/lib and ppu/lib/lp64 — the exact reversible pattern already
#     in use for libgcm_sys.a -> libgcm_sys_stub.a. Also creates the
#     previously-ABSENT lp64/libsysutil.a (closes the LP64 gap).
#  4. Install our native multilib liblv2.a: build/liblv2.a -> ppu/lib,
#     build-lp64/liblv2.a -> ppu/lib/lp64 (also closes the ABSENT
#     lp64/liblv2.a gap). Functionally validated (hello-event-flag-spu
#     RAN-CLEAN through the real pipeline).
#  5. NO per-sample CMake churn: samples link the bare names `sysutil`
#     / `lv2`; after the swap `-lsysutil`/`-llv2` transparently
#     resolve to the native archives — identical mechanism to the
#     existing libgcm_sys cutover. ~24 sysutil/gcm samples are
#     affected purely by the resolved-archive change.
#  6. Then run the full validated-suite revalidation:
#       scripts/build-all-samples.sh   (ILP32) and --lp64 (LP64)
#       scripts/rpcs3-sweep.sh         over the produced .fake.self
#     and diff the classification against the pre-rollout baseline.
#
# REVERT: scripts/rollout-off-psl1ght-sysutil.sh --revert <ts>
#
set -u
SRC="$(cd "$(dirname "$0")/.." && pwd)"
PS3DK="${PS3DK:-$SRC/stage/ps3dev/ps3dk}"
LIB="$PS3DK/ppu/lib"
LIB64="$PS3DK/ppu/lib/lp64"
MODE="${1:-}"
TS="$(date +%Y%m%d-%H%M%S)"
BK="$SRC/build/rollout-backup-$TS"

say() { printf '  %s\n' "$*"; }

plan() {
  cat <<EOF
PLAN (dry-run — nothing changed):
  backup    -> $BK/{libsysutil.a,liblv2.a,lp64-*}
  headers   -> make -C $SRC/sdk install-headers   (PS3DK=$PS3DK)
  flip      -> $LIB/libsysutil.a        => symlink libsysutil_stub.a
               $LIB64/libsysutil.a      => symlink libsysutil_stub.a (NEW)
  liblv2    -> cp $SRC/sdk/liblv2/build/liblv2.a       $LIB/liblv2.a
               cp $SRC/sdk/liblv2/build-lp64/liblv2.a  $LIB64/liblv2.a (NEW)
  validate  -> scripts/build-all-samples.sh ; scripts/build-all-samples.sh --lp64
               scripts/rpcs3-sweep.sh <all .fake.self> ; diff vs baseline
PRECONDITIONS CHECKED BELOW.
EOF
  if grep -qsE 'PS3DK-NATIVE-VIDEO-SHIM: VALIDATED' "$SRC/sdk/include/sysutil/video.h" 2>/dev/null; then
    cat <<EOF

  Native <sysutil/video.h> compat shim: PRESENT + VALIDATED.
  The ~22 direct-PSL1GHT-video samples (20 gcm + sysutil/jpg-dec +
  sysutil/png) map cleanly onto the cellVideoOut* nidgen stubs
  (built + RPCS3-validated through the real pipeline).  This
  rollout requirement is satisfied; --execute is unblocked,
  pending director approval.
EOF
  else
    cat <<EOF

  *** INCOMPLETE — DO NOT EXECUTE YET ***
  ~22 samples (20 gcm + sysutil/jpg-dec + sysutil/png) #include
  PSL1GHT <sysutil/video.h> and call videoConfigure / videoGetState
  / videoGetResolution DIRECTLY in source.  After the libsysutil
  symlink-flip those PSL1GHT video symbols vanish; nidgen provides
  the cellVideoOut* set instead.  A native <sysutil/video.h> compat
  shim must exist + be validated first or --execute breaks them.
  The guard below hard-blocks --execute until the shim is validated.
EOF
  fi
}

preconds() {
  local ok=1
  [ -e "$SRC/sdk/liblv2/build/liblv2.a" ]      || { say "MISSING sdk/liblv2/build/liblv2.a"; ok=0; }
  [ -e "$SRC/sdk/liblv2/build-lp64/liblv2.a" ] || { say "MISSING sdk/liblv2/build-lp64/liblv2.a"; ok=0; }
  [ -e "$LIB/libsysutil_stub.a" ]              || { say "MISSING $LIB/libsysutil_stub.a"; ok=0; }
  [ -e "$LIB64/libsysutil_stub.a" ]            || { say "MISSING $LIB64/libsysutil_stub.a"; ok=0; }
  grep -qE '^extern int cellSysutilRegisterCallback' "$SRC/sdk/include/cell/sysutil.h" \
      || { say "cell/sysutil.h is NOT in cut-over form"; ok=0; }
  # HARD BLOCK: ~22 samples use PSL1GHT <sysutil/video.h> directly.
  # The native compat shim (sdk/include/sysutil/video.h backed by
  # cellVideoOut* nidgen stubs) MUST exist + be validated first, or
  # --execute would break those samples. Sentinel = the shim file
  # carrying the explicit validated marker.
  if ! grep -qsE 'PS3DK-NATIVE-VIDEO-SHIM: VALIDATED' "$SRC/sdk/include/sysutil/video.h" 2>/dev/null; then
    say "BLOCKED: native sysutil/video.h compat shim missing/unvalidated"
    say "  (~22 PSL1GHT-video-API samples would break — build+validate the shim first)"
    ok=0
  fi
  [ "$ok" = 1 ] && say "preconditions: OK" || { say "preconditions: FAILED"; return 1; }
}

case "$MODE" in
  --revert)
    BKD="$SRC/build/rollout-backup-${2:-}"
    [ -d "$BKD" ] || { echo "revert: no backup dir $BKD"; exit 1; }
    echo "REVERT from $BKD (requires --execute):"; [ "${3:-}" = --execute ] || { echo "  (dry-run)"; exit 0; }
    cp -a "$BKD/libsysutil.a" "$LIB/libsysutil.a"
    cp -a "$BKD/liblv2.a"     "$LIB/liblv2.a"
    rm -f "$LIB64/libsysutil.a" "$LIB64/liblv2.a"
    echo "  reverted ILP32 libsysutil.a/liblv2.a; removed lp64 additions."
    ;;
  --execute)
    plan; preconds || exit 1
    mkdir -p "$BK"
    cp -a "$LIB/libsysutil.a" "$BK/libsysutil.a"
    cp -a "$LIB/liblv2.a"     "$BK/liblv2.a"
    say "backed up -> $BK"
    make -C "$SRC/sdk" install-headers PS3DK="$PS3DK" || { say "install-headers FAILED — aborting (no flip done)"; exit 1; }
    ln -sf libsysutil_stub.a "$LIB/libsysutil.a"
    mkdir -p "$LIB64"; ln -sf libsysutil_stub.a "$LIB64/libsysutil.a"
    cp -a "$SRC/sdk/liblv2/build/liblv2.a"      "$LIB/liblv2.a"
    cp -a "$SRC/sdk/liblv2/build-lp64/liblv2.a" "$LIB64/liblv2.a"
    say "FLIPPED. Now run: scripts/build-all-samples.sh && scripts/build-all-samples.sh --lp64 && scripts/rpcs3-sweep.sh ..."
    say "REVERT with: $0 --revert $TS --execute"
    ;;
  *)
    plan; preconds
    echo
    echo "This was a DRY RUN. Re-invoke with --execute to apply, after director approval."
    ;;
esac
