#!/usr/bin/env python3
"""Keep loader entry and transport code outside application instrumentation."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def run(args):
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


def check(root, out):
    hook = out / "provider"
    (hook / "hook").mkdir(parents=True)
    (hook / "common").mkdir()
    (hook / "hook/ps3dk_run_hook.h").write_text(
        '#ifdef __cplusplus\nextern "C" {\n#endif\n'
        'int ps3dk_run_hook_start(int *, const char ***);\n'
        '#ifdef __cplusplus\n}\n#endif\n')
    (hook / "hook/ps3dk_run_hook.c").write_text(
        'int ps3dk_run_hook_start(int *n, const char ***v) '
        '{ (void)n; (void)v; return 0; }\n')
    for name in ("net", "sha256", "hmac"):
        (hook / f"common/prn_{name}.c").write_text(
            f'int prn_{name}(int n) {{ return n + 1; }}\n')
    src = out / "src"
    src.mkdir()
    (src / "main.c").write_text('int main(void) { return 0; }\n')
    (src / "main.cpp").write_text('int main() { return 0; }\n')
    (src / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 3.20)
project(loader_instrumentation C CXX)
add_library(net INTERFACE)
add_library(sysmodule_stub INTERFACE)
add_executable(c_sample main.c)
add_executable(cpp_sample main.cpp)
foreach(t c_sample cpp_sample)
  target_compile_options(${t} PRIVATE -finstrument-functions)
endforeach()
''')
    build = out / "build"
    run(["cmake", "-S", str(src), "-B", str(build), "-G", "Unix Makefiles",
         f"-DCMAKE_PROJECT_INCLUDE={root}/tests/hardware/psgl-sample-boot/run-hook-inject.cmake",
         f"-DPS3DK_RUN_HOOK_DIR={hook}"])
    run(["cmake", "--build", str(build), "--parallel", "2"])
    for target in ("c_sample", "cpp_sample"):
        objects = list((build / f"CMakeFiles/{target}.dir").rglob("*.o"))
        assert len(objects) == 6, (target, objects)
        for obj in objects:
            symbols = run(["nm", "-u", str(obj)])
            is_app = obj.name in ("main.c.o", "main.cpp.o")
            instrumented = "__cyg_profile_func_enter" in symbols
            assert instrumented == is_app, (target, obj, symbols)
            assert ("__cyg_profile_func_exit" in symbols) == is_app, (target, obj, symbols)
        run([str(build / target)])
    print("PASS: both application objects instrumented; all ten C/C++ loader objects excluded")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    if args.out:
        args.out.mkdir(parents=True, exist_ok=False)
        check(root, args.out.resolve())
    else:
        with tempfile.TemporaryDirectory(prefix="run-hook-inject-") as tmp:
            check(root, Path(tmp))
