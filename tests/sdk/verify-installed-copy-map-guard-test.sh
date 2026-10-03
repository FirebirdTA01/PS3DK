#!/usr/bin/env bash
# Host-side guard tests for scripts/verify-installed-copy-map.sh.
#
# These check only the "misuse" guards (shared-SDK collision, TCHAIN
# escape of SCRATCH via '..' or via a scratch-local symlink, bad argc,
# missing source dirs).  The positive "producer runs and 9 REPLACING
# lines are present" path requires a PPU toolchain and is exercised on
# the WSL acceptance run.
#
# Exit: 0 if every case hits its expected exit code, non-zero on any
# mismatch.  bash-only; no make, no compiler needed.

set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd -P)"
SCRIPT="$ROOT/scripts/verify-installed-copy-map.sh"
TWORK="$(mktemp -d /tmp/verify-installed-copy-map-test.XXXXXX)"
trap 'rm -rf "$TWORK"' EXIT

PASS=0; FAIL=0
chk() {
    local label="$1" got="$2" want="$3"
    if [[ "$got" == "$want" ]]; then
        echo "  PASS  $label (exit $got)"; PASS=$((PASS+1))
    else
        echo "  FAIL  $label (got exit $got, want $want)"; FAIL=$((FAIL+1))
    fi
}
pass() { echo "  PASS  $1"; PASS=$((PASS+1)); }
fail() { echo "  FAIL  $1"; FAIL=$((FAIL+1)); }

echo "=== guard: misuse cases (expect exit 2) ==="
bash "$SCRIPT" a b >/dev/null 2>&1
chk "argc=2" $? 2

# Shared-SDK collisions (case-insensitive /c vs /mnt/c, PS3DK vs ps3dk).
bash "$SCRIPT" /s /c/SDKs/Sony/homebrew/PS3DK "$TWORK/tc" >/dev/null 2>&1
chk "scratch == shared (C:)" $? 2
bash "$SCRIPT" /s /mnt/c/SDKs/Sony/homebrew/PS3DK/x "$TWORK/tc" >/dev/null 2>&1
chk "scratch under shared (/mnt/c)" $? 2
bash "$SCRIPT" /s "$TWORK/sc" /c/SDKs/Sony/homebrew/PS3DK >/dev/null 2>&1
chk "toolchain == shared (C:)" $? 2
bash "$SCRIPT" /s "$TWORK/sc" /mnt/c/SDKs/Sony/homebrew >/dev/null 2>&1
chk "toolchain parent-of-shared (env.sh PS3DK=PS3DEV/ps3dk case)" $? 2

# TCHAIN must be strictly under SCRATCH (resolved -- '..' and symlinks don't work).
mkdir -p "$TWORK/scratch" "$TWORK/outside"
ln -s "$TWORK/outside" "$TWORK/scratch/link-to-outside"
# '..' escape
bash "$SCRIPT" /s "$TWORK/scratch" "$TWORK/scratch/../outside" >/dev/null 2>&1
chk "TCHAIN escapes via .." $? 2
# Symlink escape (the hardening the lead asked for)
bash "$SCRIPT" /s "$TWORK/scratch" "$TWORK/scratch/link-to-outside" >/dev/null 2>&1
chk "TCHAIN escapes via scratch-local symlink" $? 2
# TCHAIN == SCRATCH (not strictly under)
bash "$SCRIPT" /s "$TWORK/scratch" "$TWORK/scratch" >/dev/null 2>&1
chk "TCHAIN == SCRATCH" $? 2

# Source-tree must exist
bash "$SCRIPT" /nonexist "$TWORK/scratch" "$TWORK/scratch/tc" >/dev/null 2>&1
chk "missing source tree" $? 2

# Legitimate layout must clear the containment guard (the reason it fails
# is the missing source, NOT the containment).
out="$(bash "$SCRIPT" /src-that-does-not-exist "$TWORK/scratch" "$TWORK/scratch/tc" 2>&1)"
if grep -q "not strictly under" <<<"$out"; then
    echo "  FAIL  legitimate case rejected by containment"; FAIL=$((FAIL+1))
