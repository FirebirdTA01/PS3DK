#!/usr/bin/env python3
"""Read-only source-list audit for the cold-retention PAL dependencies.

This does not compile, link, run a profiler, or prove target symbol closure.
It catches a missing explicit helper in direct Python source groups and the
small set of shell/CMake source lists. Parent gates still test O0 and both ABIs.
"""
import ast
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
HELPERS = ('roll_file', 'roll_retention_inspect', 'roll_store')
PAL = ('pal/pal_host.c', 'pal/pal_lv2.c')


def path_literal(node):
    if isinstance(node, ast.Constant) and isinstance(node.value, str):
        return node.value
    if isinstance(node, ast.BinOp) and isinstance(node.op, ast.Div):
        return path_literal(node.right)
    if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and node.func.id == 'str' and len(node.args) == 1:
        return path_literal(node.args[0])
    return None


def missing_group(values):
    return [name for name in HELPERS if values.count('rolling/' + name + '.c') != 1]


def main():
    groups = 0
    for directory in ['tests', 'samples']:
        for path in (ROOT / directory).rglob('*.py'):
            text = path.read_text(encoding='utf-8')
            if not any(pal in text for pal in PAL) or path.resolve() == Path(__file__).resolve():
                continue
            tree = ast.parse(text, filename=str(path))
            for node in ast.walk(tree):
                if not isinstance(node, (ast.List, ast.Tuple)):
                    continue
                values = [path_literal(value) for value in node.elts]
                if not any(pal in values for pal in PAL):
                    continue
                absent = missing_group(values)
                assert not absent, (str(path.relative_to(ROOT)), node.lineno, 'missing/duplicate explicit helper', absent)
                groups += 1
    assert groups >= 25, ('source-group discovery unexpectedly shrank', groups)
    # A helper cannot disappear silently from the source-list check itself.
    complete = ['pal/pal_host.c'] + ['rolling/' + h + '.c' for h in HELPERS]
    assert not missing_group(complete)
    for helper in HELPERS:
        assert missing_group([s for s in complete if s != 'rolling/' + helper + '.c']) == [helper]

    shell_lists = [
        'tests/host/ps3dk-prof-profile/run-profile-tests.sh',
        'tests/host/ps3dk-prof-spu-int/i3b-build.sh',
        'samples/profiling/ext-probe/host-tests/e1-build.sh',
        'samples/profiling/overhead-bench/host-tests/run-host.sh',
    ]
    for name in shell_lists:
        for line in (ROOT / name).read_text().splitlines():
            if 'pal_host.c' in line:
                assert all('/rolling/' + h + '.c' in line or 'rolling/' + h + '.c' in line for h in HELPERS), name
    runtime = (ROOT / 'tests/host/ps3dk-prof-runtime/build-and-run.sh').read_text()
    assert 'lib_src=(prof_core prof_drain prof_version pal_host ' + ' '.join(HELPERS) + ')' in runtime
    assert 'roll_file|roll_retention_inspect|roll_store)' in runtime
    cmake = (ROOT / 'tests/sdk/ps3dk-prof-files/CMakeLists.txt').read_text()
    assert all('${src}/rolling/' + h + '.c' in cmake for h in HELPERS)
    standalone = (ROOT / 'tests/host/ps3dk-prof-retention/test.py').read_text()
    assert "cmd += [str(ROOT / 'sdk/libps3dkprof/src/rolling' / (name + '.c'))" in standalone
    assert "for name in ['roll_file', 'roll_retention_inspect', 'roll_store']]" in standalone

    make = (ROOT / 'sdk/libps3dkprof/Makefile').read_text()
    assert 'CORE := prof_core prof_drain prof_version pal_lv2 ' + ' '.join(HELPERS) + '\n' in make
    assert 'vpath %.c src/core src/pal src/auto src/live src/rolling' in make
    for directory in ['EXT_DIR', 'EXT64_DIR', 'EXTP_DIR', 'EXTP64_DIR']:
        assert f'$({directory})/%.o: src/rolling/%.c $(CONFIG) Makefile ext-members.mk' in make
    members = (ROOT / 'sdk/libps3dkprof/ext-members.mk').read_text()
    assert 'pal_lv2 ' + ' '.join(HELPERS) + ' ext_runtime' in members
    print(f'PASS explicit cold-retention dependencies: {groups} Python groups, shell/CMake and all SDK recipes; no builds')


if __name__ == '__main__':
    main()
