#!/usr/bin/env bash
# utf16-extension: the UTF-16 BOM extension, --extension=utf16.
#
# The reference compiler refuses a source that begins with a UTF-16 byte
# order mark (FF FE for UTF-16LE, FE FF for UTF-16BE), and so do we by
# default.  With --extension=utf16 the file is transcoded to UTF-8 before
# the lexer sees it, at BOTH points a .cg file enters the compiler - the
# file on the command line and an #include - and the result is the bytes
# of the same source written in UTF-8.  That equality is the whole safety
# argument: an enabled extension is admissible only where it reduces to
# something the reference already covers, and the UTF-8 twin IS that form,
# so no reference call is needed here.
#
# The transcode is STRICT.  The director's rules:
#   * odd byte count after the BOM refuses
#   * an unpaired surrogate refuses
#   * a U+0000 in the payload refuses
#   * no unmarked-encoding guessing, no silent substitution
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

work="${TMPDIR:-/tmp}/ps3dk-extension-utf16-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

fx="$repo_root/tools/rsx-cg-compiler/tests/shaders/extensions"
FLAG='--extension=utf16'   # the director's spelling, pinned as text

# ---------------------------------------------------------------------------
# Fixture integrity.  A normalisation that stripped the marks would turn
# every row below into a plain compile and the test would pass while proving
# nothing.
first_bytes() { head -c "$2" "$1" | od -An -tx1 | tr -d ' \n'; }
size_of() { wc -c < "$1" | tr -d ' '; }

# UTF-16LE twin begins FF FE then a 2-byte code unit per source character.
# UTF-16BE twin begins FE FF.  Both are the same length.  The UTF-8 twin
# (bom_control_f.cg) is the plain control that both must compile to.  Since
# the source is pure ASCII, each source byte is one UTF-16 code unit (2
# bytes), so utf16 size == 2 (BOM) + 2 * plain_size.
plain_main_size="$(size_of "$fx/bom_control_f.cg")"
utf16_main_size="$(size_of "$fx/utf16le_control_f.cg")"
[[ "$(first_bytes "$fx/utf16le_control_f.cg" 2)" == fffe ]] || fail "utf16le_control_f.cg must begin FF FE"
[[ "$(first_bytes "$fx/utf16be_control_f.cg" 2)" == feff ]] || fail "utf16be_control_f.cg must begin FE FF"
[[ "$utf16_main_size" -eq $((2 + 2 * plain_main_size)) ]] || \
    fail "utf16le_control_f.cg size ($utf16_main_size) != 2+2*plain ($((2+2*plain_main_size)))"
[[ "$(size_of "$fx/utf16be_control_f.cg")" == "$utf16_main_size" ]] || \
    fail "utf16le and utf16be twins must be the same size"

plain_hdr_size="$(size_of "$fx/inc_utf16_control.h")"
utf16_hdr_size="$(size_of "$fx/inc_utf16le.h")"
[[ "$(first_bytes "$fx/inc_utf16le.h" 2)" == fffe ]] || fail "inc_utf16le.h must begin FF FE"
[[ "$(first_bytes "$fx/inc_utf16be.h" 2)" == feff ]] || fail "inc_utf16be.h must begin FE FF"
[[ "$utf16_hdr_size" -eq $((2 + 2 * plain_hdr_size)) ]] || \
    fail "inc_utf16le.h size ($utf16_hdr_size) != 2+2*plain ($((2+2*plain_hdr_size)))"
[[ "$(size_of "$fx/inc_utf16be.h")" == "$utf16_hdr_size" ]] || \
    fail "inc_utf16le and inc_utf16be headers must be the same size"

# Invalid fixtures: each must carry exactly the bytes it is named for.
# utf16_invalid_nul.fcg  = BOM + U+0078 + U+0000 (LE)
#   -> 6 bytes total: FF FE 78 00 00 00   (checked by the hex6 block below)
# utf16_invalid_odd.fcg  = BOM + 3 bytes (odd payload length)
#   -> 5 bytes total
[[ "$(size_of "$fx/utf16_invalid_odd.fcg")" -eq 5 ]] || \
    fail "utf16_invalid_odd.fcg must be 5 bytes (BOM + 3-byte payload), got $(size_of "$fx/utf16_invalid_odd.fcg")"
# utf16_invalid_surrogate.fcg  = BOM + high surrogate + NUL (lone high surrogate)
#   -> 6 bytes total
[[ "$(size_of "$fx/utf16_invalid_surrogate.fcg")" -eq 6 ]] || \
    fail "utf16_invalid_surrogate.fcg must be 6 bytes (BOM + 2 code units), got $(size_of "$fx/utf16_invalid_surrogate.fcg")"