else
    echo "  PASS  legitimate case clears the containment guard"; PASS=$((PASS+1))
fi

# ---------------------------------------------------------------------------
# Codex P1 (v6): the shared-SDK collision guard must fire on the RESOLVED
# path (a scratch/tc that is a SYMLINK ALIAS that resolves into the shared
# root).  Two defects to close:
#   (a) the alias row must exercise a REAL symlink (test -L + a realpath
#       check), not a directory that `ln -s` happened to land inside of;
#   (b) the row must DISCRIMINATE the guard: pass a VALID source tree,
#       assert the SPECIFIC 'collides with the shared PS3DK install'
#       diagnostic AND exit code 2, and a MUTANT of the script (a sed-edited
#       copy in $TWORK whose guard_dir body is deleted) must make the same
#       input stop emitting that diagnostic -- proving the row passes only
#       because the guard is present.  The mutant is generated in the test,
#       NOT via a production bypass switch (claude P1: a guard-off switch
#       must not ship in the helper).  We verify the sed actually changed
#       the copy (cmp must differ) before trusting the mutant.
# PS3TC_SHARED_ROOTS aliases a scratch-local stand-in for /c alongside the
# production defaults (it only ADDS roots, never removes them).
# ---------------------------------------------------------------------------
echo "=== P1: shared-SDK collision via a SYMLINK ALIAS (expect exit 2 + specific diagnostic) ==="
FAKESHARED="$TWORK/fake-shared"
mkdir -p "$FAKESHARED"
VALID_SRC="$TWORK/valid-src"
mkdir -p "$VALID_SRC/runtime/lv2" "$VALID_SRC/sdk/librsx" "$VALID_SRC/sdk/libgcm_cmd"

# The guard's diagnostic line:
GUARD_DIAG="collides with the shared PS3DK install"
# A helper to run the script with a given scratch+toolchain pair, capturing
# its combined output to a file (stdout+stderr, correctly ordered
# '> file 2>&1') and echoing the exit code.
run_cap() {  # run_cap <out-file> <script> <src> <scratch> <tc>
    local out="$1" script="$2" src="$3" scr="$4" tc="$5"
    PS3TC_SHARED_ROOTS="$FAKESHARED" bash "$script" "$src" "$scr" "$tc" > "$out" 2>&1
}
# The single assertion used by BOTH real alias rows AND the red-path
# self-check: the guard's specific diagnostic must be present AND the exit
# code must be 2.  Factoring it in one place means the red-path self-check
# exercises the REAL fail expression (not a duplicated third one): if the
# fail-branch has a bug (e.g. the v8 ${grep} bad substitution) the self-check
# that calls this same function goes RED instead of silently passing.
assert_guard_row() {
    local label="$1" out="$2" ec="$3"
    if grep -qF "$GUARD_DIAG" "$out" && [[ "$ec" -eq 2 ]]; then
        pass "$label -> specific diagnostic + exit 2 (got $ec)"
        return 0
    fi
    fail "$label -> diagnostic=$(grep -qF "$GUARD_DIAG" "$out" && echo present || echo absent), exit=$ec (want diagnostic+exit 2)"
    return 1
}

