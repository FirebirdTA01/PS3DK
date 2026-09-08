#!/usr/bin/env bash
# t_7a4e3b36: user functions are inlined at the call site (there is no call
# instruction on NV40).  The inliner used to accept only straight-line bodies
# ending in `return expr`, so a helper with an if/else in it, or a void helper
# that writes file-scope state, made the whole program refuse: five
# reference-SDK rows (edge post-sample GenerateMotion x4 - `if (mag > 1) {
# mv /= mag; mag = 1; }` - and GpuBezierTessellation's void
# generateBezierTriangleControlPoints writing a file-scope float3 B[10]).
#
# MEASURED ON THE REFERENCE, 2026-09-07 (probes C:/cgdev/calls-probe):
#   * a helper with an if inside inlines to the same predicated/select shape
#     the entry function gets (18 instructions for the GenerateMotion float
#     twin); if/else with the result assigned in both arms likewise.
#   * a void helper called as a statement inlines its body; file-scope
#     non-uniform globals it writes are plain values afterwards (the
#     reference lists them as UNDEFINED params, B[0..2]).
#   * the reference ALSO accepts a return inside a branch, loops inside a
#     helper, and out/inout parameters - all three stay REFUSED here BY
#     NAME, measured as gaps rather than dropped.
#
# CONTROL: every accept row is refused on a compiler before this change
# ("body contains unsupported control flow" / "no return expression"), the
# refusal rows are checked against a stub that accepts everything, and the
# decoded accept rows must carry the shape the branch produces (a compare
# feeding the merge), not just exit 0.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd -P)"
here="$repo_root/tests/shader-compiler"
compiler="${1:-${RSX_CG_COMPILER:-}}"
[[ -n "$compiler" ]] || compiler="$repo_root/tools/rsx-cg-compiler/build/rsx-cg-compiler"
[[ -x "$compiler" ]] || { printf 'FAIL: rsx-cg-compiler not executable: %s\n' "$compiler" >&2; exit 1; }
fpdec="$here/fp_sources.py"; vpdec="$here/vp_words.py"
[[ -f "$fpdec" && -f "$vpdec" ]] || { printf 'FAIL: decoder missing\n' >&2; exit 1; }
shaders="$repo_root/tools/rsx-cg-compiler/tests/shaders"

work="${TMPDIR:-/tmp}/ps3dk-user-function-inline-control-flow-test.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
pass() { printf 'PASS: %s\n' "$*"; }

compile() {   # <compiler> <stem> <profile> -> rc; leaves $work/<stem>.{bin,out,err,dec}
    local cc="$1" stem="$2" profile="$3" rc=0
    rm -f "$work/$stem.bin"
    ( ulimit -v "${PS3TC_SHADER_TEST_VMEM_KB:-262144}"
      timeout "${PS3TC_SHADER_TEST_TIMEOUT:-15s}" "$cc" -p "$profile" \
          --emit-container "$work/$stem.bin" "$shaders/$stem.cg" ) \
        >"$work/$stem.out" 2>"$work/$stem.err" || rc=$?
    if [[ $rc -eq 0 && -s "$work/$stem.bin" ]]; then
        local dec="$fpdec"; [[ $profile == sce_vp_rsx ]] && dec="$vpdec"
        python3 "$dec" "$work/$stem.bin" >"$work/$stem.dec" 2>>"$work/$stem.err" \
            || fail "$stem: $(basename "$dec") could not read the container"
    fi
    return $rc
}

accept() {   # <stem> <profile> <description>
    local stem="$1" profile="$2"; shift 2
    compile "$compiler" "$stem" "$profile" \
        || { cat "$work/$stem.err" >&2; fail "$stem ($*) was refused"; }
    [[ -s "$work/$stem.bin" ]] || fail "$stem: exit 0 but no container"
    pass "$stem accepted ($*)"
}
expect() {   # <stem> <regex over decoded lines>
    grep -qE "$2" "$work/$1.dec" \
        || { cat "$work/$1.dec" >&2; fail "$1: no instruction matches /$2/"; }
}
forbid() {   # <stem> <regex that must match no line>
    if grep -qE "$2" "$work/$1.dec"; then
        cat "$work/$1.dec" >&2; fail "$1: an instruction matches /$2/"
    fi
}

