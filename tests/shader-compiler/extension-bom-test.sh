#!/usr/bin/env bash
# t_f16682d5: the leading UTF-8 byte order mark extension, --extension=bom.
#
# The reference compiler refuses a source that begins with EF BB BF, and so
# do we by default.  With --extension=bom exactly one leading mark is
# accepted at BOTH points a file enters the compiler - the file on the
# command line and an #include - and the result is the bytes of the same
# source without the mark.  That equality is the whole safety argument: an
# enabled extension is admissible only where it reduces to something the
# reference already covers, and the plain control IS that form, so no
# reference call is needed here.
#
# Two guards matter more than the acceptance rows:
#
#   The include rows and their mirror (BOM on the include with a plain main,
#   BOM on the main with a plain include) are the WIRING guard.  The include
#   read site gets the rule through a hook the driver installs; with the hook
#   not installed the extension does not reach includes - flag off, an
#   include's BOM gets the lexer's generic error with no hint; flag on, it is
#   not stripped and the compile fails where it should have passed - and
#   nothing in the type system says so.  Do not delete either row as
#   redundant with the other.
#
#   The leading U+00A9 rows are the HINT guard.  Before this existed a BOM
#   and a copyright sign produced the same unknown-character error at the
#   same place, so a hint hung off that error would name --extension=bom for
#   a file that has no BOM.  The director ruled that out: a hint comes only
#   from a detector written for the construct.  Those rows require NO
#   extension flag anywhere in stderr.
#
# Every refusal asserts exit status exactly 1 (a timeout or crash is not a
# refusal), NO output artifact (an empty file is an artifact), the located
# prefix, the diagnostic body, and the presence or absence of the flag
# token.  The flag the diagnostic prints is fed back to the compiler as an
# argument: a hint that cannot be pasted is worse than none.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
compiler="${1:-${RSX_CG_COMPILER:-}}"
fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }

if [[ -z "$compiler" ]]; then
    compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
fi
[[ -x "$compiler" ]] || fail "rsx-cg-compiler not executable: $compiler"

work="${TMPDIR:-/tmp}/ps3dk-extension-bom-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

fx="$repo_root/tools/rsx-cg-compiler/tests/shaders/extensions"
FLAG='--extension=bom'   # the director's spelling, pinned as text

# ---------------------------------------------------------------------------
# Fixture integrity.  A normalisation that stripped the marks would turn every
# row below into a plain compile and the test would pass while proving nothing.
first_bytes() { head -c "$2" "$1" | od -An -tx1 | tr -d ' \n'; }
size_of() { wc -c < "$1" | tr -d ' '; }
for f in bom_f.cg bom_double_f.cg bom_both_f.cg bom_only_f.cg inc_bom.h inc_bom_outer.h inc_bom_only.h; do
    [[ "$(first_bytes "$fx/$f" 3)" == efbbbf ]] || fail "$f has lost its leading BOM in the tree - the fixture is inert"
done
[[ "$(first_bytes "$fx/bom_double_f.cg" 6)" == efbbbfefbbbf ]] || fail "bom_double_f.cg does not begin with two marks"
[[ "$(first_bytes "$fx/lead_copyright_f.cg" 2)" == c2a9 ]] || fail "lead_copyright_f.cg has lost its leading U+00A9"
[[ "$(first_bytes "$fx/bom_midfile_f.cg" 3)" != efbbbf ]] || fail "bom_midfile_f.cg begins with a BOM; its mark belongs mid-file"
od -An -tx1 "$fx/bom_midfile_f.cg" | tr -d ' \n' | grep -q efbbbf || fail "bom_midfile_f.cg has lost its mid-file BOM"
od -An -tx1 "$fx/bom_comment_f.cg" | tr -d ' \n' | grep -q efbbbf || fail "bom_comment_f.cg has lost the BOM inside its comment"
for f in bom_control_f.cg bom_include_control_f.cg bom_comment_f.cg bom_inactive_include_f.cg inc_plain.h; do
    [[ "$(first_bytes "$fx/$f" 3)" != efbbbf ]] || fail "$f is a control and must not begin with a BOM"