# ---------------------------------------------------------------------------
# Build TWO mutants, both in $TWORK (NEVER a production bypass -- a guard-off
# switch must not ship in the helper; this is what the sed-copy is for):
#
#   MUT1 -- neutralises the guard's DIAGNOSTIC ECHO only.  The guard still
#           runs its decision and still `exit 2`s, but the message is gone.
#           Purpose: prove the alias rows' grep for the specific diagnostic
#           is HONEST -- they key on the guard's exact string, not "any
#           exit 2".
#
#   MUT2 -- disables the GUARD ITSELF: the two `guard_dir ...` CALLS are
#           sed-removed (the decision is never made).  Purpose: prove the
#           exit-2/diagnostic comes FROM THE GUARD -- with the guard's
#           decision gone the alias rows must go RED (the guard-specific
#           diagnostic disappears), even if some OTHER check also exits 2
#           for these inputs.  This is the stronger mutant: it is not a
#           silencing of the message, it is the removal of the guard.
#
# For both we verify with cmp that the copy actually differs from the shipped
# script (a no-op sed would otherwise make the mutant identical to the
# guarded version and the discrimination meaningless) and grep that the
# thing we meant to cut is actually gone.
# ---------------------------------------------------------------------------
make_mutant() {  # make_mutant <copy> <sed-prog> <must-be-gone-string>
    local copy="$1" sed_prog="$2" must_gone="$3"
    cp "$SCRIPT" "$copy"
    sed -i.bak "$sed_prog" "$copy"
    if cmp -s "$SCRIPT" "$copy"; then
        fail "mutant: $copy identical to shipped script (sed did not match)"
        return 1
    fi
    if grep -qF "$must_gone" "$copy"; then
        fail "mutant: $copy still contains '$must_gone' (sed did not neutralise it)"
        return 1
    fi
    pass "mutant: $copy differs from shipped + target string gone (cmp+grep confirm)"
    return 0
}
MUT1="$TWORK/mut1-echo.sh";        MUT1_OK=0
MUT2="$TWORK/mut2-guardoff.sh";    MUT2_OK=0
# MUT1: neutralise the diagnostic ECHO only.  The guard still decides and
# still exits 2, so this only proves the grep for the message is honest.
make_mutant "$MUT1" \
    's|REFUSE -- \$label collides with the shared PS3DK install|MUTANT_SENTINEL guard-echo-neutralised|' \
    "collides with the shared PS3DK install" && MUT1_OK=1
# MUT2: the GUARD ITSELF is disabled -- both guard_dir CALLS are removed so
# the decision never happens.  Alias rows must go RED (diagnostic disappears)
# even if some other check also exits 2.  This is the stronger mutant.
cp "$SCRIPT" "$MUT2"
sed -i.bak \
    -e 's|^guard_dir "scratch prefix (arg2).*$|: # MUTANT_SENTINEL scratch guard_dir call removed|' \
    -e 's|^guard_dir "toolchain prefix (arg3).*$|: # MUTANT_SENTINEL toolchain guard_dir call removed|' \
    "$MUT2"
if cmp -s "$SCRIPT" "$MUT2"; then
    fail "mutant MUT2 identical to shipped script (seds did not match)"
elif grep -qF 'guard_dir "scratch prefix (arg2)"' "$MUT2" ||
     grep -qF 'guard_dir "toolchain prefix (arg3)"' "$MUT2"; then
    fail "mutant MUT2 still contains a live guard_dir call"
else
    pass "mutant MUT2: both guard_dir calls removed (cmp+grep confirm)"
    MUT2_OK=1
fi

# --- scratch side: alias-scr/link -> fake-shared (a real, followable symlink) ---
SCR_ALIAS="$TWORK/alias-scr"
mkdir -p "$SCR_ALIAS"
rm -f "$SCR_ALIAS/link"
ln -s "$FAKESHARED" "$SCR_ALIAS/link"
test -L "$SCR_ALIAS/link" \
    && pass "scratch-alias symlink is real (test -L)" \
    || fail "scratch-alias symlink is real (test -L)"
# Row (guard ON): the diagnostic must be present AND exit code 2.
run_cap "$TWORK/r-scr.txt" "$SCRIPT" "$VALID_SRC" "$SCR_ALIAS/link" "$SCR_ALIAS/link/tc"
assert_guard_row "scratch aliases shared root" "$TWORK/r-scr.txt" "$?"
# Row 2a (MUT1, echo neutralised): the guard still decides and still exits,
# so the diagnostic (grep target) must be ABSENT while the guard's decision
# path is otherwise unchanged.  Proves the grep keys on the exact string.
if [[ "$MUT1_OK" -eq 1 ]]; then
    run_cap "$TWORK/m1-scr.txt" "$MUT1" "$VALID_SRC" "$SCR_ALIAS/link" "$SCR_ALIAS/link/tc"
    if grep -qF "$GUARD_DIAG" "$TWORK/m1-scr.txt"; then
        fail "mutant MUT1 (scratch): diagnostic STILL present (grep not honest)"
    else
        pass "mutant MUT1 (scratch): diagnostic ABSENT when echo neutralised"
    fi