# The join must be WIRED, not merely present: the compare's destination lane
# has to be read by a later instruction (the select/blend), and the row's
# output must come from that consumer.  A compare that nothing reads is the
# exact shape of the defect (t_cf17f501 / codex's review of d204f228: MUL,
# SGT unused, MOV o0 <- MUL).  Works over vp_words.py and fp_sources.py lines.
joined() {   # <stem> <compare opcode regex>
    python3 - "$work/$1.dec" "$2" <<'PYEOF' || { cat "$work/$1.dec" >&2; fail "$1: the compare's result is never read (unjoined store)"; }
import re, sys
lines = [l.rstrip('\n') for l in open(sys.argv[1], encoding='utf-8', errors='replace') if re.match(r'^\d+ ', l)]
cmp_re = re.compile(r'^\d+ (?:%s) dst=(R\d+) mask=(\w+)' % sys.argv[2])
seen = None
for i, l in enumerate(lines):
    m = cmp_re.match(l)
    if not m: continue
    reg, mask = m.group(1), m.group(2)
    for later in lines[i+1:]:
        if re.search(r'\bs(?:rc)?\d=-?\|?%s\.%s' % (reg, mask[0]), later) or re.search(r'\bs(?:rc)?\d=-?\|?%s\.%s{4}' % (reg, mask[0]), later):
            seen = later; break
    if seen: break
sys.exit(0 if seen else 1)
PYEOF
}
has_float() {   # <stem> <ieee754 hex, e.g. 40400000> <description>: the literal pool carries the value
    python3 - "$work/$1.bin" "$2" <<'PYEOF' || fail "$1: literal $2 ($3) is not in the container"
import sys
data = open(sys.argv[1], 'rb').read(); want = bytes.fromhex(sys.argv[2])
sys.exit(0 if (want in data or want[::-1] in data) else 1)
PYEOF
}
factor() {   # <stem> fp|vp <k>: the output is exactly k * the one input, lane by lane,
             # evaluated from the decoded operands (inline_factor_check.py); then the
             # four artifact controls - the MUL's const lane flipped, its sign flipped,
             # PROGRAM_END cleared on the last row, PROGRAM_END set on the first -
             # must be REJECTED by the same predicate, or the row is too weak to trust.
    local stem="$1" profile="$2" k="$3"
    python3 "$here/inline_factor_check.py" "$work/$stem.bin" "$profile" "$k" \
        || fail "$stem: output is not $k * input (see the value diff above)"
    local control
    for control in lane negate no-end early-end; do
        python3 "$here/inline_factor_check.py" "$work/$stem.bin" "$profile" "$k" --control "$control" \
            || fail "$stem: the $control control was NOT rejected - the value predicate is too weak"
    done
    pass "$stem: output = $k * input; lane, sign, no-END and early-END controls rejected"
}
joined_none() {   # <stem>: NO predicated write anywhere (an arm local that died never joined).
                  # Read from the condition-test and condition-write FIELDS, not from
                  # fp_sources' text, which prints neither (review: codex - a container
                  # whose output MOV carried NVFX_COND_FL decoded to the same text and
                  # passed).  Then the row's own artifact control: the first R0 write's
                  # condition flipped to FL must be REJECTED by the same predicate.
    python3 "$here/predication_check.py" "$work/$1.bin"         || fail "$1: a predicated or condition-forming write is present - something joined at the if"
    python3 "$here/predication_check.py" "$work/$1.bin" --control         || fail "$1: the COND_FL control was NOT rejected - the predication predicate is too weak"
    pass "$1: no predicated write; COND_FL control rejected"
}
envelope() {  # <stem>: no predicated write and a sound instruction envelope, read from the
              # container's FIELDS by predication_check.py (FP: condition test / cc-write /
              # OUT_NONE; VP: inline_factor_check's envelope - condition test, saturate, abs,
              # scalar slot, PROGRAM_END placement).  vp_words and fp_sources print none of
              # those fields, so a text row cannot see them (review: claude, codex).  Then the
              # checker's own container controls must be rejected for exactly their reasons.
    python3 "$here/predication_check.py" "$work/$1.bin"         || fail "$1: a predicated write or a broken envelope is present"
    python3 "$here/predication_check.py" "$work/$1.bin" --control         || fail "$1: an envelope control was NOT rejected for its own reason"
    pass "$1: envelope sound; controls rejected"
}
value_is_input() {  # <stem> fp|vp: the program's output IS its one input, lane for lane, evaluated
                    # from the decoded operands (inline_factor_check with factor 1).  An envelope
                    # check is not a value check: a correct MOV followed by a second write of -IN0
                    # to the same output passes the text rows and the envelope (review: codex).
                    # No lane/sign controls here - a one-instruction MOV has no MUL to mutate.
    python3 "$here/inline_factor_check.py" "$work/$1.bin" "$2" 1         || fail "$1: output is not the input (see the value diff above)"
    pass "$1: output = input on every lane"
}
count() {    # <stem> <regex> <expected count>
    local n; n=$(grep -cE "$2" "$work/$1.dec" || true)
    [[ "$n" -eq "$3" ]] || { cat "$work/$1.dec" >&2; fail "$1: $n lines match /$2/, expected $3"; }
}

