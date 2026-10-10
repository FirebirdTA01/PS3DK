#!/usr/bin/env bash
# Parameter-table records the reference writes and we did not.
#
# Every member of a varying entry-point struct is declared in the parameter
# table, in struct order, whether the shader reads it or not.
# The containers used to list only the members the program loaded, in
# first-use order, so a runtime looking up an unread member by name found
# nothing.  Measured on the reference compiler: an unread member is a
# varying input record with isReferenced 0 and its semantic's resource; a
# member without a semantic records resource 0xcb8 and no semantic string;
# nested struct members are named by their path and array members one per
# element; a uniform member keeps its place in the struct; an unread
# POSITION member is not declared in a fragment program.  FACE and FOG
# fragment inputs record 2199 and 3156 (they recorded 0, read or not).
# A nested member of a varying struct can be read (it was refused).
#
# isReferenced: a matrix uniform is referenced as a whole (one row read marks
# the parent and every row; an unread matrix is all 0, where ours marked the
# parent always and the rows one by one); a plain vertex input the program
# never reads is 0 (ours said 1).
#
# Checks the input records (direction in) of each case below, in order:
# name, CGtype, resource, semantic, paramno, isReferenced.
#
# Usage: tests/shader-compiler/param-records-test.sh <compiler>
set -u
compiler="${1:-${RSX_CG_COMPILER:-}}"
[ -n "$compiler" ] || { echo "param-records: FAIL: compiler required"; exit 1; }
work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT

fail=0
check() {   # <case> <profile> <entry> <expected-records> <source>
    local name=$1 profile=$2 entry=$3 expected=$4 src=$5
    printf '%s\n' "$src" > "$work/$name.cg"
    "$compiler" -p "$profile" -e "$entry" --emit-container "$work/$name.bin" "$work/$name.cg" > "$work/$name.log" 2>&1
    local rc=$?
    if [ $rc -ne 0 ] || [ ! -s "$work/$name.bin" ]; then
        echo "param-records: FAIL $name: compiler refused (rc $rc)"
        fail=1; return
    fi
    if python3 - "$work/$name.bin" "$expected" <<'EOF'
import struct, sys
b = open(sys.argv[1], 'rb').read()
count, table = struct.unpack_from('>2I', b, 12)
cstr = lambda o: b[o:b.index(0, o)].decode() if o else ''
got = []
for i in range(count):
    t, res, var, ri, nm, dv, ec, sem, d, pn, ref = struct.unpack_from('>11I', b, table + 48 * i)
    if d == 0x1001:
        got.append('%s %x %x %s %d %d' % (cstr(nm), t, res, cstr(sem) or '-',
                                      -1 if pn == 0xffffffff else pn, ref))
want = [r.strip() for r in sys.argv[2].split(';') if r.strip()]
if got != want:
    print('    want: ' + ' ; '.join(want))
    print('    got:  ' + ' ; '.join(got))
    sys.exit(1)
EOF
    then echo "param-records: ok   $name"
    else echo "param-records: FAIL $name"; fail=1
    fi
}

check unread_member sce_fp_rsx main_fragment \
'v.a 418 c94 TEXCOORD0 0 1; v.b 416 c95 TEXCOORD1 0 0; v.c 418 ac5 COLOR0 0 1' '
struct V { float4 a : TEXCOORD0; float2 b : TEXCOORD1; float4 c : COLOR0; };
float4 main_fragment(in V v) : COLOR { return float4(v.c.x, v.a.y, 0, 1); }'

check no_semantic sce_fp_rsx main_fragment \
'v.a 418 c94 TEXCOORD0 0 0; v.b 416 cb8 - 0 0; v.c 418 ac5 COLOR0 0 1' '
struct V { float4 a : TEXCOORD0; float2 b; float4 c : COLOR0; };
float4 main_fragment(in V v) : COLOR { return v.c; }'

check nested sce_fp_rsx main_fragment \
'v.a 418 c94 TEXCOORD0 0 1; v.in1.t 416 c96 TEXCOORD2 0 0; v.in1.n 417 c97 TEXCOORD3 0 0' '
struct I { float2 t : TEXCOORD2; float3 n : TEXCOORD3; };
struct V { float4 a : TEXCOORD0; I in1; };
float4 main_fragment(in V v) : COLOR { return v.a; }'

check uniform_member sce_fp_rsx main_fragment \
'v.a 418 c94 TEXCOORD0 0 1; v.k 418 cb8 - 0 0; v.c 418 ac5 COLOR0 0 0' '
struct V { float4 a : TEXCOORD0; uniform float4 k; float4 c : COLOR0; };
float4 main_fragment(in V v) : COLOR { return v.a; }'

