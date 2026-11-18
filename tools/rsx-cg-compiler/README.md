# rsx-cg-compiler — Cg → RSX (NV40) compiler

The full guide (command line, language surface, outputs, the PSGL
fixed-function library, inspection and comparison tooling, CMake and
Makefile integration, diagnostics and limits) is
[docs/tools/rsx-cg-compiler.md](../../docs/tools/rsx-cg-compiler.md).

Build:

    cmake -S tools/rsx-cg-compiler -B tools/rsx-cg-compiler/build
    cmake --build tools/rsx-cg-compiler/build --target rsx-cg-compiler

## License

MIT. The front end in `src/cg_frontend/` comes from FirebirdTA01's
open-source Vita Cg compiler, also MIT (copyright 2026 FirebirdTA01).