fi
# Row 2b (MUT2, guard itself disabled): the guard's decision is gone, so the
# shared-collision diagnostic must be ABSENT even though OTHER checks may
# still exit 2.  Proves the exit/diagnostic came FROM the guard.
if [[ "$MUT2_OK" -eq 1 ]]; then
    run_cap "$TWORK/m2-scr.txt" "$MUT2" "$VALID_SRC" "$SCR_ALIAS/link" "$SCR_ALIAS/link/tc"
    if grep -qF "$GUARD_DIAG" "$TWORK/m2-scr.txt"; then
        fail "mutant MUT2 (scratch): diagnostic STILL present (guard not what fired)"
    else
        pass "mutant MUT2 (scratch): diagnostic ABSENT when the guard itself is removed"
    fi
fi

# --- toolchain side: real-scr is a normal dir; ONLY tc is a symlink alias.
#     (Codex defect 1: do NOT 'mkdir tc' first -- tc must be a symlink, not a
#      real dir, or the alias is never exercised.) ---
TC_ALIAS="$TWORK/tc-alias"
mkdir -p "$TC_ALIAS"                 # the scratch itself is a real dir
rm -f "$TC_ALIAS/tc"
ln -s "$FAKESHARED" "$TC_ALIAS/tc"    # tc is NOW a genuine symlink alias
test -L "$TC_ALIAS/tc" \
    && pass "toolchain-alias symlink is real (test -L)" \
    || fail "toolchain-alias symlink is real (test -L)"
[[ "$(realpath -m "$TC_ALIAS/tc")" == "$(realpath -m "$FAKESHARED")" ]] \
    && pass "toolchain-alias realpath resolves to the shared root" \
    || fail "toolchain-alias realpath resolves to the shared root"
# Row (guard ON): diagnostic + exit 2.
run_cap "$TWORK/r-tc.txt" "$SCRIPT" "$VALID_SRC" "$TC_ALIAS" "$TC_ALIAS/tc"
assert_guard_row "toolchain aliases shared root" "$TWORK/r-tc.txt" "$?"
# Row 4a (MUT1, echo neutralised): diagnostic must be ABSENT (grep honest).
if [[ "$MUT1_OK" -eq 1 ]]; then
    run_cap "$TWORK/m1-tc.txt" "$MUT1" "$VALID_SRC" "$TC_ALIAS" "$TC_ALIAS/tc"
    if grep -qF "$GUARD_DIAG" "$TWORK/m1-tc.txt"; then
        fail "mutant MUT1 (toolchain): diagnostic STILL present (grep not honest)"
    else
        pass "mutant MUT1 (toolchain): diagnostic ABSENT when echo neutralised"
    fi
fi
# Row 4b (MUT2, guard itself disabled): diagnostic must be ABSENT (guard is
# what fired, not some other check).
if [[ "$MUT2_OK" -eq 1 ]]; then
    run_cap "$TWORK/m2-tc.txt" "$MUT2" "$VALID_SRC" "$TC_ALIAS" "$TC_ALIAS/tc"
    if grep -qF "$GUARD_DIAG" "$TWORK/m2-tc.txt"; then
        fail "mutant MUT2 (toolchain): diagnostic STILL present (guard not what fired)"
    else
        pass "mutant MUT2 (toolchain): diagnostic ABSENT when the guard itself is removed"
    fi
fi

