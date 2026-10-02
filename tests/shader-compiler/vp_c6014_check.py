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
# Refused today for a named reason that is NOT C6014 (the reference ACCEPTS
# it: a position write on one path).  Pinned strictly - exit 1, no
# container, this exact reason - so a crash or a silent accept cannot pass.
CONDITIONAL_STORE = 'output store in block'
NOT_C6014 = {
    'written_on_one_path': 'void main(float4 p : POSITION, out float4 o : POSITION) { if (p.x > 0) o = p; }\n',
}
# A position write only on a constant-false path is no write: the reference
# refuses C6014 (measured: if (false), a false static const bool, a chained
# constant condition).  We refuse them too, earlier, with the conditional
# store reason; either name is accepted, a container is not (review: codex).
DEAD_WRITE = {
    'dead_if_false': 'void main(float4 p : POSITION, out float4 o : POSITION, out float4 c : COLOR) { c = p; if (false) o = p; }\n',
    'dead_static_bool': 'static const bool B = false; void main(float4 p : POSITION, out float4 o : POSITION, out float4 c : COLOR) { c = p; if (B) o = p; }\n',
    'dead_chained': 'void main(float4 p : POSITION, out float4 o : POSITION, out float4 c : COLOR) { c = p; bool b = true ? false : (p.x > 0); if (b) o = p; }\n',
}


STATUS_FAILURES = []


def run(compiler, work, name, text):
    src, dst = work / (name + '.cg'), work / (name + '.vpo')
    src.write_text(text)
    try:
        p = subprocess.run([compiler, '-p', 'sce_vp_rsx', '--emit-container', str(dst), str(src)],
                           capture_output=True, text=True, timeout=60)
    except subprocess.TimeoutExpired:
        STATUS_FAILURES.append('%s: timed out' % name)
        return 124, False, ''
    # a refusal is exit 1 exactly; anything else (signal, crash, 124) is not one
    if p.returncode not in (0, 1):
        STATUS_FAILURES.append('%s: exit %d is neither success nor a refusal' % (name, p.returncode))
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
            ok = rc == 1 and not out and CONDITIONAL_STORE in err and 'C6014' not in err
            print('  %-20s %s' % (name, 'refused by the conditional-store gap, not C6014' if ok else 'NOT as pinned (rc %d)' % rc))
            if not ok:
                failures.append('%s: want exit 1, no container, the conditional-store reason, not C6014' % name)
        for name, text in DEAD_WRITE.items():
            rc, out, err = run(compiler, work, name, text)
            ok = rc == 1 and not out and ('C6014' in err or CONDITIONAL_STORE in err)
            print('  %-20s %s' % (name, 'refused' if ok else 'NOT refused (rc %d)' % rc))
            if not ok:
                failures.append('%s: a position write only on a dead path must refuse' % name)
    failures += STATUS_FAILURES
    for f in failures:
        print('FAIL:', f)
    print('vp-c6014: %s' % ('PASS' if not failures else 'FAIL (%d)' % len(failures)))
    sys.exit(1 if failures else 0)


if __name__ == '__main__':
    main()
