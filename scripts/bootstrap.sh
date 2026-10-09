#!/usr/bin/env bash
# PS3 Custom Toolchain — bootstrap
#
# Reproducibly sets up the workspace:
#   1. Verify MSYS2 MinGW64 (Windows) or APT (Linux) build deps are installed.
#   2. Fetch the pinned release tags of GCC/binutils-gdb/newlib into
#      src/upstream, depth 1, and nothing else (no default-branch tip).
#   3. Fetch PSL1GHT at its pinned commit, depth 1, into src/ps3dev.
#   4. With --with-reference-repos only: the other ps3dev repos and the
#      community forks (src/ps3dev, src/forks), kept for patch provenance.
#      No build step reads them.
#
# Every fetch is pinned and depth 1.  Idempotent: a tag or commit already
# present is not fetched again.

set -euo pipefail

WITH_REFERENCE_REPOS=0
for arg in "$@"; do
    case "$arg" in
        --with-reference-repos) WITH_REFERENCE_REPOS=1 ;;
        -h|--help)
            echo "usage: $0 [--with-reference-repos]"
            exit 0 ;;
        *) echo "[bootstrap] ERROR: unknown argument: $arg" >&2; exit 2 ;;
    esac
done

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
    zlib1g-dev libreadline-dev libncurses-dev libexpat1-dev libssl-dev \
    libelf-dev python3 python3-dev git wget rustc cargo patch autoconf \
    automake libtool zip unzip

EOF
    die "Install the missing dependencies above, then re-run bootstrap."
fi

say "Build deps present."

# -----------------------------------------------------------------------------
# Clone helpers.
# -----------------------------------------------------------------------------
# HTTP timeout/buffer tuning for slow upstreams.  sourceware.org has
# answered at 15-30 s/request under load; the GitHub Actions runner's
# default git http timeouts are short enough to misread that as a
# stall and abort with HTTP 502 mid-clone.  These three flags raise
# the bar so a slow but still-progressing transfer is not killed:
#   postBuffer 1 GB     - prevents premature client-side close on big push/fetch
#   lowSpeedLimit 1000  - threshold (bytes/s) below which we consider the link stalled
#   lowSpeedTime 600    - we only call it stalled after 10 min below threshold
# Together with retry_git below this rides through every flake we
# have seen on sourceware.org / gcc.gnu.org without operator
# intervention.
GIT_HTTP_FLAGS=(
    -c http.postBuffer=1048576000
    -c http.lowSpeedLimit=1000
    -c http.lowSpeedTime=600
)

# Retry a git invocation up to 3 times with exponential backoff
# (15 s, 30 s).  Use for clone / fetch operations that hit slow
# upstream mirrors so a transient 502 does not break the bootstrap.
retry_git() {
    local description="$1"; shift
    local attempts=3
    local delay=15
    local attempt
    for attempt in $(seq 1 "$attempts"); do
        if git "${GIT_HTTP_FLAGS[@]}" "$@"; then
            return 0
        fi
        if (( attempt < attempts )); then
            warn "$description failed (attempt $attempt/$attempts), retrying in ${delay}s..."
            sleep "$delay"
            delay=$((delay * 2))
        fi
    done
    warn "$description failed after $attempts attempts"
    return 1
}

init_remote() {
    # An empty repository with origin set; content arrives only through the
    # pinned depth-1 fetches below.
    local url="$1" dir="$2"
    if [[ ! -d "$dir/.git" ]]; then
        say "Initialising $dir ($url)"
        mkdir -p "$dir"
        git -C "$dir" init -q
        git -C "$dir" remote add origin "$url"
    fi
}

pin_commit() {
    # Check out an exact commit, fetching it at depth 1 if it is not
    # already present.
    local dir="$1" commit="$2"
    if [[ "$(git -C "$dir" rev-parse HEAD 2>/dev/null)" == "$commit" ]]; then
        say "$dir already at $commit"
        return 0
    fi
    if ! git -C "$dir" cat-file -e "$commit^{commit}" 2>/dev/null; then
        say "Fetching $commit into $dir"
        retry_git "git fetch of $commit in $dir" -C "$dir" fetch --depth 1 origin "$commit" \
            || die "could not fetch $commit in $dir"
    fi
    git -C "$dir" checkout -q --detach "$commit" \
        || die "could not check out $commit in $dir"
}

fetch_tag() {
    # One release tag at depth 1.  The build scripts `git archive` the tag,
    # so the tagged tree's blobs must all be local: no --filter here (a lazy
    # blob fetch from the promisor remote mid-archive corrupted the tar pipe
    # in CI: "fatal: could not fetch <oid> from promisor remote").
    local dir="$1" tag="$2"
    if git -C "$dir" rev-parse -q --verify "refs/tags/$tag" >/dev/null; then
        say "$dir already has $tag"
        return 0
    fi
    say "Fetching $tag into $dir"
    retry_git "git fetch tag $tag in $dir" -C "$dir" \
        fetch --depth 1 --no-tags origin "refs/tags/$tag:refs/tags/$tag" \
        || die "could not fetch tag $tag in $dir"
}

clone_shallow() {
    # Unpinned depth-1 clone: --with-reference-repos only.
    local url="$1" dir="$2"
    if [[ -d "$dir/.git" ]]; then
        say "$dir present; not updated"
    else
        say "Cloning $url -> $dir"
        retry_git "git clone of $url" clone --depth 1 "$url" "$dir"
    fi
}