# ---------------------------------------------------------------------------
# Codex P1: a failing producer step must fail the verification (exit 1)
# even when all nine REPLACING lines are present.  We use a stub source
# tree where build-runtime-lv2.sh emits BOTH of the librt lines and then
# exits non-zero, and a stub make that emits the other seven lines and
# exits zero.  With the current broken script this would PASS (exit 0);
# with the fix it must FAIL (exit 1).
# ---------------------------------------------------------------------------
echo "=== P1: failing producer step must fail the verdict (expect exit 1) ==="
T="$TWORK/failproducer"
mkdir -p "$T/src/scripts" "$T/src/runtime/lv2" "$T/src/sdk"
# build-runtime-lv2.sh: stub that emits the 2 librt REPLACING lines and exits 1.
cat > "$T/src/scripts/build-runtime-lv2.sh" <<'EOF'
#!/usr/bin/env bash
echo "REPLACING ppu/lib/librt.a with runtime/lv2/librt (vendored copy at ppu/lib/librt.a is NOT installed)"
echo "REPLACING ppu/lib/lp64/librt.a with runtime/lv2/librt (vendored copy at ppu/lib/lp64/librt.a is NOT installed)"
exit 1
EOF
cat > "$T/src/scripts/env.sh" <<'EOF'
#!/usr/bin/env bash
:
EOF
# sdk/Makefile: directory marker.  Inert for this test -- the stub make in
# the toolchain ppu/bin shadows the real make, so make never reads this file.
cat > "$T/src/sdk/Makefile" <<'EOF'
# inert -- shadowed by the stub make in the toolchain ppu/bin
EOF
# sdk/librsx and libgcm_cmd: directories.  Their Makefiles are DEAD CODE
# for this test: the script prepends $TCHAIN/ppu/bin to PATH, where we
# install a stub `make` that shadows the real one, so make never reads
# these Makefiles.  They exist only to satisfy the script's directory
# existence checks ([ -d "$SRC/sdk/librsx" ] etc).
mkdir -p "$T/src/sdk/librsx" "$T/src/sdk/libgcm_cmd"
cat > "$T/src/sdk/librsx/Makefile" <<'EOF'
# inert -- shadowed by the stub make in the toolchain ppu/bin
EOF
cat > "$T/src/sdk/libgcm_cmd/Makefile" <<'EOF'
# inert -- shadowed by the stub make in the toolchain ppu/bin
EOF
# toolchain: stub compiler so the source-tree existence check passes.
mkdir -p "$T/scratch/tc/ppu/bin"
printf '#!/bin/bash\nexit 0\n' > "$T/scratch/tc/ppu/bin/powerpc64-ps3-elf-gcc"
chmod +x "$T/scratch/tc/ppu/bin/powerpc64-ps3-elf-gcc"
# make stub: emits the 7 make-produced REPLACING lines and exits zero.
# The real `make` is not required on the test host -- the stub stands in
# for it, and the fake source tree is not a real SDK (this test is about
# the script's OWN verdict logic, not a real build).  The script prepends
# the toolchain ppu/bin (where the stub compiler lives) to PATH, so we
# put this stub `make` there too.
cat > "$T/scratch/tc/ppu/bin/make" <<'EOF'
#!/bin/bash
# stub make: emit the 7 make-produced REPLACING lines; ignore -C / target.
for a in "$@"; do :; done
echo "REPLACING ppu/include/rsx/rsx_function_macros.h with sdk/include/rsx (vendored copy at ppu/include/rsx/rsx_function_macros.h is NOT installed)"
echo "REPLACING ppu/include/rsx/gcm_sys.h with sdk/include/rsx (vendored copy at ppu/include/rsx/gcm_sys.h is NOT installed)"
echo "REPLACING ppu/include/net/socket.h with sdk/include/net (vendored copy at ppu/include/net/socket.h is NOT installed)"
echo "REPLACING ppu/lib/librsx.a with sdk/librsx (vendored copy at ppu/lib/librsx.a is NOT installed)"
echo "REPLACING ppu/lib/lp64/librsx.a with sdk/librsx (vendored copy at ppu/lib/lp64/librsx.a is NOT installed)"
echo "REPLACING ppu/lib/libgcm_cmd.a with sdk/libgcm_cmd (vendored copy at ppu/lib/libgcm_cmd.a is NOT installed)"
echo "REPLACING ppu/lib/lp64/libgcm_cmd.a with sdk/libgcm_cmd (vendored copy at ppu/lib/lp64/libgcm_cmd.a is NOT installed)"
exit 0
EOF
chmod +x "$T/scratch/tc/ppu/bin/make"