done
[[ "$(size_of "$fx/bom_only_f.cg")" == 3 && "$(size_of "$fx/inc_bom_only.h")" == 3 ]] || fail "the BOM-only fixtures must be exactly three bytes"
[[ "$(size_of "$fx/empty_control_f.cg")" == 0 && "$(size_of "$fx/inc_empty.h")" == 0 ]] || fail "the empty controls must be zero bytes"
# Near-misses: prefixes of the mark, a UTF-16 mark, and files that are only
# a partial mark.  Each must still carry exactly the bytes it is named for.
[[ "$(first_bytes "$fx/lead_ef_f.cg" 2)" == ef66 ]] || fail "lead_ef_f.cg must begin EF then source"
[[ "$(first_bytes "$fx/lead_efbb_f.cg" 3)" == efbb66 ]] || fail "lead_efbb_f.cg must begin EF BB then source"
[[ "$(first_bytes "$fx/lead_efbb41_f.cg" 3)" == efbb41 ]] || fail "lead_efbb41_f.cg must begin EF BB 41"
[[ "$(first_bytes "$fx/lead_utf16be_f.cg" 2)" == feff ]] || fail "lead_utf16be_f.cg must begin FE FF"
[[ "$(first_bytes "$fx/lead_utf16le_f.cg" 2)" == fffe ]] || fail "lead_utf16le_f.cg must begin FF FE"
[[ "$(size_of "$fx/only_ef_f.cg")" == 1 && "$(first_bytes "$fx/only_ef_f.cg" 1)" == ef ]] || fail "only_ef_f.cg must be exactly the byte EF"
[[ "$(size_of "$fx/only_efbb_f.cg")" == 2 && "$(first_bytes "$fx/only_efbb_f.cg" 2)" == efbb ]] || fail "only_efbb_f.cg must be exactly EF BB"
printf '  %-40s ok\n' "fixture bytes intact"

# ---------------------------------------------------------------------------
# run <compiler> <tag> <mode off|on> <profile> <stem> [extra args...]
#   exit status in $rc; container path $work/<tag>.fpo (fresh per run);
#   stderr+stdout at $work/<tag>.log.
run() {
    local cc="$1" tag="$2" mode="$3" profile="$4" stem="$5"; shift 5
    local -a flags=()
    [[ "$mode" == on ]] && flags=("$FLAG")
    rc=0
    rm -f "$work/$tag.fpo"
    (
        ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
        timeout "${PS3TC_SHADER_TEST_TIMEOUT:-20s}" "$cc" -p "$profile" -I "$fx" \
            "${flags[@]}" "$@" --emit-container "$work/$tag.fpo" "$fx/$stem"
    ) >"$work/$tag.log" 2>&1 || rc=$?
    [[ "$rc" -eq 124 ]] && fail "$tag timed out - a hang is not a refusal"
    return 0
}

accept() {  # <tag> <mode> <stem>  -> compiled, container present
    run "$compiler" "$1" "$2" sce_fp_rsx "$3"
    [[ "$rc" -eq 0 ]] || { tail -n 4 "$work/$1.log" >&2; fail "$1: $3 with the extension $2 exited $rc, expected 0"; }
    [[ -s "$work/$1.fpo" ]] || fail "$1: $3 compiled but wrote no container"
}
same_bytes() {  # <tag a> <tag b> <why>
    cmp -s "$work/$1.fpo" "$work/$2.fpo" || fail "$3 ($1 vs $2 differ)"
}

# check_refusal <compiler> <tag> <mode> <profile> <stem> <hint yes|no> <located prefix> <body>
#   echoes a problem, or nothing.  ONE function for every refusal row and for
#   the self-check stubs, so what the stubs prove is what the rows assert.
check_refusal() {
    local cc="$1" tag="$2" mode="$3" profile="$4" stem="$5" hint="$6" prefix="$7" body="$8"
    run "$cc" "$tag" "$mode" "$profile" "$stem"
    if [[ "$rc" -ne 1 ]]; then printf 'exited %s, expected exactly 1' "$rc"; return 0; fi
    if [[ -e "$work/$tag.fpo" ]]; then printf 'refused but left an output artifact'; return 0; fi
    if ! grep -qF -- "$prefix" "$work/$tag.log"; then printf "no located diagnostic '%s'" "$prefix"; return 0; fi
    if ! grep -qF -- "$body" "$work/$tag.log"; then printf "stderr does not contain '%s'" "$body"; return 0; fi
    if [[ "$hint" == yes ]]; then
        if ! grep -qF -- "$FLAG" "$work/$tag.log"; then printf 'does not name %s' "$FLAG"; return 0; fi
    else
        if grep -q -- '--extension' "$work/$tag.log"; then printf 'names an extension flag from an unrelated error'; return 0; fi
    fi
}
refuse() {  # <label> <tag> <mode> <profile> <stem> <hint> <prefix> <body>
    local label="$1"; shift
    local problem
    problem="$(check_refusal "$compiler" "$@")"
    [[ -z "$problem" ]] || { head -n 4 "$work/$1.log" >&2; fail "$label: $problem"; }
    printf '  %-40s refused\n' "$label"
}
BOM_BODY='byte order mark'
UNK_BODY='unknown character'