check two_structs sce_fp_rsx main_fragment \
'v.a 418 c94 TEXCOORD0 0 0; v.c 418 ac5 COLOR0 0 0; w.d 418 c98 TEXCOORD4 1 0; w.e 418 c99 TEXCOORD5 1 1' '
struct V { float4 a : TEXCOORD0; float4 c : COLOR0; };
struct W { float4 d : TEXCOORD4; float4 e : TEXCOORD5; };
float4 main_fragment(in V v, in W w) : COLOR { return w.e; }'

check none_read sce_fp_rsx main_fragment \
'v.a 418 c94 TEXCOORD0 0 0; v.c 418 ac5 COLOR0 0 0' '
struct V { float4 a : TEXCOORD0; float4 c : COLOR0; };
float4 main_fragment(in V v) : COLOR { return 1; }'

check array_member sce_fp_rsx main_fragment \
'v.a 418 c94 TEXCOORD0 0 1; v.arr[0] 418 c95 TEXCOORD1 0 0; v.arr[1] 418 c96 TEXCOORD1 0 0; v.c 418 ac5 COLOR0 0 0' '
struct V { float4 a : TEXCOORD0; float4 arr[2] : TEXCOORD1; float4 c : COLOR0; };
float4 main_fragment(in V v) : COLOR { return v.a; }'

check fp_position sce_fp_rsx main_fragment \
'v.a 418 c94 TEXCOORD0 0 1; v.c 418 ac5 COLOR0 0 0' '
struct V { float4 pos : POSITION; float4 a : TEXCOORD0; float4 c : COLOR0; };
float4 main_fragment(V v) : COLOR { return v.a; }'

check fp_special sce_fp_rsx main_fragment \
'v.w 418 945 WPOS 0 0; v.f 415 897 FACE 0 0; v.x 418 cb8 - 0 0; v.y 418 c94 - 0 1; v.fg 415 c54 FOG 0 0; v.c1 418 ac6 COLOR1 0 0; v.t7 418 c9b TEXCOORD7 0 0' '
struct V { float4 p : POSITION; float4 w : WPOS; float f : FACE; float4 x; float4 y; float fg : FOG; float4 c1 : COLOR1; float4 t7 : TEXCOORD7; };
float4 main_fragment(in V v) : COLOR { return v.y; }'

check face_fog_read sce_fp_rsx main_fragment \
'f 415 897 FACE 0 1; fg 415 c54 FOG 1 1' '
float4 main_fragment(float f : FACE, float fg : FOG) : COLOR { return float4(f, fg, 0, 1); }'

check vp_unread sce_vp_rsx main_vertex \
'a.pos 418 841 POSITION 0 1; a.nrm 417 843 NORMAL 0 0; a.uv 416 849 TEXCOORD0 0 1; a.col 418 844 COLOR0 0 0; mvp 428 882 - 1 1; mvp[0] 418 882 - 1 1; mvp[1] 418 882 - 1 1; mvp[2] 418 882 - 1 1; mvp[3] 418 882 - 1 1' '
struct A { float4 pos : POSITION; float3 nrm : NORMAL; float2 uv : TEXCOORD0; float4 col : COLOR0; };
struct O { float4 pos : POSITION; float2 uv : TEXCOORD0; };
O main_vertex(A a, uniform float4x4 mvp) { O o; o.pos = mul(mvp, a.pos); o.uv = a.uv; return o; }'

check vp_no_semantic sce_vp_rsx main_vertex \
'a.pos 418 841 POSITION 0 1; a.x 418 cb8 - 0 0; a.bw 418 842 BLENDWEIGHT 0 0; a.t 417 84f TANGENT 0 0; a.a5 418 846 ATTR5 0 0' '
struct A { float4 pos : POSITION; float4 x; float4 bw : BLENDWEIGHT; float3 t : TANGENT; float4 a5 : ATTR5; };
float4 main_vertex(A a) : POSITION { return a.pos; }'

check nested_read sce_fp_rsx main_fragment \
'v.a 418 c94 TEXCOORD0 0 0; v.in1.t 416 c96 TEXCOORD2 0 0; v.in1.n 417 c97 TEXCOORD3 0 1; v.arr[0] 418 c98 TEXCOORD4 0 0; v.arr[1] 418 c99 TEXCOORD4 0 1' '
struct I { float2 t : TEXCOORD2; float3 n : TEXCOORD3; };
struct V { float4 a : TEXCOORD0; I in1; float4 arr[2] : TEXCOORD4; };
float4 main_fragment(in V v) : COLOR { return float4(v.in1.n, v.arr[1].x); }'

