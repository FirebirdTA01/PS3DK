# Session Handoff — Windows → CachyOS

This document captures conversational/decision context that doesn't fit `CLAUDE.md` (which is the canonical project-context file). Read this when resuming the project on CachyOS to pick up where the prior session left off.

## How we got here (chronological)

1. **User request:** build a modern OSS PS3 SDK supporting C++17 on PPU + SPU. User is a former former licensee with private SDK reference access. Already had the strategic frame: ps3dev as base, reference SDK as oracle, GCC 8.5/9.5 as the SPU branching point, accept that SPU is a downstream resurrection.
2. **Plan agent + research:** Confirmed PSL1GHT gaps (fragment shaders, networking, ad-hoc naming), cataloged GCC SPU history (obsolete in 9, removed in 10.1; binutils 2.42 still ships spu-elf intact; newlib still ships libgloss/spu).
3. **AskUserQuestion (round 1):** locked in Native MSYS2, SPU Option C Hybrid, GCC 12.4.0 for PPU, NV40-FP assembler.
4. **AskUserQuestion (round 2):** reference SDK mount deferred, Rust tooling, v3 RFC naming with compat shim.
5. **Plan written + approved** at `~/.claude/plans/i-m-going-to-create-melodic-dawn.md`.
6. **Phase 0 executed in full** — directory scaffolding, all clones (531 MB upstream + 5 forks + 3 ps3dev), .gitignore/.gitattributes/README/LICENSE/NOTICE, env.sh + bootstrap.sh, build-{ppu,spu,psl1ght,portlibs}-toolchain.sh, Rust nidgen + coverage-report code, NID schema + sys_lv2.yaml, hello-ppu-c++17 + hello-spu samples, docs/patches-inventory.md, docs/phase-0-status.md.
7. **FNID algorithm proven** against PSDevWiki vectors via Python (Rust impl follows same logic; pending Rust install for `cargo test`).
8. **Phase 1a started:** read binutils-2.22-PS3.patch (90 lines, all SPU-PIE), inspected binutils 2.42 sources, confirmed PPU + SPU targets work out-of-box. Wrote empty-patch READMEs at `patches/{ppu,spu}/binutils-2.42/`.
9. **Hurdle:** Windows shell turned out to be Git Bash for Windows (Git SDK), not full MSYS2. No pacman, no /mingw64/bin/gcc, no `gmp.h`. Tried `pacman -S` — silently no-op'd because pacman doesn't exist.
10. **WSL2 not installed either.** Proposed three paths: install MSYS2, install WSL2, manual MinGW-w64.
11. **User pivoted to CachyOS** (already dual-booted, Arch-based, performance-tuned). Migrating now.

## Why we chose what we chose