# ---------------------------------------------------------------------------
# Controls compile in both modes to the same bytes: an enabled extension is
# inert where its construct is absent.
accept ctl_off off bom_control_f.cg
accept ctl_on  on  bom_control_f.cg
same_bytes ctl_off ctl_on "the plain control changed bytes when the extension was enabled"
accept ictl_off off bom_include_control_f.cg
accept ictl_on  on  bom_include_control_f.cg
same_bytes ictl_off ictl_on "the plain include control changed bytes when the extension was enabled"
printf '  %-40s ok\n' "controls inert under the flag"

# Main source.
refuse "off: leading BOM"        bom_off off sce_fp_rsx bom_f.cg yes 'bom_f.cg:1:1: error:' "$BOM_BODY"
accept bom_on on bom_f.cg
same_bytes bom_on ctl_off "on: a BOM-prefixed source must compile to the bytes of the same source without the mark"
printf '  %-40s == control\n' "on: leading BOM"

# The hint guard: a leading byte that is NOT a BOM gets the ordinary error
# and no hint, in both modes.
refuse "off: leading U+00A9, no hint" cp_off off sce_fp_rsx lead_copyright_f.cg no 'lead_copyright_f.cg:1:1: error:' "$UNK_BODY"
refuse "on:  leading U+00A9, no hint" cp_on  on  sce_fp_rsx lead_copyright_f.cg no 'lead_copyright_f.cg:1:1: error:' "$UNK_BODY"

# Near-misses that a WRONG DETECTOR passes and U+00A9 does not catch: the
# first one and two bytes of the mark, the mark with a wrong third byte, a
# UTF-16 mark (out of scope: no detector, no hint), and files that are
# nothing but one or two bytes of the mark - a three-byte compare against a
# shorter buffer is the classic read past the end.  All refuse in both
# modes with the generic error and no hint.
for near in lead_ef lead_efbb lead_efbb41 lead_utf16be lead_utf16le only_ef only_efbb; do
    refuse "off: $near, no hint" "${near}_off" off sce_fp_rsx "${near}_f.cg" no "${near}_f.cg:1:1: error:" "$UNK_BODY"
    refuse "on:  $near, no hint" "${near}_on"  on  sce_fp_rsx "${near}_f.cg" no "${near}_f.cg:1:1: error:" "$UNK_BODY"
done

# Exactly ONE leading mark is the extension.  Disabled, the leading mark is
# what is reported (honest: the extension is present and off).  Enabled, the
# first is consumed and the second is an ordinary error at 1:1 with NO hint,
# because enabling the flag again would not help.
refuse "off: double BOM"                 dbl_off off sce_fp_rsx bom_double_f.cg yes 'bom_double_f.cg:1:1: error:' "$BOM_BODY"
refuse "on:  double BOM, second refused" dbl_on  on  sce_fp_rsx bom_double_f.cg no  'bom_double_f.cg:1:1: error:' "$UNK_BODY"

# A mark that is not at byte 0 is not this extension in either mode.
refuse "off: mid-file BOM, no hint" mid_off off sce_fp_rsx bom_midfile_f.cg no 'bom_midfile_f.cg:2:1: error:' "$UNK_BODY"
refuse "on:  mid-file BOM, no hint" mid_on  on  sce_fp_rsx bom_midfile_f.cg no 'bom_midfile_f.cg:2:1: error:' "$UNK_BODY"

# A BOM inside a comment compiled before this existed and must keep doing so:
# comment text is not a leading mark.
accept cmt_off off bom_comment_f.cg
accept cmt_on  on  bom_comment_f.cg
same_bytes cmt_off ctl_off "off: a BOM inside a comment changed the bytes"
same_bytes cmt_on  ctl_off "on: a BOM inside a comment changed the bytes"
printf '  %-40s == control\n' "both: BOM inside a comment"