refuse() {   # <description> <stem> <profile> <message fragment>
    refuse_with "$compiler" "$@"
}
refuse_with() {   # <compiler> <description> <stem> <profile> <message fragment>
    local cc="$1" desc="$2" stem="$3" profile="$4" frag="$5" rc=0
    compile "$cc" "$stem" "$profile" || rc=$?
    [[ $rc -eq 1 ]] || { cat "$work/$stem.err" >&2; fail "$desc: exit $rc, expected the named refusal (exit 1)"; }
    [[ ! -e "$work/$stem.bin" ]] || fail "$desc: refused but left a container behind"
    grep -qF -- "$frag" "$work/$stem.err" \
        || { cat "$work/$stem.err" >&2; fail "$desc: refusal does not name '$frag'"; }
    pass "$desc refused by name"
}

# --- stub self-check: a compiler that accepts everything must FAIL refuse() ---
stub="$work/accept-all.sh"
printf '#!/usr/bin/env bash\nout=""; while [[ $# -gt 0 ]]; do [[ $1 == --emit-container ]] && out=$2; shift; done\nprintf "x" > "$out"\nexit 0\n' >"$stub"
chmod +x "$stub"
if ( refuse_with "$stub" "stub" fp_inline_loop_f sce_fp_rsx "a loop" ) >/dev/null 2>&1; then
    fail "self-check: refuse() passed against a compiler that accepts everything"
fi
pass "self-check: refuse() rejects an accept-all stub"

# ---------------------------------------------------------------- accepted shapes
accept fp_inline_if_f sce_fp_rsx "GenerateMotion twin: an if inside the helper (normalise when |mv| > 1)"
expect fp_inline_if_f '^[0-9]+ (SGT|SLT|SGE|SLE|SEQ|SNE|MOVC|DIV|RCP) '
accept fp_inline_if_else_f sce_fp_rsx "if/else inside the helper assigning the result in both arms"
expect fp_inline_if_else_f '^[0-9]+ (SGT|SLT|SGE|SLE|SEQ|SNE|MOVC) '
accept vp_inline_void_global_v sce_vp_rsx "void helper writing file-scope B[0..2], read by main"
expect vp_inline_void_global_v '^[0-9]+ (MAD|MUL) '
# The stores must REACH the reads: before the promotion the program compiled
# and read B from constant slots c[46x] (the phantom uniform) - a wrong picture
# with exit 0.  The reference computes from the inputs; so must we.
expect vp_inline_void_global_v '^[0-9]+ (MAD|MUL) .*IN(0|2)\.'
forbid vp_inline_void_global_v '^[0-9]+ (MAD|MUL|ADD|MOV) .*c\[46[4-7]\]\.(x|y|z)(x|y|z)(x|y|z)'
accept fp_global_array_write_f sce_fp_rsx "fragment file-scope array written in main and read back"
expect fp_global_array_write_f '^[0-9]+ (MAD|MUL) '
expect fp_global_array_write_f 'TEX0'
expect fp_global_array_write_f 'TEX1'

