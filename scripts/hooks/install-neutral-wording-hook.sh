#!/usr/bin/env bash
# Install the neutral-wording pre-push hook for this clone and all of its
# worktrees (they share the common git dir).  Refuses to replace a different
# existing pre-push hook unless --force is given.
set -euo pipefail
root=$(git rev-parse --show-toplevel)
hooks=$(git rev-parse --git-common-dir)/hooks
src="$root/scripts/hooks/pre-push-neutral-wording.sh"
dst="$hooks/pre-push"
mkdir -p "$hooks"
if [ -e "$dst" ] && ! cmp -s "$src" "$dst" && [ "${1:-}" != "--force" ]; then
    echo "install-neutral-wording-hook: $dst exists and differs; rerun with --force to replace it" >&2
    exit 1
fi
cp "$src" "$dst"
chmod +x "$dst"
echo "install-neutral-wording-hook: installed $dst"