# Includes - the wiring guard.  Plain main + BOM include, BOM main + plain
# include (bom_both has both), and a BOM'd header that itself includes a
# BOM'd header.
refuse "off: BOM on an include"  inc_off off sce_fp_rsx bom_include_f.cg yes 'inc_bom.h:1:1: error:' "$BOM_BODY"
accept inc_on on bom_include_f.cg
same_bytes inc_on ictl_off "on: a BOM on an include must compile to the plain-include bytes"
printf '  %-40s == control\n' "on: BOM on an include"
refuse "off: BOM on a nested include" nest_off off sce_fp_rsx bom_nested_include_f.cg yes 'inc_bom_outer.h:1:1: error:' "$BOM_BODY"
accept nest_on on bom_nested_include_f.cg
same_bytes nest_on ictl_off "on: nested BOM'd includes must compile to the plain-include bytes"
printf '  %-40s == control\n' "on: nested BOM includes"
refuse "off: BOM on main AND include" both_off off sce_fp_rsx bom_both_f.cg yes 'bom_both_f.cg:1:1: error:' "$BOM_BODY"
accept both_on on bom_both_f.cg
same_bytes both_on ictl_off "on: BOM on main and include must compile to the plain bytes"
printf '  %-40s == control\n' "on: BOM on main and include"

# An include inside an inactive conditional is never read, so its BOM is
# never seen: this pins the detector to the READ, not to a scan of the text.
accept inact_off off bom_inactive_include_f.cg
accept inact_on  on  bom_inactive_include_f.cg
same_bytes inact_off ctl_off "off: a BOM'd include under #if 0 changed the bytes or was refused"
same_bytes inact_on  ctl_off "on: a BOM'd include under #if 0 changed the bytes"
printf '  %-40s == control\n' "both: BOM include under #if 0"

# A file that is ONLY a mark.  Off: the BOM refusal.  On: the mark is
# consumed and what remains is an empty file, which must behave EXACTLY as
# the empty control does - same exit, same artifact presence, same diagnostic
# up to the file name.  What an empty file does is not this test's business;
# that it is the same in both spellings is.
refuse "off: BOM-only main"    only_off  off sce_fp_rsx bom_only_f.cg yes 'bom_only_f.cg:1:1: error:' "$BOM_BODY"
refuse "off: BOM-only include" ionly_off off sce_fp_rsx bom_only_include_f.cg yes 'inc_bom_only.h:1:1: error:' "$BOM_BODY"
# check_empty_pair <compiler> <tag> <stem> <control tag> <control stem> <sed name substitution> <body>
#   echoes a problem, or nothing.  Equality alone certifies nothing - two
#   identical crashes are equal - so each run is first held to what an empty
#   file does today: exit exactly 1, no artifact, the expected body, and no
#   extension hint.  Only then are the diagnostics compared, both through the
#   same normalisation (a Windows-hosted build writes CRLF) with only the
#   known differing file names substituted.
check_empty_pair() {
    local cc="$1" tag="$2" stem="$3" ctag="$4" cstem="$5" subst="$6" body="$7" crc
    run "$cc" "$ctag" off sce_fp_rsx "$cstem"; crc=$rc
    run "$cc" "$tag"  on  sce_fp_rsx "$stem"
    if [[ "$crc" -ne 1 ]]; then printf 'the plain empty control exited %s, expected exactly 1' "$crc"; return 0; fi
    if [[ "$rc" -ne 1 ]]; then printf 'exited %s, expected exactly 1' "$rc"; return 0; fi
    if [[ -e "$work/$ctag.fpo" ]]; then printf 'the plain empty control left an output artifact'; return 0; fi
    if [[ -e "$work/$tag.fpo" ]]; then printf 'refused but left an output artifact'; return 0; fi
    if ! grep -qF -- "$body" "$work/$ctag.log"; then printf "the plain empty control's stderr does not contain '%s'" "$body"; return 0; fi
    if ! grep -qF -- "$body" "$work/$tag.log"; then printf "stderr does not contain '%s'" "$body"; return 0; fi
    if grep -q -- '--extension' "$work/$tag.log"; then printf 'an enabled compile named an extension flag'; return 0; fi
    tr -d '\r' < "$work/$tag.log" | sed -e "$subst" > "$work/$tag.norm"
    tr -d '\r' < "$work/$ctag.log" > "$work/$ctag.norm"
    if ! cmp -s "$work/$tag.norm" "$work/$ctag.norm"; then
        diff "$work/$ctag.norm" "$work/$tag.norm" >&2 || true
        printf 'diagnostics differ from the plain empty control (after renaming)'; return 0
    fi
}
same_as_control() {  # <label> <tag> <stem> <control tag> <control stem> <subst> <body>
    local label="$1"; shift
    local problem
    problem="$(check_empty_pair "$compiler" "$@")"
    [[ -z "$problem" ]] || { head -n 4 "$work/$1.log" >&2; fail "$label: $problem"; }
    printf '  %-40s == control (exit 1, no artifact)\n' "$label"
}
EMPTY_MAIN_BODY="entry point 'main' not found"
EMPTY_INC_BODY='Failed to read include file'
same_as_control "on: BOM-only main"    only_on  bom_only_f.cg         empty_ctl  empty_control_f.cg         's/bom_only_f/empty_control_f/g' "$EMPTY_MAIN_BODY"
same_as_control "on: BOM-only include" ionly_on bom_only_include_f.cg iempty_ctl empty_include_control_f.cg 's/bom_only_include_f/empty_include_control_f/g; s/inc_bom_only\.h/inc_empty.h/g' "$EMPTY_INC_BODY"