# ---------------------------------------------------------------- the array join and global effects (review: codex, claude)
accept vp_global_array_if_v sce_vp_rsx "B[0]=p; if (p.x>0) B[0]=p*2: the conditional store joins (negative p.x must NOT double)"
joined vp_global_array_if_v 'SGT|SLT|SGE|SLE'
accept vp_global_array_if_else_v sce_vp_rsx "both arms store B[0]"
joined vp_global_array_if_else_v 'SGT|SLT|SGE|SLE'
accept vp_inline_void_global_if_v sce_vp_rsx "the conditional store to B[0] inside a void helper joins inside the inlined body"
joined vp_inline_void_global_if_v 'SGT|SLT|SGE|SLE'
accept vp_global_array_if_multi_v sce_vp_rsx "three elements written in one branch: three selects, one compare"
count  vp_global_array_if_multi_v '^[0-9]+ (SGT|SLT|SGE|SLE) ' 1
count  vp_global_array_if_multi_v '^[0-9]+ MAD .* src0=R[0-9]+\.xxxx' 3
accept vp_global_array_if_unwritten_v sce_vp_rsx "an element never written before the if selects against its UNIFORM element"
joined vp_global_array_if_unwritten_v 'SGT|SLT|SGE|SLE'
expect vp_global_array_if_unwritten_v '^[0-9]+ MAD .* src2=C4[0-9][0-9]\.xyzw'
accept fp_global_two_arrays_if_f sce_fp_rsx "TWO promoted arrays, one element of each written in one branch: both never-written elements select against their uniform (the merge loads are numbered by sorted (name, index), never by hash order - review: claude)"
joined fp_global_two_arrays_if_f 'SGT|SLT|SGE|SLE'
accept vp_global_two_arrays_if_v sce_vp_rsx "two promoted arrays in a vertex program, two never-written elements"
joined vp_global_two_arrays_if_v 'SGT|SLT|SGE|SLE'
count  vp_global_two_arrays_if_v '^[0-9]+ MAD .* src0=R[0-9]+\.xxxx' 2
accept vp_inline_global_write_v sce_vp_rsx "gen(p) writes file-scope G = p*2; main returns G: the write reaches the caller"
has_float vp_inline_global_write_v 40000000 "the 2.0 of gen's write"
forbid vp_inline_global_write_v '^[0-9]+ MOV dst=o0 mask=xyzw src0=IN0\.xyzw'
accept vp_inline_array_shadow_v sce_vp_rsx "a callee-local B[1] shadows the file-scope B for the body only: main still returns p"
expect vp_inline_array_shadow_v '^[0-9]+ MOV dst=o0 mask=xyzw src0=IN0\.xyzw'
count  vp_inline_array_shadow_v '^[0-9]+ ' 1
accept vp_inline_reverse_shadow_v sce_vp_rsx "a CALLER-local G shadows the global gen() writes: main returns its own 3*p"
has_float vp_inline_reverse_shadow_v 40400000 "the caller's 3.0"
count  vp_inline_reverse_shadow_v '^[0-9]+ MUL ' 1
accept vp_inline_reverse_array_shadow_v sce_vp_rsx "a CALLER-local B[1] shadows the file-scope B that gen() writes: main returns its own p (review: codex)"
expect vp_inline_reverse_array_shadow_v '^[0-9]+ MOV dst=o0 mask=xyzw src0=IN0\.xyzw'
forbid vp_inline_reverse_array_shadow_v '^[0-9]+ MUL '
# A caller-local shadowing a file-scope name: for an inlined helper the name
# means the GLOBAL (its stashed binding, or the uniform when never assigned),
# and the helper's write goes back to that stash, never to the local
# (review: codex - void_shadow_read returned the local's 3p, reference p).
accept vp_inline_void_shadow_read_v sce_vp_rsx "void gen(){D=G;} after main shadows G with a local 3p: D reads the GLOBAL p"
expect vp_inline_void_shadow_read_v '^[0-9]+ MOV dst=o0 mask=xyzw src0=IN0\.xyzw'
forbid vp_inline_void_shadow_read_v '^[0-9]+ MUL '
accept vp_inline_shadow_read_v sce_vp_rsx "get() returns the global G, not the caller's local"
expect vp_inline_shadow_read_v '^[0-9]+ MOV dst=o0 mask=xyzw src0=IN0\.xyzw'
forbid vp_inline_shadow_read_v '^[0-9]+ MUL '
accept vp_inline_shadow_array_read_v sce_vp_rsx "get() returns the global B[0], not the caller's local B[0]"
expect vp_inline_shadow_array_read_v '^[0-9]+ MOV dst=o0 mask=xyzw src0=IN0\.xyzw'
forbid vp_inline_shadow_array_read_v '^[0-9]+ MUL '
accept vp_inline_shadow_write_twice_v sce_vp_rsx "gen() doubles the shadowed global twice; main still returns its own 3p"
has_float vp_inline_shadow_write_twice_v 40400000 "the caller's 3.0"
count  vp_inline_shadow_write_twice_v '^[0-9]+ MUL ' 1
accept vp_inline_shadow_unassigned_v sce_vp_rsx "a shadowed global nothing assigned: the helper reads the uniform"
expect vp_inline_shadow_unassigned_v '^[0-9]+ MOV dst=o0 mask=xyzw src0=C4[0-9][0-9]\.xyzw'
# The stashed global bindings are control-flow state like the maps they
# mirror: a helper called INSIDE a branch writes the global through the
# stash and that write must join (review: codex - conditional_stash.cg took
# the helper's 2p on both paths, compare unread).
accept vp_inline_conditional_call_global_v sce_vp_rsx "if (p.x>0) gen(p) writes the shadowed global: get() selects p / 2p"
joined vp_inline_conditional_call_global_v 'SGT|SLT|SGE|SLE'
accept vp_inline_conditional_call_array_v sce_vp_rsx "the same through a shadowed file-scope array"
joined vp_inline_conditional_call_array_v 'SGT|SLT|SGE|SLE'
accept vp_inline_conditional_call_unassigned_v sce_vp_rsx "a never-assigned shadowed global written in a branch selects against its uniform"
joined vp_inline_conditional_call_unassigned_v 'SGT|SLT|SGE|SLE'
expect vp_inline_conditional_call_unassigned_v '^[0-9]+ MAD .* src2=C4[0-9][0-9]\.xyzw'
# The parameter exemption decides only whether to REFUSE; the binding itself
# still resolves through the name map, so an inner local shadowing a
# PARAMETER reads the inner local (reference: the {9,8,7,6} constant).
# Newly accepted here - the parent refuses the nested block (review: claude).
accept fp_inline_param_inner_shadow_f sce_fp_rsx "an inner block local shadows the helper's PARAMETER: D takes the inner constant"
forbid fp_inline_param_inner_shadow_f '^[0-9]+ MOV dst=R0 .* s0=TEX0'
expect fp_inline_param_inner_shadow_f '^[0-9]+ MOV dst=R0 mask=xyzw .* s0=c[0-9]+\.'
# Per-scope state is per FUNCTION: another function's local shadowing G
# must leave nothing behind that a helper called from main could be bound
# to (the stash is cleared with the rest of ScopeState at function exit;
# the two-entry-point leak witness from the t_7a4e3b36 review).
accept vp_inline_cross_function_stash_v sce_vp_rsx "another function shadows G with a local; main's get() still reads the global p"
expect vp_inline_cross_function_stash_v '^[0-9]+ MOV dst=o0 mask=xyzw src0=IN0\.xyzw'
forbid vp_inline_cross_function_stash_v '^[0-9]+ MUL '
accept fp_local_array_if_f sce_fp_rsx "LOCAL array: if (uv.x > 0.5) a[0] = 1.0 selects, not overwrites (t_cf17f501, red on the tip)"
joined fp_local_array_if_f 'SGT|SLT|SGE|SLE'
accept fp_global_array_if_f sce_fp_rsx "file-scope array in a fragment program, conditional store"
joined fp_global_array_if_f 'SGT|SLT|SGE|SLE'