- **GCC 12.4.0 over 13.x for PPU**: smaller patch rebase (avoids GCC 13's middle-end churn). 12 is enough for full C++17 + solid C++20.
- **GCC 9.5.0 for SPU (not 8.5.0)**: 9.5.0 is the last with both SPU backend AND C++17 frontend complete. 8.5.0 has SPU but partial C++17.
- **Binutils 2.42 for both**: still ships spu-elf and powerpc64-*-elf catchall. No fork needed.
- **CachyOS over WSL2 Ubuntu**: user dual-boots already. Native Linux beats WSL2 for GCC bootstrap (CPU-bound; ~30-90 min per attempt). Eventual CI is Linux anyway. The `MSYS2 native` original choice fell because Windows shell was Git Bash, not real MSYS2.
- **Rust over Python for tooling**: load-bearing FNID code; strong typing; single static binary; ships in CI cleanly. Python equivalence test (`hashlib.sha1` etc.) used to validate Rust impl since Rust isn't installed yet.
- **PSL1GHT v3 naming with compat shim** over keeping legacy: ≥95% reference-SDK API coverage requires reference-SDK names. Shim header keeps homebrew compatibility for 1-2 releases.
- **Empty binutils patch over porting the 2012 SPU-PIE patch**: that patch's `config.has_pie` mechanism was completely refactored upstream (now uses `config.has_shared`). PS3 homebrew uses `-fpic` compile-time, not `-pie` link-time, so SPU PIE is unneeded.

## Outstanding strategic items

- **reference SDK mount path**: still deferred. Phase 3 (coverage report, NID verification) blocks on this. User indicated SDK is on disk somewhere; will mount via `mklink /J reference/private <path>` (Windows) or `ln -s <path> reference/private` (Linux).
- **Windows-hosted toolchain binaries**: user explicitly noted this is an eventual goal. Approach: Canadian-cross. Build on Linux, configure with `--host=x86_64-w64-mingw32`, install Mingw-w64 cross-compiler in CachyOS host. Belongs to Phase 5 infra (CI matrix).
- **Phase 2b SPU forward-port to GCC 12+**: 12-24 weeks of compiler engineering. Independent stream from M0-M8. Not started; tracked as M9.

## What broke / lessons learned this session

- **Don't trust uname/MSYSTEM alone to identify MSYS2.** Git Bash for Windows sets `MSYSTEM=MINGW64` and `uname` returns `MINGW64_NT-...`, but it's not full MSYS2. Verify pacman + `/etc/pacman.conf` + `/etc/os-release` before assuming.
- **Background pacman with `--noconfirm` exited 0 on Git Bash** because the bash builtin returned 0 even though `pacman: command not found`. Always check the actual log/output of an install command, don't trust exit code alone.
- **Partial git clones work great for upstream sources.** `--filter=blob:none` lets us fetch blobs on demand via `git show <tag>:<path>` or `git archive`. Saved disk and bandwidth.

## Migration mechanics

- **Authored files:** ~30 files, well under 1 MB. Bundled to `D:\ps3-toolchain-bootstrap.bundle`.
- **Memory:** copied to `D:\ps3-toolchain-memory\` (drop into `~/.claude/projects/<project-slug>/memory/` on CachyOS). The project slug on Linux will look like `-home-<user>-PS3_Custom_Toolchain` (slashes replaced with dashes, leading dash). Adjust per Claude Code's conventions on that host.
- **Plan file:** copy `~/.claude/plans/i-m-going-to-create-melodic-dawn.md` from Windows to the same path on CachyOS, OR re-author from this repo's `docs/plan.md` (added as part of the migration).
- **Re-clone upstreams on CachyOS:** ~600 MB. `./scripts/bootstrap.sh` handles it; takes 5-10 min on a decent connection.

## First commands on CachyOS

```bash
# 1. Mount D: (or whatever transport you used) and unbundle
cd ~  # or wherever you want the project
git clone /mnt/d/ps3-toolchain-bootstrap.bundle PS3_Custom_Toolchain
cd PS3_Custom_Toolchain

# 2. Restore memory (optional but recommended for session continuity)
mkdir -p ~/.claude/projects/$(pwd | sed 's|/|-|g')/memory
cp /mnt/d/ps3-toolchain-memory/* ~/.claude/projects/$(pwd | sed 's|/|-|g')/memory/

# 3. Restore plan file
mkdir -p ~/.claude/plans
cp docs/plan.md ~/.claude/plans/i-m-going-to-create-melodic-dawn.md

# 4. Install build deps (CachyOS / Arch)
sudo pacman -S --needed base-devel gcc gmp mpfr libmpc isl python rust git wget bison flex texinfo cmake ninja patch

# 5. Re-clone upstreams + ps3dev + forks
source scripts/env.sh
./scripts/bootstrap.sh

# 6. Resume Phase 1a — binutils dry-build
./scripts/build-ppu-toolchain.sh --only binutils
./scripts/build-spu-toolchain.sh --only binutils

# 7. Launch Claude Code with this project as cwd; CLAUDE.md auto-loads.
claude
```

## Next session's first ask

After a fresh `claude` session in this directory: "Resume Phase 1a — verify binutils 2.42 builds for both targets, then start the GCC 12.4.0 patch rebase." That gives the new session enough context, in combination with auto-loaded CLAUDE.md and memories, to continue without re-litigating.
