"""A mutable file-scope `static` starts each invocation at its initialiser.

libretro's crt-geom declares `static float2 aspect = float2(1.0, 1.0);`,
scale2xSFX `static float3 thresh = float3(...)/255.0;` and the ntsc decoders
`static float luma_filter[TAPS + 1] = {...}`; every read refused (an
unresolved global id, or "ldunif ... no registered uniform source").  The
reference folds a read before any write to the initialiser and sees later
writes, including an inlined helper's.  Rows are judged by value with
fp_eval against the C formula.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

import fp_eval

ROWS = [
    ('scalar', """static float k = 0.5;
float4 main(float4 t : TEXCOORD0) : COLOR { return float4(t.xy * k, 0, 1); }
""", lambda t: [t[0] * 0.5, t[1] * 0.5, 0.0, 1.0]),
    ('vector_pair', """static float2 aspect = float2(1.0, 0.5);
float4 main(float4 t : TEXCOORD0) : COLOR { return float4(t.xy * aspect, 0, 1); }
""", lambda t: [t[0], t[1] * 0.5, 0.0, 1.0]),
    ('divided_init', """static float3 thresh = float3(64.0, 128.0, 32.0) / 256.0;
float4 main(float4 t : TEXCOORD0) : COLOR { return float4(t.xyz + thresh, 1); }
""", lambda t: [t[0] + 0.25, t[1] + 0.5, t[2] + 0.125, 1.0]),
    ('array_macro_extent', """#define TAPS 2
static float lf[TAPS + 1] = { 0.25, 0.5, 0.125 };
float4 main(float4 t : TEXCOORD0) : COLOR
{
    float s = 0.0;
    for (int i = 0; i < TAPS; i++) s += lf[i] * t[i];
    s += lf[TAPS];
    return float4(s, 0, 0, 1);
}
""", lambda t: [t[0] * 0.25 + t[1] * 0.5 + 0.125, 0.0, 0.0, 1.0]),
    ('write_on_one_path', """static float g = 0.25;
float4 main(float4 t : TEXCOORD0) : COLOR
{
    if (t.x > 0.5) g = t.y;
    return float4(g, 0, 0, 1);
}
""", lambda t: [t[1] if t[0] > 0.5 else 0.25, 0.0, 0.0, 1.0]),
    # An entry parameter named like a static shadows it, and the two stay
    # independent: the entry reads its parameter, an inlined helper the
    # static, and a helper's write lands on the static (review: codex,
    # static-entry-shadow; measured: (t, 0.75, 0, 1) and (t, t + 1, 0, 1)).
    ('entry_param_shadow', """static float g = 0.75;
float4 main(float g : TEXCOORD0) : COLOR { return float4(g, 0.0, 0.0, 1.0); }
""", lambda t: [t[0], 0.0, 0.0, 1.0]),
    ('helper_reads_shadowed', """static float g = 0.75;
float rd() { return g; }
float4 main(float g : TEXCOORD0) : COLOR { return float4(g, rd(), 0.0, 1.0); }
""", lambda t: [t[0], 0.75, 0.0, 1.0]),
    ('helper_writes_shadowed', """static float g = 0.75;
void wr(float v) { g = v + 1.0; }
float rd() { return g; }
float4 main(float g : TEXCOORD0) : COLOR { wr(g); return float4(g, rd(), 0.0, 1.0); }
""", lambda t: [t[0], t[0] + 1.0, 0.0, 1.0]),
    # An initialiser the constant evaluator cannot fold - a helper call - is
    # built at program start (measured: a read is 0.5); an entry parameter of
    # the same name still shadows it, and an inlined helper reads the static
    # (measured: (t, 0.5, 0, 1)).  The UNREAD form stays accepted
    # (call-visibility fp_reach_init_root_valid_f).
    ('call_initialiser_read', """float f(float t) { return t * 2.0; }
static float g = f(0.25);
float4 main(float4 t : TEXCOORD0) : COLOR { return float4(g, t.x, 0, 1); }
""", lambda t: [0.5, t[0], 0.0, 1.0]),
    ('call_initialiser_shadowed', """float f(float t) { return t * 2.0; }
static float g = f(0.25);
float rd() { return g; }
float4 main(float g : TEXCOORD0) : COLOR { return float4(g, rd(), 0, 1); }
""", lambda t: [t[0], 0.5, 0.0, 1.0]),
]

INPUTS = [[0.75, 0.5, 0.25, 1.0], [0.25, 0.125, 0.5, 0.0]]


def main():
    compiler = sys.argv[1]
    failures = []
    if not fp_eval.self_test():
        failures.append('fp_eval self-test failed')
    with tempfile.TemporaryDirectory(prefix='mutable-static-') as tmp:
        for name, source, formula in ROWS:
            src, dst = Path(tmp) / (name + '.cg'), Path(tmp) / (name + '.bin')
            src.write_text(source)
            run = subprocess.run([compiler, '-p', 'sce_fp_rsx', '--emit-container', str(dst), str(src)],
                                 capture_output=True, text=True, timeout=60)
            if run.returncode != 0 or not dst.exists():
                failures.append('%s refused: %s' % (name, (run.stderr.strip().splitlines() or ['?'])[0]))
                print('  %-20s REFUSED' % name)
                continue
            blob = dst.read_bytes()
            for t in INPUTS:
                got, want = fp_eval.evaluate(blob, {'TEX0': t}), formula(t)
                ok = got == want
                print('  %-20s %-26s %s' % (name, t, 'value ok' if ok else 'WRONG %s want %s' % (got, want)))
                if not ok:
                    failures.append('%s: got %s for %s, want %s' % (name, got, t, want))
    for f in failures:
        print('FAIL:', f)
    print('mutable-static: %s' % ('PASS' if not failures else 'FAIL (%d)' % len(failures)))
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