# The ENTRY function's parameters are a scope like its locals (t_3af598c8).
# A parameter that shadows a file-scope name: an inlined helper reading that
# name reads the GLOBAL (reference: MOVR R0, G from the uniform, both records
# listed), and a helper WRITING it leaves the parameter alone (reference:
# MOVR R0, f[TEX0]).  Red on 47459167: the read rows took the varying
# (src0=IN0 / s0=TEX0) and the write rows returned the helper's constant.
accept vp_inline_entry_param_read_v sce_vp_rsx "main's PARAMETER G shadows the global get() reads: get() returns the uniform G"
expect vp_inline_entry_param_read_v '^[0-9]+ MOV dst=o0 mask=xyzw src0=C[0-9]+\.xyzw'
forbid vp_inline_entry_param_read_v 'src0=IN0'
count  vp_inline_entry_param_read_v '^[0-9]+ ' 1
accept vp_inline_entry_param_write_v sce_vp_rsx "set() writes the global G while main's PARAMETER G is live: main returns its parameter"
expect vp_inline_entry_param_write_v '^[0-9]+ MOV dst=o0 mask=xyzw src0=IN0\.xyzw'
forbid vp_inline_entry_param_write_v 'src0=C[0-9]+'
count  vp_inline_entry_param_write_v '^[0-9]+ ' 1
accept fp_inline_entry_param_read_f sce_fp_rsx "fragment twin: main's PARAMETER G shadows the global get() reads (the t_3af598c8 witness)"
expect fp_inline_entry_param_read_f '^[0-9]+ MOV dst=R0 mask=xyzw .* s0=c[0-9]+\.'
forbid fp_inline_entry_param_read_f 's0=TEX0'
accept fp_inline_entry_param_write_f sce_fp_rsx "fragment twin: set() writes the global while main's PARAMETER G is live: main returns TEXCOORD0"
expect fp_inline_entry_param_write_f '^[0-9]+ MOV dst=R0 mask=xyzw .* s0=TEX0\.'
forbid fp_inline_entry_param_write_f 's0=c[0-9]+'
# The caller binding survives REASSIGNMENT (review: codex, scope-param-rebound):
# the write-back asks the stash, not whether the name still holds the value
# its declaration was given.  Red on 47459167 AND on the first candidate
# c0884536: both emit a bare MOV R0 <- c0 and never read TEXCOORD0.
accept fp_inline_entry_param_rebound_f sce_fp_rsx "main's PARAMETER G reassigned (G = G*2) before set() writes the global: main returns 2*TEXCOORD0"
expect fp_inline_entry_param_rebound_f '^[0-9]+ MUL dst=R[0-9]+ mask=xyzw .* s0=TEX0\.xyzw'
count  fp_inline_entry_param_rebound_f '^[0-9]+ MUL ' 1
factor fp_inline_entry_param_rebound_f fp 2
forbid fp_inline_entry_param_rebound_f '^[0-9]+ MOV dst=R0 mask=xyzw .* s0=c[0-9]+\.xyzw'
# The reference folds 3*2 into one MULR by 6; we keep both multiplies (a
# pre-existing fold gap, not this boundary).  The VALUE is what is pinned:
# has_float presence was measured insufficient - a container reading the
# const block's zero lane, or negating the factor, passed it (review: codex).
accept fp_inline_local_rebound_f sce_fp_rsx "a caller LOCAL G reassigned (G = G*2) before set() writes the global: main returns 6*TEXCOORD0"
expect fp_inline_local_rebound_f '^[0-9]+ MUL dst=R[0-9]+ mask=xyzw .* s0=TEX0\.xyzw'
count  fp_inline_local_rebound_f '^[0-9]+ MUL ' 2
factor fp_inline_local_rebound_f fp 6
forbid fp_inline_local_rebound_f '^[0-9]+ MOV dst=R0 mask=xyzw .* s0=c[0-9]+\.xyzw'
accept vp_inline_entry_param_rebound_v sce_vp_rsx "vertex twin: main's PARAMETER G reassigned before set() writes the global: o0 = 2*IN0"
expect vp_inline_entry_param_rebound_v '^[0-9]+ MUL dst=R[0-9]+ mask=xyzw src0=IN0\.xyzw'
count  vp_inline_entry_param_rebound_v '^[0-9]+ MUL ' 1
factor vp_inline_entry_param_rebound_v vp 2
forbid vp_inline_entry_param_rebound_v '^[0-9]+ MOV dst=o0 mask=xyzw src0=C[0-9]+'
accept vp_inline_local_rebound_v sce_vp_rsx "vertex twin: a caller LOCAL G reassigned before set() writes the global: o0 = 6*IN0"
expect vp_inline_local_rebound_v '^[0-9]+ MUL dst=R[0-9]+ mask=xyzw src0=IN0\.xyzw'
count  vp_inline_local_rebound_v '^[0-9]+ MUL ' 2
factor vp_inline_local_rebound_v vp 6
forbid vp_inline_local_rebound_v '^[0-9]+ MOV dst=o0 mask=xyzw src0=C[0-9]+'