# utf16_only.fcg  = BOM only (no payload)
#   -> exactly 2 bytes
[[ "$(size_of "$fx/utf16_only.fcg")" -eq 2 ]] || \
    fail "utf16_only.fcg must be exactly 2 bytes (BOM only), got $(size_of "$fx/utf16_only.fcg")"
# Headers
[[ "$(size_of "$fx/inc_utf16_control.h")" -ne 0 ]] || \
    fail "inc_utf16_control.h must not be empty"

# Surrogate boundary fixtures (both orders).  Each is 6 bytes (BOM + 2 code
# units).  first_bytes() emits compact hex (no spaces), so pin with compact
# hex on both sides: full 6-byte string must equal the named bytes exactly.
hex6() { od -An -tx1 "$1" | tr -d ' \n'; }
for case in \
    "utf16_lonelow.fcg:fffe00dc7800" \
    "utf16_highterminal.fcg:fffe780000d8" \
    "utf16_wrongpair.fcg:fffe00d84100" \
    "utf16be_lonelow.fcg:feffdc000078" \
    "utf16be_highterminal.fcg:feff0078d800" \
    "utf16be_wrongpair.fcg:feffd8000041"
do
    f="${case%%:*}"; want="${case#*:}"
    [[ "$f" == *.fcg ]] || fail "internal: bad surrogate case '$case'"
    got="$(hex6 "$fx/$f")"
    [[ "$got" == "$want" ]] || \
        fail "$f must be exactly $want (6 bytes, BOM + 2 code units), got $got"
done

# NUL-in-UTF-16 (both orders): BOM + U+0078 + U+0000.  The NUL is at code
# index 1 (NOT index 0) and the file is 6 bytes, so neither order is byte-
# identical to a UTF-32 BOM (FF FE 00 00 / 00 00 FE FF) or to the odd-count
# case - the transcode is even and reaches the NUL code-unit check.
[[ "$(hex6 "$fx/utf16_invalid_nul.fcg")" == fffe78000000 ]] || \
    fail "utf16_invalid_nul.fcg must be FF FE 78 00 00 00, got $(hex6 "$fx/utf16_invalid_nul.fcg")"
[[ "$(hex6 "$fx/utf16be_invalid_nul.fcg")" == feff00780000 ]] || \
    fail "utf16be_invalid_nul.fcg must be FE FF 00 78 00 00, got $(hex6 "$fx/utf16be_invalid_nul.fcg")"
# BE odd: 5 bytes (BOM + 3-byte payload = odd)
[[ "$(size_of "$fx/utf16be_invalid_odd.fcg")" -eq 5 ]] || \
    fail "utf16be_invalid_odd.fcg must be 5 bytes (BOM + 3-byte payload)"

# BMP + surrogate-pair + CRLF valid twins: each must begin with the UTF-16LE
# BOM (FF FE).  The byte-size invariant (2 + 2*plain) holds for pure-ASCII
# (crlf) only; BMP/pair carry multi-byte UTF-8 in the plain and a single
# code unit each in UTF-16LE, so the sizes differ.
for t in bmp pair crlf; do
    [[ "$(first_bytes "$fx/utf16le_${t}_f.cg" 2)" == fffe ]] || \
        fail "utf16le_${t}_f.cg must begin FF FE"
    [[ "$(size_of "$fx/utf16le_${t}_control_f.cg")" -gt 0 ]] || \
        fail "utf16le_${t}_control_f.cg must not be empty"
done

# UTF-32 fixtures must carry the 4-byte BOM:
#   BE: 00 00 FE FF ;  LE: FF FE 00 00
[[ "$(first_bytes "$fx/utf16_utf32be.fcg" 4)" == 0000feff ]] || \
    fail "utf16_utf32be.fcg first 4 bytes must be 00 00 FE FF, got $(first_bytes "$fx/utf16_utf32be.fcg" 4)"
[[ "$(first_bytes "$fx/utf16_utf32le.fcg" 4)" == fffe0000 ]] || \
    fail "utf16_utf32le.fcg first 4 bytes must be FF FE 00 00, got $(first_bytes "$fx/utf16_utf32le.fcg" 4)"

# BOM-only include (2 bytes)
[[ "$(size_of "$fx/inc_utf16only.h")" -eq 2 ]] || \
    fail "inc_utf16only.h must be exactly 2 bytes (BOM only)"
# BOM-only include control (empty)
[[ "$(size_of "$fx/inc_utf16only_plain.h")" -eq 0 ]] || \
    fail "inc_utf16only_plain.h must be 0 bytes"

# badC (valid UTF-16, transcodes to invalid C)
[[ "$(size_of "$fx/utf16le_badc_f.cg")" -gt 2 ]] || \
    fail "utf16le_badc_f.cg must be non-empty"
# CRLF fixture
[[ "$(size_of "$fx/utf16le_crlf_f.cg")" -gt 2 ]] || \
    fail "utf16le_crlf_f.cg must be non-empty"
