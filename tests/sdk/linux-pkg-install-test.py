#!/usr/bin/env python3
"""Exercise the real Linux installer and release staging with native pkg.

Only unrelated Rust, shader and sprx builds are replaced by inert commands.
Removing pkg installation/staging or bypassing the SHA-1 gate must fail.
"""
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def run(command, cwd, env, success=True):
    result = subprocess.run(command, cwd=cwd, env=env, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if success and result.returncode:
        raise AssertionError(f"{command} exited {result.returncode}\n{result.stdout}")
    return result


def release_step(name):
    lines = (ROOT / '.github/workflows/release.yml').read_text().splitlines()
    start = lines.index('  build-host-tools-linux:')
    end = lines.index('  build-host-tools-windows:', start)
    lines = lines[start:end]
    start = lines.index('      - name: ' + name)
    start = lines.index('        run: |', start) + 1
    body = []
    for line in lines[start:]:
        if line and not line.startswith('          '):
            break
        body.append(line[10:] if line else '')
    assert body, name
    return '\n'.join(body).replace('${{ needs.verify-version.outputs.version }}', 'test')


def round_trip(pkg, scratch, env):
    assert pkg.is_file() and os.access(pkg, os.X_OK), f"pkg not installed: {pkg}"
    payload = scratch / 'payload'
    payload.mkdir(exist_ok=True)
    (payload / 'data.bin').write_bytes(b'Linux installed pkg round trip\n')
    output = scratch / 'test.pkg'
    content_id = 'UP0001-TEST12345_00-0000000000000001'
    run([str(pkg), '--contentid=' + content_id, str(payload), str(output)], scratch, env)
    listing = run([str(pkg), '--list', str(output)], scratch, env)
    assert 'data.bin' in listing.stdout, listing.stdout
    run([str(pkg), '-x', str(output)], scratch, env)
    assert (scratch / content_id / 'data.bin').read_bytes() == (payload / 'data.bin').read_bytes()


with tempfile.TemporaryDirectory(prefix='linux pkg install ') as directory:
    scratch = Path(directory)
    repo = scratch / 'repo'
    (repo / 'scripts').mkdir(parents=True)
    for script in ('env.sh', 'install-host-tools.sh', 'list-rust-bins.sh', 'build-pkg-host.sh'):
        source = ROOT / 'scripts' / script
        if source.exists():
            shutil.copy2(source, repo / 'scripts' / script)
            (repo / 'scripts' / script).chmod(0o755)
    shutil.copytree(ROOT / 'tools/sfo-pkg', repo / 'tools/sfo-pkg')
    # Keep real workspace binary discovery, with one inert fixture binary.
    (repo / 'tools/Cargo.toml').write_text('[workspace]\nmembers = ["fixture"]\n')
    (repo / 'tools/fixture').mkdir()
    (repo / 'tools/fixture/Cargo.toml').write_text('[[bin]]\nname = "fixture-tool"\n')
    for name in ('tools/target/release/fixture-tool', 'tools/sprx-linker/sprxlinker',
                 'tools/rsx-cg-compiler/build/rsx-cg-compiler'):
        destination = repo / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2('/bin/true', destination)
    (repo / 'CHANGELOG.md').write_text('Test release\n')
    stubs = scratch / 'unrelated-builds'
    stubs.mkdir()
    for name in ('cargo', 'cmake', 'make'):
        command = stubs / name
        command.write_text('#!/bin/sh\nexit 0\n')
        command.chmod(0o755)
    prefix = scratch / 'prefix'
    prefix.mkdir()
    build = scratch / 'build'
    (build / 'host-tools-linux/rsx-cg-compiler').mkdir(parents=True)
    shutil.copy2('/bin/true', build / 'host-tools-linux/rsx-cg-compiler/rsx-cg-compiler')
    env = dict(os.environ, PATH=str(stubs) + os.pathsep + os.environ['PATH'],
               PS3DEV=str(prefix), PS3_BUILD_ROOT=str(build))
    run(['bash', 'scripts/install-host-tools.sh'], repo, env)
    for bin_dir in (prefix / 'bin', prefix / 'ps3dk/bin'):
        round_trip(bin_dir / 'pkg', scratch, env)
    print('linux-pkg: PASS installer supplies working pkg in both prefixes')

    # Execute the actual release build/staging bodies, including tar creation.
    run(['bash', '-euo', 'pipefail', '-c', release_step('Build pkg')], repo, env)
    run(['bash', '-euo', 'pipefail', '-c', release_step('Stage tool tarball')], repo, env)
    archive = repo / 'ps3-sdk-tools-test-linux-x86_64.tar.xz'
    with tarfile.open(archive) as package:
        member = package.getmember('ps3-sdk-tools-test-linux-x86_64/bin/pkg')
        assert member.isfile() and member.mode & 0o111
        extracted = scratch / 'tarball-pkg'
        extracted.write_bytes(package.extractfile(member).read())
        extracted.chmod(0o755)
    round_trip(extracted, scratch, env)
    print('linux-pkg: PASS release tarball contains working native pkg')

    # A failed native SHA-1 test must stop installation, even after a good build.
    (repo / 'tools/sfo-pkg/sha1-test.c').write_text(
        '#include <stdio.h>\nint main(void) { puts("injected SHA-1 failure"); return 23; }\n')
    failed_prefix = scratch / 'failed-prefix'
    failed_prefix.mkdir()
    failed_env = dict(env, PS3DEV=str(failed_prefix))
    result = run(['bash', 'scripts/install-host-tools.sh'], repo, failed_env, success=False)
    assert result.returncode != 0 and 'injected SHA-1 failure' in result.stdout, result.stdout
    assert not (failed_prefix / 'bin/pkg').exists()
    assert not (failed_prefix / 'ps3dk/bin/pkg').exists()
    print('linux-pkg: PASS SHA-1 failure prevents installation')
