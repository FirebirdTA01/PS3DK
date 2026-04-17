#!/usr/bin/env bash
# PS3 Custom Toolchain — bootstrap
#
# Reproducibly sets up the workspace:
#   1. Verify MSYS2 MinGW64 (Windows) or APT (Linux) build deps are installed.
#   2. Clone upstream GCC/binutils-gdb/newlib mirrors into src/upstream (shallow,
#      pinned release tags).
#   3. Clone ps3dev repos into src/ps3dev.
#   4. Clone community forks into src/forks for patch provenance.
#   5. Document the Sony SDK mount step (does not execute it automatically —
#      the user must provide a path when ready).
#
# Idempotent: safe to re-run. Existing clones are fast-forwarded or skipped.

set -euo pipefail

# Source env.sh to get PS3_TOOLCHAIN_ROOT.
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
# shellcheck disable=SC1091
source "$script_dir/env.sh"

say() { printf "[bootstrap] %s\n" "$*"; }
warn() { printf "[bootstrap] WARNING: %s\n" "$*" >&2; }
die() { printf "[bootstrap] ERROR: %s\n" "$*" >&2; exit 1; }

# -----------------------------------------------------------------------------
# Detect host.
# -----------------------------------------------------------------------------
HOST_KIND="unknown"
case "$(uname -s)" in
    MINGW*|MSYS*)
        HOST_KIND="msys2"
        [[ "${MSYSTEM:-}" == "MINGW64" || "${MSYSTEM:-}" == "UCRT64" ]] \
            || warn "MSYS2 detected but MSYSTEM=$MSYSTEM — expected MINGW64 or UCRT64."
        ;;
    Linux*)  HOST_KIND="linux" ;;
    Darwin*) HOST_KIND="macos" ;;
esac
say "Host: $HOST_KIND"

# -----------------------------------------------------------------------------
# Dependency check.
# -----------------------------------------------------------------------------
need_cmds=(git make gcc g++ autoconf automake libtoolize python3 wget bison flex makeinfo cmake ninja)
missing=()
for c in "${need_cmds[@]}"; do
    command -v "$c" >/dev/null 2>&1 || missing+=("$c")
done

# GMP/MPFR/MPC/ISL are libraries — verified indirectly by pkg-config or presence
# of headers in the toolchain include path. Left to the user to confirm; we
# document the MSYS2 / apt install line below.