# 0x1A (Ctrl-Z) guard: the UTF-16LE comment "// ctrlz <0x1A>" must NOT be
# refused as a NUL; off-mode the refusal is the UTF-16 BOM (not NUL), and
# on-mode the transcode must SUCCEED (U+001A is not U+0000).
# First 4 bytes: FF FE BOM, then the comment slash (2F 00 in LE).
[[ "$(first_bytes "$fx/utf16le_ctrlz.fcg" 4)" == fffe2f00 ]] || \
    fail "utf16le_ctrlz.fcg must begin FF FE 2F 00 (BOM + comment slash in UTF-16LE), got $(first_bytes "$fx/utf16le_ctrlz.fcg" 4)"

# Nested UTF-16LE include: outer and inner are both FF FE
[[ "$(first_bytes "$fx/inc_utf16le_outer.h" 2)" == fffe ]] || fail "inc_utf16le_outer.h must begin FF FE"
[[ "$(first_bytes "$fx/inc_utf16le_inner.h" 2)" == fffe ]] || fail "inc_utf16le_inner.h must begin FF FE"
# Plain inner (for the control): no BOM
[[ "$(first_bytes "$fx/inc_utf16le_inner_plain.h" 2)" != fffe ]] || fail "inc_utf16le_inner_plain.h must NOT begin with a UTF-16 BOM"

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
    local cc="$1" tag="$2" mode="$3" profile="$4" stem="$5" hint="$6" prefix="$7" body="$8"; shift 8
    local -a extra=("$@")   # extra flags (e.g. --extension=bom) appended after the mode flag
    run "$cc" "$tag" "$mode" "$profile" "$stem" "${extra[@]}"
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
refuse() {  # <label> <tag> <mode> <profile> <stem> <hint> <prefix> <body> [extra flag ...]
    local label="$1"; shift
    local problem
    problem="$(check_refusal "$compiler" "$@")"
    [[ -z "$problem" ]] || { head -n 4 "$work/$1.log" >&2; fail "$label: $problem"; }
    printf '  %-40s refused\n' "$label"
}
UTF16_BODY='byte order mark'
STRICT_BODY='not valid UTF-16'
# Each strict defect has its own distinguishing substring so the wrong
# classifier (odd reported as NUL, NUL reported as surrogate) cannot pass.
# The decoder reports the offending code unit's zero-based index; pin it so
# a defect reported at the wrong position also fails.
ODD_BODY='an odd number of bytes'
NUL1_BODY='a U+0000 (NUL) at code index 1'
SURR0_BODY='an unpaired surrogate at code index 0'
SURR1_BODY='an unpaired surrogate at code index 1'
UNKNOWN_BODY='unknown character'
EMPTY_INCLUDE_BODY='Failed to read include file'
EMPTY_MAIN_BODY="entry point 'main' not found"
UTF32_BODY='UTF-32'

# ---------------------------------------------------------------------------
# Controls compile in both modes to the same bytes: an enabled extension is
# inert where its construct is absent.
accept ctl_off off bom_control_f.cg
accept ctl_on  on  bom_control_f.cg
same_bytes ctl_off ctl_on "the plain control changed bytes when the extension was enabled"
accept ictl_off off utf16_include_control_f.cg
accept ictl_on  on  utf16_include_control_f.cg
same_bytes ictl_off ictl_on "the plain include control changed bytes when the extension was enabled"
printf '  %-40s ok\n' "controls inert under the flag"

# ---------------------------------------------------------------------------
# Main source: a UTF-16LE (and BE) file refused without the flag, with a
# hint; with the flag, compiles to the bytes of the same source written in
# UTF-8 (the plain control).
refuse "off: leading UTF-16LE BOM" le_off off sce_fp_rsx utf16le_control_f.cg yes 'utf16le_control_f.cg:1:1: error:' "$UTF16_BODY"
accept le_on on utf16le_control_f.cg
same_bytes le_on ctl_off "on: a UTF-16LE file must compile to the bytes of the same file written in UTF-8"
printf '  %-40s == control\n' "on: leading UTF-16LE BOM"

refuse "off: leading UTF-16BE BOM" be_off off sce_fp_rsx utf16be_control_f.cg yes 'utf16be_control_f.cg:1:1: error:' "$UTF16_BODY"
accept be_on on utf16be_control_f.cg
same_bytes be_on ctl_off "on: a UTF-16BE file must compile to the bytes of the same file written in UTF-8"
printf '  %-40s == control\n' "on: leading UTF-16BE BOM"

# Profile independence: the policy is the flag's, not the profile's.
refuse "off: vertex profile, leading UTF-16LE BOM" vp_off off sce_vp_rsx utf16le_control_f.cg yes 'utf16le_control_f.cg:1:1: error:' "$UTF16_BODY"
refuse "off: psp2 profile, leading UTF-16LE BOM" psp2_off off sce_fp_psp2 utf16le_control_f.cg yes 'utf16le_control_f.cg:1:1: error:' "$UTF16_BODY"