# The stash captures the global's binding ONCE (stashShadowedGlobal's
# capture-once clause), and since t_3af598c8 that clause has a second caller:
# the parameter loop.  With it removed, a later shadowing declaration re-stashes
# the EARLIER local (or the parameter) as "the global" and get() returns that
# instead of loading the uniform (review: claude - mutant emitted MUL IN0*2 then
# MOV, never a uniform read).  Both shapes are byte-identical to the reference
# on the candidate: FENCBR; MOVR R0, G from the uniform.
accept fp_inline_two_block_shadow_f sce_fp_rsx "two sequential block locals named G, then get() reads the file-scope G: the uniform"
expect fp_inline_two_block_shadow_f '^[0-9]+ MOV dst=R0 mask=xyzw .* s0=c[0-9]+\.xyzw'
forbid fp_inline_two_block_shadow_f 's0=TEX0'
forbid fp_inline_two_block_shadow_f '^[0-9]+ MUL '
count  fp_inline_two_block_shadow_f '^[0-9]+ ' 2
accept fp_inline_param_then_local_shadow_f sce_fp_rsx "main's PARAMETER G, then a block local G: get() still reads the uniform (red on 47459167)"
expect fp_inline_param_then_local_shadow_f '^[0-9]+ MOV dst=R0 mask=xyzw .* s0=c[0-9]+\.xyzw'
forbid fp_inline_param_then_local_shadow_f 's0=TEX0'
forbid fp_inline_param_then_local_shadow_f '^[0-9]+ MUL '
count  fp_inline_param_then_local_shadow_f '^[0-9]+ ' 2

