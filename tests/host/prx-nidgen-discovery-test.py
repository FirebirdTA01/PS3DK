#!/usr/bin/env python3
"""Regression test for nidgen tool discovery in PRX tests.

Verifies that find_nidgen:
1. Rejects unrelated sibling worktrees (never implicitly selects a sibling directory's build).
2. Rejects implicit external/installed tool locations without opt-in.
3. Discovers in-tree build outputs when present within the repo.
4. Accepts explicit --nidgen selection and env NIDGEN selection, failing clearly on missing targets.
"""

import importlib.util
import os
from pathlib import Path
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
link_test_path = ROOT / 'tests/sdk/prx-register-guid-link-test.py'
spec = importlib.util.spec_from_file_location('prx_link_test', link_test_path)
prx_mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(prx_mod)
find_nidgen = prx_mod.find_nidgen


def test_sibling_isolation():
    with tempfile.TemporaryDirectory(prefix='nidgen-discovery-') as tmp:
        base = Path(tmp)
        repo_root = base / 'worktree-target'
        repo_root.mkdir()

        # Create dummy sibling worktree with dummy nidgen
        sibling = base / 'prx-register-guid'
        sibling_bin = sibling / 'tools/target/x86_64-pc-windows-gnu/debug/nidgen.exe'
        sibling_bin.parent.mkdir(parents=True, exist_ok=True)
        sibling_bin.write_text('dummy sibling binary')

        # Create dummy installed SDK location
        installed_dev = base / 'ps3dev'
        installed_bin = installed_dev / 'bin/nidgen.exe'
        installed_bin.parent.mkdir(parents=True, exist_ok=True)
        installed_bin.write_text('dummy installed binary')

        # Clean env
        old_env = os.environ.get('NIDGEN')
        if 'NIDGEN' in os.environ:
            del os.environ['NIDGEN']

        try:
            # 1. Unrelated sibling must NOT be selected
            found = find_nidgen(repo_root)
            assert found is None, f"sibling binary unexpectedly discovered: {found}"

            # 2. Installed tool must NOT be implicitly selected
            found_dev = find_nidgen(repo_root)
            assert found_dev is None, f"installed binary unexpectedly discovered: {found_dev}"

            # 3. Explicit --nidgen selection works
            explicit = find_nidgen(repo_root, explicit_nidgen=sibling_bin)
            assert explicit == sibling_bin.resolve(), f"explicit nidgen failed: {explicit}"

            # 4. Explicit non-existent target fails clearly
            try:
                find_nidgen(repo_root, explicit_nidgen=base / 'nonexistent/nidgen.exe')
                assert False, "expected FileNotFoundError for nonexistent explicit target"
            except FileNotFoundError:
                pass

            # 5. Environment variable NIDGEN selection works
            os.environ['NIDGEN'] = str(sibling_bin)
            env_found = find_nidgen(repo_root)
            assert env_found == sibling_bin.resolve(), f"env NIDGEN failed: {env_found}"

            # 6. Environment variable non-existent target fails clearly
            os.environ['NIDGEN'] = str(base / 'nonexistent/nidgen.exe')
            try:
                find_nidgen(repo_root)
                assert False, "expected FileNotFoundError for nonexistent env target"
            except FileNotFoundError:
                pass
            del os.environ['NIDGEN']

            # 7. In-tree build output IS discovered
            in_tree_bin = repo_root / 'tools/target/x86_64-pc-windows-gnu/debug/nidgen.exe'
            in_tree_bin.parent.mkdir(parents=True, exist_ok=True)
            in_tree_bin.write_text('dummy in-tree binary')

            discovered = find_nidgen(repo_root)
            assert discovered == in_tree_bin, f"in-tree build not discovered: {discovered}"

        finally:
            if old_env is not None:
                os.environ['NIDGEN'] = old_env
            elif 'NIDGEN' in os.environ:
                del os.environ['NIDGEN']


def main():
    test_sibling_isolation()
    print("prx-nidgen-discovery: PASS")
    return 0


if __name__ == '__main__':
    sys.exit(main())