# ---------------------------------------------------------------------------
# STRICT transcode refusals.  All three files carry a valid UTF-16 BOM, so
# the detector fires in both modes.  Off: the "you don't have the flag"
# refusal (body = "byte order mark", hint = yes).  On: the strict-transcode
# refusal (body = "not valid UTF-16").  In BOTH cases the diagnostic names
# --extension=utf16, so the hint check applies in both.
#
# utf16_invalid_odd.fcg  -> "an odd number of bytes after the byte order mark"
# utf16_invalid_nul.fcg  -> "a U+0000 (NUL) at code index 1"
# utf16_invalid_surrogate.fcg -> "an unpaired surrogate at code index 0"
#
# lead_utf16be_f.cg (FE FF + ASCII) and lead_utf16le_f.cg (FF FE + ASCII)
# were previously BOM-test near-misses.  They are now the strict-refusal
# cases for UTF-16: when the BOM is stripped and the ASCII payload is
# treated as UTF-16, the odd-length payload is refused as "odd number of
# bytes" (59 ASCII bytes = 59 code units = odd length).  Off: the UTF-16
# BOM detector fires (hint --extension=utf16).  On: the strict transcode
# refuses with "odd number of bytes".  (They do not compile, so no
# same_bytes check applies.)
refuse "off: lead_utf16le (odd payload), BOM refusal" lue_off  off sce_fp_rsx lead_utf16le_f.cg yes 'lead_utf16le_f.cg:1:1: error:' "$UTF16_BODY"
refuse "on:  lead_utf16le (odd payload), strict"     lue_on   on  sce_fp_rsx lead_utf16le_f.cg yes 'lead_utf16le_f.cg:1:1: error:' "$ODD_BODY"
refuse "off: lead_utf16be (odd payload), BOM refusal" lub_off  off sce_fp_rsx lead_utf16be_f.cg yes 'lead_utf16be_f.cg:1:1: error:' "$UTF16_BODY"
refuse "on:  lead_utf16be (odd payload), strict"     lub_on   on  sce_fp_rsx lead_utf16be_f.cg yes 'lead_utf16be_f.cg:1:1: error:' "$ODD_BODY"
printf '  %-40s ok\n' "lead_utf16be/le odd-payload strict refusals"

# OFF rows: the BOM detector fires before any transcode, so the message is
# the "file begins with a UTF-16 byte order mark" refusal.
refuse "off: odd byte count, BOM refusal"  odd_off  off sce_fp_rsx utf16_invalid_odd.fcg        yes 'utf16_invalid_odd.fcg:1:1: error:'     "$UTF16_BODY"
refuse "off: NUL code unit, BOM refusal"   nul_off  off sce_fp_rsx utf16_invalid_nul.fcg        yes 'utf16_invalid_nul.fcg:1:1: error:'     "$UTF16_BODY"
refuse "off: unpaired surrogate, BOM refusal" sur_off off sce_fp_rsx utf16_invalid_surrogate.fcg yes 'utf16_invalid_surrogate.fcg:1:1: error:' "$UTF16_BODY"

# ON rows: the BOM is consumed and the payload is transcoded; the strict
# check fires with a specific defect.  The message contains
# "--extension=utf16" (identifying which extension is in effect) so hint=yes.
refuse "on: odd byte count, strict"        odd_on  on sce_fp_rsx utf16_invalid_odd.fcg        yes 'utf16_invalid_odd.fcg:1:1: error:'     "$ODD_BODY"
refuse "on: NUL code unit, strict"         nul_on  on sce_fp_rsx utf16_invalid_nul.fcg        yes 'utf16_invalid_nul.fcg:1:1: error:'     "$NUL1_BODY"
refuse "on: unpaired surrogate, strict"    sur_on  on sce_fp_rsx utf16_invalid_surrogate.fcg  yes 'utf16_invalid_surrogate.fcg:1:1: error:' "$SURR0_BODY"
# BE order mirrors the LE strict refusals: same defects, byte-swapped.
refuse "off: odd (BE), BOM refusal"        oddbe_off off sce_fp_rsx utf16be_invalid_odd.fcg   yes 'utf16be_invalid_odd.fcg:1:1: error:'  "$UTF16_BODY"
refuse "off: NUL (BE), BOM refusal"        nulbe_off off sce_fp_rsx utf16be_invalid_nul.fcg   yes 'utf16be_invalid_nul.fcg:1:1: error:'  "$UTF16_BODY"
refuse "on: odd (BE), strict"              oddbe_on  on sce_fp_rsx utf16be_invalid_odd.fcg    yes 'utf16be_invalid_odd.fcg:1:1: error:'  "$ODD_BODY"
refuse "on: NUL (BE), strict"              nulbe_on  on sce_fp_rsx utf16be_invalid_nul.fcg    yes 'utf16be_invalid_nul.fcg:1:1: error:'  "$NUL1_BODY"
printf '  %-40s ok\n' "strict transcode refusals (odd / NUL, LE+BE)"