# ------------------------------------------------- block exit (t_7396e0c2)
# A block's own declarations end with it: ScopeState's fourth boundary row.
# Every row below is reference-measured (C:/cgdev/block-probe); the values are
# the reference's, the SHAPE differs only by the standing export-fold MOV, the
# varying preload, and a dead compare left after an arm's only consumer dies.
# Red on 4d0097af: restash read p*2, then_only_global refused, then_only_param
# emitted a select against the parameter, loop_local returned 3p, helper_shadow
# read the uniform (the walked callee-scoped set dropped the post-block write).
accept fp_block_local_restash_f sce_fp_rsx "a block-local G dies at the block's end: return G reads the file-scope G (byte-identical to the reference)"
expect fp_block_local_restash_f '^[0-9]+ MOV dst=R0 mask=xyzw .* s0=c[0-9]+\.xyzw'
forbid fp_block_local_restash_f 's0=TEX0'
forbid fp_block_local_restash_f '^[0-9]+ MUL '
accept fp_block_outer_write_f sce_fp_rsx "an assignment to an OUTER name inside a block SURVIVES the block (the restore is keyed on declarations, not on keys touched)"
expect fp_block_outer_write_f '^[0-9]+ MUL dst=R[0-9]+ mask=xyzw .* s0=TEX0\.xyzw'
factor fp_block_outer_write_f fp 2
accept fp_block_helper_shadow_f sce_fp_rsx "a helper's block-local shadow dies; its post-block write to the global reaches the caller (reference 2*p)"
expect fp_block_helper_shadow_f '^[0-9]+ MUL dst=R[0-9]+ mask=xyzw .* s0=TEX0\.xyzw'
factor fp_block_helper_shadow_f fp 2
accept fp_block_then_only_global_f sce_fp_rsx "a then-only local shadowing the global dies before the join: return G reads the uniform (refused on 4d0097af)"
expect fp_block_then_only_global_f '^[0-9]+ MOV dst=R0 mask=xyzw .* s0=c[0-9]+\.xyzw'
forbid fp_block_then_only_global_f '^[0-9]+ MUL '
joined_none fp_block_then_only_global_f
accept fp_block_then_only_param_f sce_fp_rsx "a then-only local shadowing the entry PARAMETER dies before the join: main returns its parameter, no select"
expect fp_block_then_only_param_f '^[0-9]+ MOV dst=R0 mask=xyzw .* s0=TEX0\.xyzw'
forbid fp_block_then_only_param_f '^[0-9]+ MOV dst=R0 mask=xyzw .* s0=c[0-9]+\.xyzw'
joined_none fp_block_then_only_param_f
accept fp_block_use_before_decl_f sce_fp_rsx "a use BEFORE the block-local's declaration names the file-scope G (no pre-scan binding; reference 2*(p+G))"
expect fp_block_use_before_decl_f '^[0-9]+ ADD dst=R[0-9]+ mask=xyzw .* s0=c[0-9]+\.xyzw s1=R[0-9]+\.xyzw'
count  fp_block_use_before_decl_f '^[0-9]+ MUL ' 1
accept fp_block_field_write_f sce_fp_rsx "an outer struct FIELD written inside a block survives (a binding site with no DeclStmt)"
expect fp_block_field_write_f '^[0-9]+ MAD dst=R[0-9]+ mask=xyzw .* s1=c[0-9]+\.xxxx s2=R[0-9]+\.xyzw'
accept fp_block_loop_local_f sce_fp_rsx "a per-trip loop local does not carry past the loop: the outer t is returned (byte-identical to the reference)"
expect fp_block_loop_local_f '^[0-9]+ MOV dst=R0 mask=xyzw .* s0=TEX0\.xyzw'
forbid fp_block_loop_local_f '^[0-9]+ MUL '
accept fp_block_struct_shadow_f sce_fp_rsx "an inner S s shadows an outer S s: after the block s.f is the OUTER field - a declared struct's qualified keys end with it (review: codex; byte-identical)"
expect fp_block_struct_shadow_f '^[0-9]+ MOV dst=R0 mask=xyzw .* s0=TEX0\.xyzw'
forbid fp_block_struct_shadow_f '^[0-9]+ MUL '
accept fp_block_outer_shadow_helper_write_f sce_fp_rsx "an outer local shadows G; a helper writes the real G inside an inner block; get() after the block reads 2*p (a pre-existing stash entry is kept, not restored)"
factor fp_block_outer_shadow_helper_write_f fp 2
accept fp_block_local_then_helper_write_f sce_fp_rsx "a block-local G shadows the global; a helper INSIDE the block writes the real G; after the block G is the global and carries 2*p (the unstash)"
factor fp_block_local_then_helper_write_f fp 2
# A GLOBAL struct is flattened into globals named by its qualified keys ("s.f"),
# so a shadowing local `S s` stashes every key it owns and a helper's write to
# the real field is routed to the stash and comes back at block exit (review:
# codex, composed boundary - the candidate restored that write away; the parent
# was right by accident, the local and the global sharing one flat key).
accept fp_block_struct_global_write_f sce_fp_rsx "a block-local S s shadows the GLOBAL struct; a helper inside the block writes the real s.f; after the block s.f is the global's 2*p"
factor fp_block_struct_global_write_f fp 2
accept fp_block_struct_global_shadow_f sce_fp_rsx "a block-local S s shadows the GLOBAL struct, no helper: after the block s.f is the global's field, the uniform (3*p on 4d0097af)"
expect fp_block_struct_global_shadow_f '^[0-9]+ MOV dst=R0 mask=xyzw .* s0=c[0-9]+\.xyzw'
forbid fp_block_struct_global_shadow_f '^[0-9]+ MUL '
accept fp_inline_struct_local_write_f sce_fp_rsx "a helper declares its OWN S s and writes s.f: the caller's global s.f is untouched - ownership in the write-back (3*p on 4d0097af)"
expect fp_inline_struct_local_write_f '^[0-9]+ MOV dst=R0 mask=xyzw .* s0=TEX0\.xyzw'
forbid fp_inline_struct_local_write_f '^[0-9]+ MUL '