# Profile independence.  The Vita reference accepts a BOM where the PS3 one
# refuses, and this compiler accepts the psp2 profile names as aliases; the
# policy is the flag's, not the profile's.
refuse "off: psp2 profile, leading BOM" psp2_off off sce_fp_psp2 bom_f.cg yes 'bom_f.cg:1:1: error:' "$BOM_BODY"
refuse "off: vertex profile, leading BOM" vp_off off sce_vp_rsx bom_f.cg yes 'bom_f.cg:1:1: error:' "$BOM_BODY"

# ---------------------------------------------------------------------------
# The printed hint is pasteable: take the token the refusal printed and hand
# it back.  Also pinned to the director's exact spelling.
token="$(grep -o -- '--extension=[A-Za-z0-9_-]*' "$work/bom_off.log" | head -n 1)"
[[ "$token" == "$FLAG" ]] || fail "the refusal printed '$token', not the pinned spelling $FLAG"
run "$compiler" fed off sce_fp_rsx bom_f.cg "$token"
[[ "$rc" -eq 0 ]] || { head -n 3 "$work/fed.log" >&2; fail "feeding the printed token '$token' back was refused (exit $rc) - the hint is not pasteable"; }
same_bytes fed ctl_off "the fed-back token compiled to different bytes from the control"
printf '  %-40s ok\n' "printed hint fed back: $token"

# An unknown extension name enables nothing: with a BOM'd source it is the
# unknown-name refusal that fires, not the BOM one and not an acceptance.
run "$compiler" bogus off sce_fp_rsx bom_f.cg --extension=bogus
[[ "$rc" -eq 1 ]] || fail "--extension=bogus with a BOM'd source exited $rc, expected exactly 1"
[[ ! -e "$work/bogus.fpo" ]] || fail "--extension=bogus left an output artifact"
grep -qF "unknown extension 'bogus'" "$work/bogus.log" || fail "--extension=bogus was not refused as an unknown name"
printf '  %-40s refused\n' "--extension=bogus enables nothing"