# -- Surrogate boundaries (both orders).  The decoder must refuse a lone low
# surrogate, a high surrogate in the terminal position, and a high surrogate
# followed by a code point that is NOT a low surrogate (wrong pair) - in
# each case the transcode is not "valid UTF-16".  Off: the UTF-16-BOM
# refusal (the BOM detector fires first); on: the strict "not valid UTF-16"
# refusal naming "unpaired surrogate".
for order in "" be; do
    sfx="${order:+${order}}"
    refuse "off: lone low surrogate (${sfx:-LE})"    "ll_${sfx:-le}_off"  off sce_fp_rsx "utf16${sfx}_lonelow.fcg"        yes "utf16${sfx}_lonelow.fcg:1:1: error:"        "$UTF16_BODY"
    refuse "off: high terminal surrogate (${sfx:-LE})" "ht_${sfx:-le}_off" off sce_fp_rsx "utf16${sfx}_highterminal.fcg"  yes "utf16${sfx}_highterminal.fcg:1:1: error:"  "$UTF16_BODY"
    refuse "off: wrong pair (${sfx:-LE})"         "wp_${sfx:-le}_off"  off sce_fp_rsx "utf16${sfx}_wrongpair.fcg"     yes "utf16${sfx}_wrongpair.fcg:1:1: error:"     "$UTF16_BODY"
    refuse "on:  lone low surrogate (${sfx:-LE})"    "ll_${sfx:-le}_on"  on  sce_fp_rsx "utf16${sfx}_lonelow.fcg"        yes "utf16${sfx}_lonelow.fcg:1:1: error:"        "$SURR0_BODY"
    refuse "on:  high terminal surrogate (${sfx:-LE})" "ht_${sfx:-le}_on" on  sce_fp_rsx "utf16${sfx}_highterminal.fcg"  yes "utf16${sfx}_highterminal.fcg:1:1: error:"  "$SURR1_BODY"
    refuse "on:  wrong pair (${sfx:-LE})"         "wp_${sfx:-le}_on"  on  sce_fp_rsx "utf16${sfx}_wrongpair.fcg"     yes "utf16${sfx}_wrongpair.fcg:1:1: error:"     "$SURR0_BODY"
done
printf '  %-40s ok\n' "surrogate boundary refusals (lone low / high terminal / wrong pair, LE+BE)"

# -- Valid multi-byte decode (BMP + surrogate pair).  These are the positive
# controls that prove the decoder path beyond ASCII: a U+00E9 (BMP, 2-byte
# UTF-8) and a U+1F310 (surrogate pair, 4-byte UTF-8) each in a comment, and
# a CRLF line ending.  Each compiles byte-identically to its UTF-8 twin, so
# the transcode is proven to round-trip, not merely not crash.
accept bmp_off off utf16le_bmp_control_f.cg
accept bmp_on  on  utf16le_bmp_f.cg
same_bytes bmp_on  bmp_off "on: a UTF-16LE BMP (U+00E9) file must compile to its UTF-8 twin"
printf '  %-40s == control\n' "on: UTF-16LE BMP decode"
accept pair_off off utf16le_pair_control_f.cg
accept pair_on  on  utf16le_pair_f.cg
same_bytes pair_on  pair_off "on: a UTF-16LE surrogate-pair (U+1F310) file must compile to its UTF-8 twin"
printf '  %-40s == control\n' "on: UTF-16LE surrogate-pair decode"
accept crlf_off off utf16le_crlf_control_f.cg
accept crlf_on  on  utf16le_crlf_f.cg
same_bytes crlf_on  crlf_off "on: a UTF-16LE CRLF file must compile to its UTF-8 twin"
printf '  %-40s == control\n' "on: UTF-16LE CRLF decode"

# -- 0x1A (Ctrl-Z): not a NUL.  A UTF-16LE comment containing U+001A must
# Transcode (accept) rather than refuse as a NUL.  This pins the NUL detector
# to code unit 0x0000 and not to 0x001A (the legacy text-mode EOF byte of the
# default reader, deliberately discarded now that slurpFile uses ios::binary).
# Off: the UTF-16 BOM refusal (hint).  On: the transcode is valid and the
# file compiles to its UTF-8 twin - proving U+001A decodes, it is not a
# refusal trigger.
refuse "off: U+001A lead, BOM refusal" cz_off off sce_fp_rsx utf16le_ctrlz.fcg yes 'utf16le_ctrlz.fcg:1:1: error:' "$UTF16_BODY"
accept cz_ctl off utf16le_ctrlz_control_f.cg
accept cz_on on utf16le_ctrlz.fcg
same_bytes cz_on cz_ctl "on: a UTF-16LE U+001A comment must transcode and compile to its UTF-8 twin (U+001A is not a NUL)"
printf '  %-40s == control\n' "on: U+001A leads to valid transcode (not a NUL)"