# ---------------------------------------------------------------- named gaps
refuse "return inside a branch of the helper" fp_inline_return_in_if_f sce_fp_rsx "a return inside control flow"
refuse "a loop inside the helper"             fp_inline_loop_f         sce_fp_rsx "a loop"
refuse "out parameters on the helper"         vp_inline_void_out_v     sce_vp_rsx "out/inout parameters are not supported"
# NESTED shadowing, formerly REFUSED by name (t_cf17f501's placeholder): an
# enclosing helper's parameter now stashes the global at binding and its
# locals stash at declaration / unstash at block exit, so a nested helper
# reads and writes the GLOBAL through the stash (t_1de985bd).  Every value
# below is the reference's (sce-cgc 450, C:/cgdev/refusal-probe): the global
# p in the three reads, and 2*(3*p) for the nested writer under an enclosing
# parameter shadow - a write that the old epilogue would have ERASED with the
# enclosing helper's stash entry.  Red on bd7d5f54 (refused).
accept vp_inline_parameter_shadow_v sce_vp_rsx "a nested helper reads a global an enclosing helper's PARAMETER shadows: the global p (was refused)"
expect vp_inline_parameter_shadow_v '^[0-9]+ MOV dst=o0 mask=xyzw src0=IN0\.xyzw'
forbid vp_inline_parameter_shadow_v '^[0-9]+ MUL '
envelope vp_inline_parameter_shadow_v
value_is_input vp_inline_parameter_shadow_v vp
accept vp_inline_nested_local_shadow_v sce_vp_rsx "a nested helper reads a global an enclosing helper's LOCAL shadows: the global p (was refused)"
expect vp_inline_nested_local_shadow_v '^[0-9]+ MOV dst=o0 mask=xyzw src0=IN0\.xyzw'
forbid vp_inline_nested_local_shadow_v '^[0-9]+ MUL '
envelope vp_inline_nested_local_shadow_v
value_is_input vp_inline_nested_local_shadow_v vp
accept vp_inline_late_local_shadow_v sce_vp_rsx "a nested helper reads the global BEFORE declaring a local of that name in a later block: the global p (was refused)"
expect vp_inline_late_local_shadow_v '^[0-9]+ MOV dst=o0 mask=xyzw src0=IN0\.xyzw'
forbid vp_inline_late_local_shadow_v '^[0-9]+ MUL '
envelope vp_inline_late_local_shadow_v
value_is_input vp_inline_late_local_shadow_v vp
accept vp_inline_nested_writer_v sce_vp_rsx "a nested helper WRITES the global under an enclosing helper's parameter shadow: main reads 6*p (the created stash entry carries the write out)"
factor vp_inline_nested_writer_v vp 6
# A read BEFORE a top-level local of the same name in the SAME helper body:
# the read names the GLOBAL, not the enclosing helper's parameter (review:
# codex, lift-late-direct-local - only the callee's PARAMETERS exempt a name
# from the prologue's stash rebinding; a later top-level local rebinds the
# name when its declaration is reached).  Reference: D = the global p.
accept vp_inline_late_direct_local_v sce_vp_rsx "D = G before a top-level local G in the same helper, under an enclosing parameter shadow: D is the global p"
expect vp_inline_late_direct_local_v '^[0-9]+ MOV dst=o0 mask=xyzw src0=IN0\.xyzw'
forbid vp_inline_late_direct_local_v '^[0-9]+ MUL '
envelope vp_inline_late_direct_local_v
value_is_input vp_inline_late_direct_local_v vp
refuse "run-time index over a written file-scope array" vp_global_array_dynamic_read_v sce_vp_rsx "read with a run-time index"

echo "user-function-inline-control-flow-test: PASS"