# A matrix uniform is referenced as a whole: one row read marks the parent
# and every row; an unread matrix is all 0.
check fp_matrix_rows sce_fp_rsx main_fragment \
't 418 c94 TEXCOORD0 0 1; M 428 cb8 - -1 1; M[0] 418 cb8 - -1 1; M[1] 418 cb8 - -1 1; M[2] 418 cb8 - -1 1; M[3] 418 cb8 - -1 1; N 428 cb8 - -1 0; N[0] 418 cb8 - -1 0; N[1] 418 cb8 - -1 0; N[2] 418 cb8 - -1 0; N[3] 418 cb8 - -1 0' '
float4x4 M; float4x4 N;
float4 main_fragment(float4 t : TEXCOORD0) : COLOR { return M[1] * t; }'

check fp_matrix_params sce_fp_rsx main_fragment \
't 418 c94 TEXCOORD0 0 1; P 428 cb8 - 1 1; P[0] 418 cb8 - 1 1; P[1] 418 cb8 - 1 1; P[2] 418 cb8 - 1 1; P[3] 418 cb8 - 1 1; Q 428 cb8 - 2 0; Q[0] 418 cb8 - 2 0; Q[1] 418 cb8 - 2 0; Q[2] 418 cb8 - 2 0; Q[3] 418 cb8 - 2 0; M 428 cb8 - -1 0; M[0] 418 cb8 - -1 0; M[1] 418 cb8 - -1 0; M[2] 418 cb8 - -1 0; M[3] 418 cb8 - -1 0' '
uniform float4x4 M;
float4 main_fragment(float4 t : TEXCOORD0, uniform float4x4 P, uniform float4x4 Q) : COLOR { return mul(P, t); }'

# A plain vertex input the program never reads is unreferenced.
check vp_unread_inputs sce_vp_rsx main_vertex \
'pos 418 841 POSITION 0 1; t0 416 849 TEXCOORD0 1 1; t1 416 84a TEXCOORD1 2 0; color 418 844 COLOR 3 0; mvp 428 882 - 6 1; mvp[0] 418 882 - 6 1; mvp[1] 418 882 - 6 1; mvp[2] 418 882 - 6 1; mvp[3] 418 882 - 6 1' '
void main_vertex(float4 pos : POSITION, float2 t0 : TEXCOORD0, float2 t1 : TEXCOORD1, float4 color : COLOR,
                 out float4 oPos : POSITION, out float2 oT : TEXCOORD0, uniform float4x4 mvp)
{ oPos = mul(mvp, pos); oT = t0; }'

# A bare varying array parameter is one input per element: arr[1] is the
# next semantic slot, declared as its own record.  It was typed as one
# element, so arr[1] read arr[0]'s input (a silent wrong input).  The
# records pin the declared elements; the input mask pins which register the
# program actually reads (TEXCOORD5 = bit 19 in the fragment mask, the
# vertex mask's bit 12 for TEXCOORD4).
check fp_array_param sce_fp_rsx main_fragment \
'arr[0] 418 c98 TEXCOORD4 0 0; arr[1] 418 c99 TEXCOORD4 0 1' '
float4 main_fragment(float4 arr[2] : TEXCOORD4) : COLOR { return arr[1]; }'

check vp_array_param sce_vp_rsx main_vertex \
'pos 418 841 POSITION 0 1; w[0] 418 84b TEXCOORD2 1 0; w[1] 418 84c TEXCOORD2 1 0; w[2] 418 84d TEXCOORD2 1 1' '
void main_vertex(float4 pos : POSITION, float4 w[3] : TEXCOORD2, out float4 oPos : POSITION, out float4 oT : TEXCOORD0)
{ oPos = pos; oT = w[2]; }'

input_mask() {   # <case> <stage fp|vp> <expected mask hex>
    local got
    got=$(python3 - "$work/$1.bin" "$2" <<'EOF'
import struct, sys
b = open(sys.argv[1], 'rb').read()
prog = struct.unpack_from('>I', b, 20)[0]
print('%x' % struct.unpack_from('>I', b, prog + (4 if sys.argv[2] == 'fp' else 12))[0])
EOF
)
    if [ "$got" = "$3" ]; then echo "param-records: ok   $1 reads input mask $3"
    else echo "param-records: FAIL $1 input mask $got, want $3"; fail=1
    fi
}
[ -s "$work/fp_array_param.bin" ] && input_mask fp_array_param fp 80000
[ -s "$work/vp_array_param.bin" ] && input_mask vp_array_param vp 1001

[ $fail -eq 0 ] && echo "param-records: PASS" || echo "param-records: FAIL"
exit $fail