# -- Repeated-mark contract (F6): a UTF-16LE file whose payload BEGINS with a
# second mark U+FEFF.  After the first BOM is consumed and the payload is
# transcoded, the text again begins with a leading mark (U+FEFF) that the
# single-leading-mark contract does not strip: the lexer reports it as a
# downstream "unknown character" at 1:1 (the second mark is the first code
# char, exactly as the BOM suite pins its second mark).  All three rows go
# through the shared check_refusal (rc==1, no artifact, located diagnostic),
# so a crash-status mutant (rc!=1) and a bom-consumption mutant (artifact)
# both fail.  The first two rows carry NO --extension hint; the on+bom row
# additionally proves enabling --extension=bom does not consume the mark.
#   off: the FIRST FF FE is the utf16 BOM refusal with its hint.
refuse "off: repeated UTF-16 mark" rmark_off off sce_fp_rsx utf16le_doublemark_f.cg yes 'utf16le_doublemark_f.cg:1:1: error:' "$UTF16_BODY"
#   on (utf16 only): the second mark survives transcode -> downstream refusal,
#     no extension hint.
refuse "on : repeated UTF-16 mark (utf16)" rmark_u on sce_fp_rsx utf16le_doublemark_f.cg no 'utf16le_doublemark_f.cg:1:1: error:' "$UNKNOWN_BODY"
#   on (utf16 + bom): the bom flag must NOT change the outcome -- the second
#     mark still refuses identically, located, with no extension hint.
refuse "on : repeated UTF-16 mark (utf16+bom)" rmark_ub on sce_fp_rsx utf16le_doublemark_f.cg no 'utf16le_doublemark_f.cg:1:1: error:' "$UNKNOWN_BODY" --extension=bom
printf '  %-40s single leading mark preserved\n' "repeated UTF-16 mark (bom cannot consume it)"

# -- Downstream-invalid UTF-8 twin: valid UTF-16 (transcodes cleanly) but the
# decoded C has no entry-point 'main'.  The strict transcode must PASS and the
# refusal must come from the C stage ("entry point not found"), not the
# encoder.  On: exit 1, no artifact, the empty-file body, no extension hint.
run "$compiler" badc_on on sce_fp_rsx utf16le_badc_f.cg
[[ "$rc" -eq 1 ]] || { head -n 4 "$work/badc_on.log" >&2; fail "on: valid-UTF16/C-bad exited $rc, expected 1"; }
[[ ! -e "$work/badc_on.fpo" ]] || fail "on: valid-UTF16/C-bad left an output artifact"
grep -qF "$EMPTY_MAIN_BODY" "$work/badc_on.log" || \
    { head -n 4 "$work/badc_on.log" >&2; fail "on: valid-UTF16/C-bad did not report the C entry-point refusal"; }
grep -qF "$STRICT_BODY" "$work/badc_on.log" && \
    fail "on: valid-UTF16/C-bad was refused by the encoder, but it was valid UTF-16"
printf '  %-40s C-stage refusal (encoder passed)\n' "downstream-invalid UTF-8 twin"

# -- UTF-32: explicitly unsupported, refused in BOTH modes, no flag to enable,
# no utf16 hint (a pasteable utf16 flag would be a lie - enabling it cannot
# admit a UTF-32 BOM).  The 4-byte signatures must be caught before the
# 2-byte UTF-16 check (the UTF-32-Lead FF FE overlaps the UTF-16LE mark).
refuse "off: UTF-32BE, explicit refusal" u32be_off off sce_fp_rsx utf16_utf32be.fcg no 'utf16_utf32be.fcg:1:1: error:' "$UTF32_BODY"
refuse "on:  UTF-32BE, enabling does nothing" u32be_on on sce_fp_rsx utf16_utf32be.fcg no 'utf16_utf32be.fcg:1:1: error:' "$UTF32_BODY"
refuse "off: UTF-32LE, explicit refusal" u32le_off off sce_fp_rsx utf16_utf32le.fcg no 'utf16_utf32le.fcg:1:1: error:' "$UTF32_BODY"
refuse "on:  UTF-32LE, enabling does nothing" u32le_on on sce_fp_rsx utf16_utf32le.fcg no 'utf16_utf32le.fcg:1:1: error:' "$UTF32_BODY"
printf '  %-40s refused both modes (UTF-32 unsupported)\n' "UTF-32 LE/BE explicit refusal"

