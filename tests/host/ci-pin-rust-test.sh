#!/usr/bin/env bash
# The release workflow's exact Rust pin (scripts/ci-pin-rust.sh).
#
#   1. Every dtolnay/rust-toolchain step in .github/workflows/release.yml names
#      exactly the pinned version (no @stable, no other version), and each job
#      that installs Rust runs `scripts/ci-pin-rust.sh --check` after it.
#   2. --check on fake rustc/cargo: the pinned version -> exit 0; another
#      version, a nightly of the pinned number, or a missing cargo -> exit 1.
#
# usage: ci-pin-rust-test.sh   Exit 0 pass, 1 fail.
set -u
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
pin="$root/scripts/ci-pin-rust.sh"
wf="$root/.github/workflows/release.yml"
status=0
ok()   { echo "ci-pin-rust: ok   $*"; }
fail() { echo "ci-pin-rust: FAIL $*"; status=1; }
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT

version="$(bash "$pin" --version)"
if [[ "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    ok "pinned version $version is exact"
else
    fail "pinned version '$version' is not an exact x.y.z"
fi

# 1. The workflow: each job's dtolnay step and check step.
awk -v want="dtolnay/rust-toolchain@$version" '
    /^  [A-Za-z0-9_-]+:$/ { job = $1 }
    /dtolnay\/rust-toolchain@/ {
        ref = $0; sub(/.*uses: */, "", ref); sub(/ .*/, "", ref)
        if (ref != want) printf "BADREF %s %s\n", job, ref
        installs[job] = 1; checked[job] = 0
    }
    /run: bash scripts\/ci-pin-rust.sh --check/ { if (job in installs) checked[job] = 1; else printf "EARLY %s\n", job }
    END { for (j in installs) printf "%s %s\n", (checked[j] ? "CHECKED" : "UNCHECKED"), j }
' "$wf" > "$work/wf.txt"
installs=$(grep -c '^CHECKED\|^UNCHECKED' "$work/wf.txt")
if grep -q '^BADREF\|^UNCHECKED\|^EARLY' "$work/wf.txt"; then
    fail "release.yml:"; sed 's/^/    /' "$work/wf.txt"
elif [ "$installs" -ge 3 ]; then
    ok "release.yml: $installs jobs install dtolnay/rust-toolchain@$version and check the pin after it"
else
    fail "release.yml: only $installs Rust jobs found (expected the host-tools and toolchain jobs)"
fi

# 2. --check against fake tools.
fake() {  # fake <dir> <rustc line> <cargo line or empty for none>
    mkdir -p "$1"
    printf '#!/bin/sh\necho "%s"\n' "$2" > "$1/rustc"; chmod +x "$1/rustc"
    if [ -n "$3" ]; then printf '#!/bin/sh\necho "%s"\n' "$3" > "$1/cargo"; chmod +x "$1/cargo"; fi
}
run_check() { PATH="$1:/usr/bin:/bin" bash "$pin" --check > "$work/out.txt" 2>&1; echo $?; }

fake "$work/good" "rustc $version (0000000 2026-01-01)" "cargo $version (0000000 2026-01-01)"
rc=$(run_check "$work/good")
if [ "$rc" -eq 0 ]; then
    ok "--check: pinned rustc and cargo -> exit 0"
else
    fail "--check refused the pinned version (exit $rc)"; sed 's/^/    /' "$work/out.txt"
fi

major="${version%%.*}"; rest="${version#*.}"; minor="${rest%%.*}"
other="$major.$((minor + 1)).0"
for case in "other-rustc:rustc $other (0 x):cargo $version (0 x)" \
            "other-cargo:rustc $version (0 x):cargo $other (0 x)" \
            "nightly:rustc $version-nightly (0 x):cargo $version-nightly (0 x)" \
            "no-cargo:rustc $version (0 x):"; do
    name="${case%%:*}"; rest="${case#*:}"; r="${rest%%:*}"; c="${rest#*:}"
    if [ "$name" = no-cargo ] && command -v cargo >/dev/null 2>&1 && [ "$(dirname "$(command -v cargo)")" = /usr/bin ]; then
        echo "ci-pin-rust: skip no-cargo (a cargo in /usr/bin would be found)"; continue
    fi
    fake "$work/$name" "$r" "$c"
    rc=$(run_check "$work/$name")
    if [ "$rc" -eq 1 ]; then
        ok "--check: $name -> exit 1 ($(head -1 "$work/out.txt"))"
    else
        fail "--check: $name -> exit $rc, want 1"; sed 's/^/    /' "$work/out.txt"
    fi
done

[ "$status" -eq 0 ] && echo "PASS: ci-pin-rust-test" || echo "FAIL: ci-pin-rust-test"
exit "$status"