if [[ ${#missing[@]} -gt 0 ]]; then
    warn "Missing tools: ${missing[*]}"
    cat <<'EOF'

MSYS2 MinGW64 install line:
  pacman -S --needed base-devel \
    mingw-w64-x86_64-toolchain \
    mingw-w64-x86_64-cmake \
    mingw-w64-x86_64-ninja \
    mingw-w64-x86_64-gmp \
    mingw-w64-x86_64-mpfr \
    mingw-w64-x86_64-mpc \
    mingw-w64-x86_64-isl \
    mingw-w64-x86_64-python \
    mingw-w64-x86_64-rust \
    git wget bison flex texinfo

Ubuntu 22.04 install line:
  sudo apt install -y build-essential gcc-12 g++-12 cmake ninja-build \
    texinfo bison flex libgmp-dev libmpfr-dev libmpc-dev libisl-dev \
    python3 git wget rustc cargo

EOF
    die "Install the missing dependencies above, then re-run bootstrap."
fi

say "Build deps present."

# -----------------------------------------------------------------------------
# Clone helpers.
# -----------------------------------------------------------------------------
clone_shallow() {
    local url="$1" dir="$2"
    if [[ -d "$dir/.git" ]]; then
        say "Updating $dir"
        git -C "$dir" fetch --depth 1 --all --tags --prune
    else
        say "Cloning $url -> $dir"
        git clone --depth 1 "$url" "$dir"
    fi
}

clone_partial() {
    # Use blob:none filtering for large upstreams (gcc, binutils-gdb) so we pay
    # blob bandwidth only for files we actually check out.
    local url="$1" dir="$2"
    if [[ -d "$dir/.git" ]]; then
        say "Updating $dir"
        git -C "$dir" fetch --all --tags --prune
    else
        say "Cloning (partial) $url -> $dir"
        git clone --filter=blob:none "$url" "$dir"
    fi
}

fetch_tag() {
    local dir="$1" tag="$2"
    (cd "$dir" && git fetch --depth 1 origin "refs/tags/$tag:refs/tags/$tag" 2>/dev/null) \
        || warn "Failed to fetch tag $tag in $dir (may already be present)"
}

# -----------------------------------------------------------------------------
# 1. Upstream toolchain mirrors.
# -----------------------------------------------------------------------------
say "=== Upstream toolchain mirrors ==="

UPSTREAM_DIR="$PS3_TOOLCHAIN_ROOT/src/upstream"
mkdir -p "$UPSTREAM_DIR"

# GCC — need both 12.4.0 (PPU) and 9.5.0 (SPU).
clone_partial "https://gcc.gnu.org/git/gcc.git" "$UPSTREAM_DIR/gcc"
fetch_tag "$UPSTREAM_DIR/gcc" "releases/gcc-12.4.0"
fetch_tag "$UPSTREAM_DIR/gcc" "releases/gcc-9.5.0"

# Binutils + GDB share one repo.
clone_partial "https://sourceware.org/git/binutils-gdb.git" "$UPSTREAM_DIR/binutils-gdb"
fetch_tag "$UPSTREAM_DIR/binutils-gdb" "binutils-2_42"
fetch_tag "$UPSTREAM_DIR/binutils-gdb" "gdb-14.2-release"

# Newlib.
clone_partial "https://sourceware.org/git/newlib-cygwin.git" "$UPSTREAM_DIR/newlib-cygwin"
fetch_tag "$UPSTREAM_DIR/newlib-cygwin" "newlib-4.4.0.20231231"

# -----------------------------------------------------------------------------
# 2. ps3dev repos.
# -----------------------------------------------------------------------------
say "=== ps3dev repos ==="

PS3DEV_DIR="$PS3_TOOLCHAIN_ROOT/src/ps3dev"
mkdir -p "$PS3DEV_DIR"

clone_shallow "https://github.com/ps3dev/ps3toolchain.git"   "$PS3DEV_DIR/ps3toolchain"
clone_shallow "https://github.com/ps3dev/PSL1GHT.git"        "$PS3DEV_DIR/PSL1GHT"
clone_shallow "https://github.com/ps3dev/ps3libraries.git"   "$PS3DEV_DIR/ps3libraries"

# -----------------------------------------------------------------------------
# 3. Fork repos (patch provenance).
# -----------------------------------------------------------------------------
say "=== fork repos ==="

FORKS_DIR="$PS3_TOOLCHAIN_ROOT/src/forks"
mkdir -p "$FORKS_DIR"

clone_shallow "https://github.com/bucanero/ps3toolchain.git"       "$FORKS_DIR/bucanero-ps3toolchain"
clone_shallow "https://github.com/bucanero/PSL1GHT.git"            "$FORKS_DIR/bucanero-PSL1GHT"
clone_shallow "https://github.com/jevinskie/ps3-gcc.git"           "$FORKS_DIR/jevinskie-ps3-gcc"
clone_shallow "https://github.com/Estwald/PSDK3v2.git"             "$FORKS_DIR/Estwald-PSDK3v2"
clone_shallow "https://github.com/StrawFox64/PS3Toolchain.git"     "$FORKS_DIR/StrawFox64-PS3Toolchain"

# -----------------------------------------------------------------------------
# 4. Sony SDK mount (informational — not executed).
# -----------------------------------------------------------------------------
say "=== Sony SDK mount (deferred) ==="

if [[ -e "$PS3_TOOLCHAIN_ROOT/reference/sony-sdk" ]]; then
    say "reference/sony-sdk/ already exists (mounted or placeholder)."
else
    cat <<EOF
The official Sony PS3 SDK is used privately as an API coverage oracle.
It is never committed and never shipped. When ready to mount it:

  # Windows (MSYS2):
  cmd //c mklink /J "$(cygpath -w "$PS3_TOOLCHAIN_ROOT/reference/sony-sdk")" "D:\\path\\to\\Sony\\SDK\\475.001"
  icacls "$(cygpath -w "$PS3_TOOLCHAIN_ROOT/reference/sony-sdk")" /deny "\$USERNAME":(WD,DC)

See reference/SONY_SDK_POLICY.md for the full rules.

Phase 3 coverage reporting and stub verification tooling will run in
PSL1GHT-only mode until the mount is present.
EOF
fi

# -----------------------------------------------------------------------------
# 5. Build root.
# -----------------------------------------------------------------------------
say "=== Build root ==="

if [[ ! -d "$PS3_BUILD_ROOT" ]]; then
    say "Creating short build root: $PS3_BUILD_ROOT (avoids Windows MAX_PATH)"
    mkdir -p "$PS3_BUILD_ROOT"
fi

# -----------------------------------------------------------------------------
# 6. Stage layout sanity.
# -----------------------------------------------------------------------------
say "=== Staging layout ==="

mkdir -p "$PS3DEV"/{bin,ppu,spu,psl1ght,portlibs}

# -----------------------------------------------------------------------------
say "=== Done ==="
cat <<EOF
Next steps:
  source ./scripts/env.sh
  ./scripts/build-ppu-toolchain.sh      # Phase 1 (hours)
  ./scripts/build-spu-toolchain.sh      # Phase 2a (hours)
  ./scripts/build-psl1ght.sh            # Phase 3 (minutes)
  ./scripts/build-portlibs.sh           # Phase 4 (tens of minutes)

See README.md for the project overview and docs/ for deeper references.
EOF