bash "$SCRIPT" "$T/src" "$T/scratch" "$T/scratch/tc" > "$TWORK/vfail.log" 2>&1
ec=$?
all9=$(grep -c '^REPLACING' "$T/scratch/verify.log" 2>/dev/null || echo 0)
distinct=$(grep '^REPLACING' "$T/scratch/verify.log" 2>/dev/null | sort -u | wc -l | tr -d ' ')
echo "  REPLACING lines in log: ${distinct} unique (expect 9 total across 4 producer steps; log may have duplicates from repeated runs)"
echo "  verify exit = ${ec} (want 1)"
if [[ "$ec" == "1" && "$distinct" -ge 9 ]]; then
    echo "  PASS  failing producer step -> exit 1 even with all 9 unique lines present"; PASS=$((PASS+1))
else
    echo "  FAIL  failing producer step produced exit ${ec} (want 1), unique=${distinct}"; FAIL=$((FAIL+1))
fi

# Control: the SAME structure with build-runtime-lv2.sh EXIT 0 (a successful
# producer) must PASS (exit 0) with all 9 unique lines present.  This pins
# the green side so we are not only testing the red path.
echo "=== control: succeeding producer must PASS (expect exit 0) ==="
TS="$TWORK/successproducer"
mkdir -p "$TS/src/scripts" "$TS/src/runtime/lv2" "$TS/src/sdk/librsx" "$TS/src/sdk/libgcm_cmd"
cat > "$TS/src/scripts/build-runtime-lv2.sh" <<'EOF'
#!/usr/bin/env bash
echo "REPLACING ppu/lib/librt.a with runtime/lv2/librt (vendored copy at ppu/lib/librt.a is NOT installed)"
echo "REPLACING ppu/lib/lp64/librt.a with runtime/lv2/librt (vendored copy at ppu/lib/lp64/librt.a is NOT installed)"
exit 0
EOF
printf ': # env\n' > "$TS/src/scripts/env.sh"
cat > "$TS/src/sdk/Makefile" <<'EOF'
# inert
EOF
printf '# inert\n' > "$TS/src/sdk/librsx/Makefile"
printf '# inert\n' > "$TS/src/sdk/libgcm_cmd/Makefile"
mkdir -p "$TS/scratch/tc/ppu/bin"
printf '#!/bin/bash\nexit 0\n' > "$TS/scratch/tc/ppu/bin/powerpc64-ps3-elf-gcc"
chmod +x "$TS/scratch/tc/ppu/bin/powerpc64-ps3-elf-gcc"
cat > "$TS/scratch/tc/ppu/bin/make" <<'EOF'
#!/bin/bash
echo "REPLACING ppu/include/rsx/rsx_function_macros.h with sdk/include/rsx (vendored copy at ppu/include/rsx/rsx_function_macros.h is NOT installed)"
echo "REPLACING ppu/include/rsx/gcm_sys.h with sdk/include/rsx (vendored copy at ppu/include/rsx/gcm_sys.h is NOT installed)"
echo "REPLACING ppu/include/net/socket.h with sdk/include/net (vendored copy at ppu/include/net/socket.h is NOT installed)"
echo "REPLACING ppu/lib/librsx.a with sdk/librsx (vendored copy at ppu/lib/librsx.a is NOT installed)"
echo "REPLACING ppu/lib/lp64/librsx.a with sdk/librsx (vendored copy at ppu/lib/lp64/librsx.a is NOT installed)"
echo "REPLACING ppu/lib/libgcm_cmd.a with sdk/libgcm_cmd (vendored copy at ppu/lib/libgcm_cmd.a is NOT installed)"
echo "REPLACING ppu/lib/lp64/libgcm_cmd.a with sdk/libgcm_cmd (vendored copy at ppu/lib/lp64/libgcm_cmd.a is NOT installed)"
exit 0
EOF
chmod +x "$TS/scratch/tc/ppu/bin/make"
bash "$SCRIPT" "$TS/src" "$TS/scratch" "$TS/scratch/tc" > "$TWORK/void.log" 2>&1
ec=$?
distinct=$(grep '^REPLACING' "$TS/scratch/verify.log" 2>/dev/null | sort -u | wc -l | tr -d ' ')
if [[ "$ec" == "0" && "$distinct" -ge 9 ]]; then
    echo "  PASS  successful producer -> exit 0 with all 9 unique lines"; PASS=$((PASS+1))