# ---------------------------------------------------------------------------
# Includes - the wiring guard.  Plain main + UTF-16LE (and BE) include.
# Off: the include's BOM fires the detector (diagnostic located at the
# include's resolved path).  On: the include is transcoded and the result
# compiles to the plain-include bytes.
refuse "off: UTF-16LE on an include" inc_off off sce_fp_rsx utf16le_include_f.cg yes 'inc_utf16le.h:1:1: error:' "$UTF16_BODY"
accept inc_on on utf16le_include_f.cg
same_bytes inc_on ictl_off "on: a UTF-16LE include must compile to the plain-include bytes"
printf '  %-40s == control\n' "on: UTF-16LE on an include"

refuse "off: UTF-16BE on an include" incbe_off off sce_fp_rsx utf16be_include_f.cg yes 'inc_utf16be.h:1:1: error:' "$UTF16_BODY"
accept incbe_on on utf16be_include_f.cg
same_bytes incbe_on ictl_off "on: a UTF-16BE include must compile to the plain-include bytes"
printf '  %-40s == control\n' "on: UTF-16BE on an include"

# Nested UTF-16LE includes: a UTF-16LE outer header that itself #includes a
# UTF-16LE inner header, with the main consuming both defines.  This is the
# wiring guard for the include READ path in a second frame: without the
# hook applied at the nested read, the inner UTF-16LE include is not
# transcoded, the inner define never reaches main, and the compile fails or
# changes bytes.  Off: the first UTF-16 mark the reader meets (the outer
# header) is refused.  On: both headers transcode and the bytes equal the
# plain nested control.
accept nestctl_off off utf16_nested_control_f.cg
refuse "off: nested UTF-16LE include" nest_off off sce_fp_rsx utf16_nested_f.cg yes 'inc_utf16le_outer.h:1:1: error:' "$UTF16_BODY"
accept nest_on on utf16_nested_f.cg
same_bytes nest_on nestctl_off "on: a nested UTF-16LE include must compile to the plain-nested bytes"
printf '  %-40s == control\n' "on: nested UTF-16LE include"

# A BOM-only UTF-16LE header (2 bytes) as an #include.  The baseline policy
# is in unchanged preprocessor.cpp (handleInclude): an empty include file is
# refused ("Failed to read include file").  We do NOT broaden that here.
#   off: the include read sees a UTF-16 BOM and is refused by the decoder,
#        with the --extension=utf16 hint (located at the include path).
#   on : the mark is consumed, leaving an EMPTY header, which is refused by
#        the empty-header policy -- byte-for-byte the same refusal as the plain
#        0-byte header below.  Both are exit-1, no artifact, no extension hint.
refuse "off: BOM-only UTF-16LE include"     bominc_off  off sce_fp_rsx utf16_include_only_f.cg          yes 'inc_utf16only.h:1:1: error:' "$UTF16_BODY"
# baseline control: a 0-byte include refuses under the empty-header policy,
# and it is NOT an extension error (no --extension hint).
refuse "off: empty-include baseline (0-byte)" bomincctl  off sce_fp_rsx utf16_include_only_control_f.cg no  'Failed to read include file' 'inc_utf16only_plain.h'
# on: the BOM is consumed -> empty header -> the SAME baseline refusal.
refuse "on : BOM-only UTF-16LE include -> empty" bominc_on on  sce_fp_rsx utf16_include_only_f.cg          no  'Failed to read include file' 'inc_utf16only.h'
printf '  %-40s baseline\n' "BOM-only UTF-16LE include (empty-header policy)"

# ---------------------------------------------------------------------------
# A file that is ONLY the UTF-16 BOM.  Off: the BOM refusal with hint.
# On: the mark is consumed and what remains is an empty file, which must
# behave EXACTLY as an empty file does today - same exit, same artifact
# presence, same diagnostic up to the file name.  No extension hint.
refuse "off: UTF-16 BOM-only main" only_off off sce_fp_rsx utf16_only.fcg yes 'utf16_only.fcg:1:1: error:' "$UTF16_BODY"
# On: payload after BOM strip is empty -> empty file -> "entry point not found"
run "$compiler" only_on on sce_fp_rsx utf16_only.fcg
[[ "$rc" -eq 1 ]] || { head -n 4 "$work/only_on.log" >&2; fail "on: BOM-only main exited $rc, expected 1 (empty file)"; }
[[ ! -e "$work/only_on.fpo" ]] || fail "on: BOM-only main left an output artifact"
grep -qF "$EMPTY_MAIN_BODY" "$work/only_on.log" || \
    { head -n 4 "$work/only_on.log" >&2; fail "on: BOM-only main did not report the empty-file diagnostic"; }
grep -q -- '--extension' "$work/only_on.log" && \
    fail "on: BOM-only main (empty after strip) named an extension flag from an unrelated error"
printf '  %-40s == empty\n' "on: UTF-16 BOM-only main (empty after strip)"