# ---------------------------------------------------------------------------
# SELF-CHECK.  One stub per assertion in check_refusal, each wrong in exactly
# one way and otherwise a perfect refusal, so each assertion is the SOLE
# reason its stub fails.  All are exit 1 except the first; a stub that only
# got the exit status wrong would leave every other assertion unproven.
make_stub() {  # <path> <script lines...>
    local path="$1"; shift
    { echo '#!/usr/bin/env bash'; printf '%s\n' "$@"; } > "$path"
    chmod +x "$path"
}
write_named_output='for a in "$@"; do case "$prev" in --emit-container) printf x > "$a";; esac; prev="$a"; done'
# The exit-1 artifact stub leaves an EMPTY file: an empty file is an artifact,
# and a checker weakened from -e to -s would miss exactly this.
write_empty_output='for a in "$@"; do case "$prev" in --emit-container) : > "$a";; esac; prev="$a"; done'
good_bom_msg='echo "bom_f.cg:1:1: error: file begins with a UTF-8 byte order mark; enable it with --extension=bom" >&2'
expect_stub() {  # <stub> <tag> <hint> <prefix> <body> <reason fragment>
    local stub="$1" tag="$2" hint="$3" prefix="$4" body="$5" reason="$6" problem
    problem="$(check_refusal "$stub" "$tag" off sce_fp_rsx bom_f.cg "$hint" "$prefix" "$body")"
    [[ -n "$problem" ]] || fail "self-check: the $tag stub satisfied the refusal check - the assertion it was built to trip is not being made"
    case "$problem" in
        *"$reason"*) ;;
        *) fail "self-check: the $tag stub was rejected for the wrong reason: $problem" ;;
    esac
}
P='bom_f.cg:1:1: error:'
make_stub "$work/s-accept"    "$write_named_output" 'exit 0'
expect_stub "$work/s-accept"    s_accept    yes "$P" "$BOM_BODY" 'expected exactly 1'
make_stub "$work/s-artifact"  "$write_empty_output" "$good_bom_msg" 'exit 1'
expect_stub "$work/s-artifact"  s_artifact  yes "$P" "$BOM_BODY" 'left an output artifact'
make_stub "$work/s-unlocated" 'echo "error: file begins with a UTF-8 byte order mark; enable it with --extension=bom" >&2' 'exit 1'
expect_stub "$work/s-unlocated" s_unlocated yes "$P" "$BOM_BODY" 'no located diagnostic'
make_stub "$work/s-unrelated" 'echo "bom_f.cg:1:1: error: unrelated stage failure --extension=bom" >&2' 'exit 1'
expect_stub "$work/s-unrelated" s_unrelated yes "$P" "$BOM_BODY" 'stderr does not contain'
make_stub "$work/s-spelling"  'echo "bom_f.cg:1:1: error: file begins with a UTF-8 byte order mark; enable it with --enable-extension=bom" >&2' 'exit 1'
expect_stub "$work/s-spelling"  s_spelling  yes "$P" "$BOM_BODY" "does not name $FLAG"
make_stub "$work/s-hinting"   'echo "bom_f.cg:1:1: error: unknown character; try --extension=bom" >&2' 'exit 1'
expect_stub "$work/s-hinting"   s_hinting   no  "$P" "$UNK_BODY" 'from an unrelated error'
printf '  %-40s ok\n' "self-check: six stubs rejected"

# The empty-pair rows have their own adversaries, because equality is the
# weakest assertion there: a wrapper that crashed identically for the empty
# control and its BOM-only twin printed "== control" and passed before these
# existed.  Each stub answers both stems of a pair with identical output and
# is wrong in exactly one way.
expect_pair_stub() {  # <stub> <tag> <reason fragment>
    local stub="$1" tag="$2" reason="$3" problem
    problem="$(check_empty_pair "$stub" "${tag}_on" bom_only_f.cg "${tag}_ctl" empty_control_f.cg 's/bom_only_f/empty_control_f/g' "$EMPTY_MAIN_BODY")"
    [[ -n "$problem" ]] || fail "self-check: the $tag stub satisfied the empty-pair check - equality is being accepted as refusal"
    case "$problem" in
        *"$reason"*) ;;
        *) fail "self-check: the $tag stub was rejected for the wrong reason: $problem" ;;
    esac
}
make_stub "$work/p-crash"     'echo "synthetic crash" >&2' 'exit 134'
expect_pair_stub "$work/p-crash"     p_crash     'expected exactly 1'
make_stub "$work/p-artifact"  "$write_empty_output" 'echo "x:0:0: error: entry point '"'"'main'"'"' not found" >&2' 'exit 1'
expect_pair_stub "$work/p-artifact"  p_artifact  'left an output artifact'
make_stub "$work/p-unrelated" 'echo "x:0:0: error: unrelated stage failure" >&2' 'exit 1'
expect_pair_stub "$work/p-unrelated" p_unrelated 'does not contain'
make_stub "$work/p-hinting"   'echo "x:0:0: error: entry point '"'"'main'"'"' not found; try --extension=bom" >&2' 'exit 1'
expect_pair_stub "$work/p-hinting"   p_hinting   'named an extension flag'
printf '  %-40s ok\n' "self-check: four empty-pair stubs rejected"

printf 'PASS: extension-bom-test\n'