# -----------------------------------------------------------------------------
# 1. Upstream toolchain mirrors.
# -----------------------------------------------------------------------------
say "=== Upstream toolchain mirrors ==="

UPSTREAM_DIR="$PS3_TOOLCHAIN_ROOT/src/upstream"
mkdir -p "$UPSTREAM_DIR"

# GCC — need both 12.4.0 (PPU) and 9.5.0 (SPU).
# Use the GitHub mirror — gcc.gnu.org's git endpoint has answered
# at 15-30 s/request under load (HTTP 200 but past the GitHub
# Actions timeout for git's hundreds-of-requests clone path),
# returning RPC-failed 502s mid-clone.  github.com/gcc-mirror/gcc
# is the official mirror maintained by the GCC team and answers
# in ~250 ms; tag names match (releases/gcc-12.4.0 etc.).
init_remote "https://github.com/gcc-mirror/gcc.git" "$UPSTREAM_DIR/gcc"
fetch_tag "$UPSTREAM_DIR/gcc" "releases/gcc-12.4.0"
fetch_tag "$UPSTREAM_DIR/gcc" "releases/gcc-9.5.0"

# Binutils + GDB share one repo.
init_remote "https://sourceware.org/git/binutils-gdb.git" "$UPSTREAM_DIR/binutils-gdb"
fetch_tag "$UPSTREAM_DIR/binutils-gdb" "binutils-2_42"
fetch_tag "$UPSTREAM_DIR/binutils-gdb" "gdb-14.2-release"

# Newlib.
init_remote "https://sourceware.org/git/newlib-cygwin.git" "$UPSTREAM_DIR/newlib-cygwin"
fetch_tag "$UPSTREAM_DIR/newlib-cygwin" "newlib-4.4.0"

# -----------------------------------------------------------------------------
# 2. ps3dev repos.
# -----------------------------------------------------------------------------
say "=== ps3dev repos ==="

PS3DEV_DIR="$PS3_TOOLCHAIN_ROOT/src/ps3dev"
mkdir -p "$PS3DEV_DIR"

init_remote "https://github.com/ps3dev/PSL1GHT.git" "$PS3DEV_DIR/PSL1GHT"
# PSL1GHT is PINNED: patches/psl1ght/ is written against this exact commit and
# build-psl1ght.sh now fails on a hunk that does not apply.  Before this pin a
# fresh bootstrap (and therefore every CI run) took whatever master was that
# day, while long-lived clones kept the master of the day they were made.
# Bump the commit and re-verify the series together, never one without the
# other.
PSL1GHT_COMMIT="f649a08fd536a9e27c08c7db2d93a2d7ee4c3bbe"   # master 2026-06-29
pin_commit "$PS3DEV_DIR/PSL1GHT" "$PSL1GHT_COMMIT"

# -----------------------------------------------------------------------------
# 3. Reference repos (patch provenance only; no build step reads them).
# -----------------------------------------------------------------------------
if (( WITH_REFERENCE_REPOS )); then
    say "=== reference repos ==="
    FORKS_DIR="$PS3_TOOLCHAIN_ROOT/src/forks"
    mkdir -p "$FORKS_DIR"
    clone_shallow "https://github.com/ps3dev/ps3toolchain.git"         "$PS3DEV_DIR/ps3toolchain"
    clone_shallow "https://github.com/ps3dev/ps3libraries.git"         "$PS3DEV_DIR/ps3libraries"
    clone_shallow "https://github.com/bucanero/ps3toolchain.git"       "$FORKS_DIR/bucanero-ps3toolchain"
    clone_shallow "https://github.com/bucanero/PSL1GHT.git"            "$FORKS_DIR/bucanero-PSL1GHT"
    clone_shallow "https://github.com/jevinskie/ps3-gcc.git"           "$FORKS_DIR/jevinskie-ps3-gcc"
    clone_shallow "https://github.com/Estwald/PSDK3v2.git"             "$FORKS_DIR/Estwald-PSDK3v2"
    clone_shallow "https://github.com/StrawFox64/PS3Toolchain.git"     "$FORKS_DIR/StrawFox64-PS3Toolchain"
else
    say "Skipping reference repos (pass --with-reference-repos to clone them)"
fi

# -----------------------------------------------------------------------------
# 4. Build root.
# -----------------------------------------------------------------------------
say "=== Build root ==="

if [[ ! -d "$PS3_BUILD_ROOT" ]]; then
    say "Creating short build root: $PS3_BUILD_ROOT (avoids Windows MAX_PATH)"
    mkdir -p "$PS3_BUILD_ROOT"
fi

# -----------------------------------------------------------------------------
# 5. Stage layout sanity.
# -----------------------------------------------------------------------------
say "=== Staging layout ==="

mkdir -p "$PS3DEV"/{bin,ppu,spu,psl1ght,portlibs}

# -----------------------------------------------------------------------------
say "=== Done ==="
cat <<EOF
Next steps:
  source ./scripts/env.sh
  ./scripts/build-ppu-toolchain.sh      # PPU toolchain (hours)
  ./scripts/build-spu-toolchain.sh      # SPU toolchain (hours)
  ./scripts/build-psl1ght.sh            # PSL1GHT runtime (minutes)
  ./scripts/build-portlibs.sh           # portlibs (tens of minutes)

See README.md for the project overview and docs/ for deeper references.
EOF