# ---------------------------------------------------------------------------
# The printed hint is pasteable: take the token the refusal printed and
# hand it back.  Also pinned to the director's exact spelling.
token="$(grep -o -- '--extension=[A-Za-z0-9_-]*' "$work/le_off.log" | head -n 1)"
[[ "$token" == "$FLAG" ]] || fail "the refusal printed '$token', not the pinned spelling $FLAG"
run "$compiler" fed off sce_fp_rsx utf16le_control_f.cg "$token"
[[ "$rc" -eq 0 ]] || { head -n 3 "$work/fed.log" >&2; fail "feeding the printed token '$token' back was refused (exit $rc) - the hint is not pasteable"; }
same_bytes fed ctl_off "the fed-back token compiled to different bytes from the control"
printf '  %-40s ok\n' "printed hint fed back: $token"

# An unknown extension name enables nothing: with a UTF-16LE source it is
# the unknown-name refusal that fires, not the UTF-16 one and not an
# acceptance.
run "$compiler" bogus off sce_fp_rsx utf16le_control_f.cg --extension=bogus
[[ "$rc" -eq 1 ]] || fail "--extension=bogus with a UTF-16LE source exited $rc, expected exactly 1"
[[ ! -e "$work/bogus.fpo" ]] || fail "--extension=bogus left an output artifact"
grep -qF "unknown extension 'bogus'" "$work/bogus.log" || \
    fail "--extension=bogus was not refused as an unknown name"
printf '  %-40s refused\n' "--extension=bogus enables nothing"

# ---------------------------------------------------------------------------
# SELF-CHECK.  One stub per assertion in check_refusal, each wrong in
# exactly one way and otherwise a perfect refusal, so each assertion is the
# SOLE reason its stub fails.
make_stub() {
    local path="$1"; shift
    { echo '#!/usr/bin/env bash'; printf '%s\n' "$@"; } > "$path"
    chmod +x "$path"
}
write_named_output='for a in "$@"; do case "$prev" in --emit-container) printf x > "$a";; esac; prev="$a"; done'
write_empty_output='for a in "$@"; do case "$prev" in --emit-container) : > "$a";; esac; prev="$a"; done'
good_utf16_msg='echo "utf16le_control_f.cg:1:1: error: file begins with a UTF-16 byte order mark; enable it with --extension=utf16" >&2'
expect_stub() {
    local stub="$1" tag="$2" hint="$3" prefix="$4" body="$5" reason="$6" problem
    problem="$(check_refusal "$stub" "$tag" off sce_fp_rsx utf16le_control_f.cg "$hint" "$prefix" "$body")"
    [[ -n "$problem" ]] || \
        fail "self-check: the $tag stub satisfied the refusal check - the assertion it was built to trip is not being made"
    case "$problem" in
        *"$reason"*) ;;
        *) fail "self-check: the $tag stub was rejected for the wrong reason: $problem" ;;
    esac
}
P='utf16le_control_f.cg:1:1: error:'
make_stub "$work/s-accept"    "$write_named_output" 'exit 0'
expect_stub "$work/s-accept"    s_accept    yes "$P" "$UTF16_BODY" 'expected exactly 1'
make_stub "$work/s-artifact"  "$write_empty_output" "$good_utf16_msg" 'exit 1'
expect_stub "$work/s-artifact"  s_artifact  yes "$P" "$UTF16_BODY" 'left an output artifact'
make_stub "$work/s-unlocated" 'echo "error: file begins with a UTF-16 byte order mark; enable it with --extension=utf16" >&2' 'exit 1'
expect_stub "$work/s-unlocated" s_unlocated yes "$P" "$UTF16_BODY" 'no located diagnostic'
make_stub "$work/s-unrelated" 'echo "utf16le_control_f.cg:1:1: error: unrelated stage failure --extension=utf16" >&2' 'exit 1'
expect_stub "$work/s-unrelated" s_unrelated yes "$P" "$UTF16_BODY" 'stderr does not contain'
make_stub "$work/s-spelling"  'echo "utf16le_control_f.cg:1:1: error: file begins with a UTF-16 byte order mark; enable it with --enable-extension=utf16" >&2' 'exit 1'
expect_stub "$work/s-spelling"  s_spelling  yes "$P" "$UTF16_BODY" "does not name $FLAG"
make_stub "$work/s-hinting"   'echo "utf16le_control_f.cg:1:1: error: unknown character; try --extension=utf16" >&2' 'exit 1'
expect_stub "$work/s-hinting"   s_hinting   no  "$P" 'unknown character' 'from an unrelated error'
printf '  %-40s ok\n' "self-check: six stubs rejected"

python3 "$repo_root/tests/shader-compiler/extension_registry_check.py" "$compiler"
printf 'PASS: extension-utf16-test\n'