else
    echo "  FAIL  successful producer produced exit ${ec} (want 0), unique=${distinct}"; FAIL=$((FAIL+1))
fi

# ---------------------------------------------------------------------------
# Red-path self-check (claude P1 / codex v8/v9): the two real alias rows call
# assert_guard_row(), whose RED branch runs fail().  A v8-style ${grep ...}
# bad substitution in that branch made fail() never execute and FAIL not rise,
# so a red row still printed "RESULT ... 0 failed" (a false green).  To close
# that WITHOUT duplicating the fail expression (codex v9: the self-check must
# exercise the REAL assertion, not a third copy), the self-check calls the
# SAME assert_guard_row() with a RED input (diagnostic absent, exit 0).
#
# A bad substitution inside the call is FATAL -- it would abort the whole
# script before the delta check, which is how a false green slips by.  We
# therefore run the call in a subshell: if the shared RED path executes,
# FAIL (exported) increments by one and we require EXACTLY that (delta == 1)
# plus a non-zero counter before restoring it; if the shared RED path is
# broken (bad substitution, etc.) the subshell dies, FAIL is untouched,
# delta is 0, and this row goes RED.  Either way the self-check is the
# thing that turns the planted bug red.  The planted red is expected and
# accounted for, so we restore the counter afterwards.
# ---------------------------------------------------------------------------
echo "=== red-path self-check: assert_guard_row()'s RED path must increment FAIL exactly once ==="
save_fail=$FAIL
fake="$TWORK/selfcheck-red.txt"
echo "no guard diagnostic here" > "$fake"
newfail_file="$TWORK/selfcheck-fail-count"
rm -f "$newfail_file"
# Run the SHARED assertion (the exact code the real rows call) in a subshell.
# - GOOD red path: fail() increments FAIL to save_fail+1; the subshell then
#   records that count to $newfail_file.
# - BROKEN red path (a v8-style ${...} bad substitution): the error is FATAL
#   to the subshell, it aborts before recording the count -> no file.
# Either way the parent survives and can decide: file present + delta==1 is
# the only green; anything else (no file, wrong delta) is a false green.
(
    FAIL=$save_fail
    set +e            # a clean red return (0) must not abort; only a fatal error aborts
    assert_guard_row "self-check red row" "$fake" 0
    # Only reached if assert_guard_row did NOT abort on a bad substitution:
    printf '%s\n' "$FAIL" > "$newfail_file"
    exit 0
) >/dev/null 2>&1
delta=0
recorded=""
[[ -f "$newfail_file" ]] && recorded="$(cat "$newfail_file" 2>/dev/null)"
if [[ -n "$recorded" ]]; then
    delta=$((recorded - save_fail))
fi
if [[ "$delta" -eq 1 && "$recorded" -gt 0 ]]; then
    pass "red-path self-check: shared RED path incremented FAIL exactly once (delta $delta; recorded count $recorded non-zero)"
else
    # A genuine regression in THIS test: the shared fail branch is not
    # counting.  Count it as a real failure (do NOT restore an unexpected value).
    echo "  FAIL  red-path self-check: shared RED path did not yield exactly one increment (recorded=${recorded:-<none>}, delta=${delta}) -- the shared fail branch is a false green (or crashed on a bad substitution)"
    FAIL=$((FAIL + 1))
fi

echo
echo "RESULT: $PASS passed, $FAIL failed"
[[ "$FAIL" -eq 0 ]]
