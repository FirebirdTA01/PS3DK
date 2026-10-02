"""A vertex program must write POSITION/HPOS on some path (C6014, t_52640169).

Measured on sce-cgc 475: refused with "error C6014: Required output 'HPOS'
not written" when the program writes only other outputs or declares
`out float4 o : POSITION` and never assigns it; ACCEPTED when it writes
position on one path only, writes part of it, or writes an HPOS struct
member.  We accepted the refused shapes (a vertex program with no
position at all).  The conditional write is refused here today for an
unrelated, named reason, so its row asserts only that C6014 is not it.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

REFUSE = {
    'color_only': 'float4 main(float4 p : POSITION) : COLOR { return p; }\n',
    'declared_unwritten': 'void main(float4 p : POSITION, out float4 o : POSITION, out float4 c : COLOR) { c = p; }\n',
    'texcoord_only': 'void main(float4 p : POSITION, out float4 t : TEXCOORD0) { t = p; }\n',
}
ACCEPT = {
    'hpos_struct': 'struct V { float4 h : HPOS; }; V main(float4 p : POSITION) { V v; v.h = p; return v; }\n',
    'partial_write': 'void main(float4 p : POSITION, out float4 o : POSITION) { o.xy = p.xy; }\n',
    'plain': 'float4 main(float4 p : POSITION) : POSITION { return p; }\n',
}
NOT_C6014 = {
    'written_on_one_path': 'void main(float4 p : POSITION, out float4 o : POSITION) { if (p.x > 0) o = p; }\n',
}


def run(compiler, work, name, text):
    src, dst = work / (name + '.cg'), work / (name + '.vpo')
    src.write_text(text)
    p = subprocess.run([compiler, '-p', 'sce_vp_rsx', '--emit-container', str(dst), str(src)],
                       capture_output=True, text=True, timeout=60)
    return p.returncode, dst.exists() and dst.stat().st_size > 0, p.stderr


def main():
    compiler = sys.argv[1]
    failures = []
    with tempfile.TemporaryDirectory(prefix='vp-c6014-') as tmp:
        work = Path(tmp)
        for name, text in REFUSE.items():
            rc, out, err = run(compiler, work, name, text)
            ok = rc == 1 and not out and 'C6014' in err
            print('  %-20s %s' % (name, 'refused C6014' if ok else 'NOT refused by C6014 (rc %d)' % rc))
            if not ok:
                failures.append('%s: want exit 1, no container, C6014' % name)
        for name, text in ACCEPT.items():
            rc, out, err = run(compiler, work, name, text)
            ok = rc == 0 and out
            print('  %-20s %s' % (name, 'accepted' if ok else 'REFUSED: ' + (err.strip().splitlines() or ['?'])[-1][:90]))
            if not ok:
                failures.append('%s refused' % name)
        for name, text in NOT_C6014.items():
            rc, out, err = run(compiler, work, name, text)
            ok = 'C6014' not in err
            print('  %-20s %s' % (name, 'not C6014 (rc %d)' % rc if ok else 'WRONGLY C6014'))
            if not ok:
                failures.append('%s: refused as C6014 though it writes position on a path' % name)
    for f in failures:
        print('FAIL:', f)
    print('vp-c6014: %s' % ('PASS' if not failures else 'FAIL (%d)' % len(failures)))
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
