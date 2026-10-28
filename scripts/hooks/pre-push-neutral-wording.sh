#!/usr/bin/env bash
# git pre-push hook: refuse a push whose ref names or new commits (messages,
# added paths, and lines added by any of the commits, read from the
# committed blobs) fail scripts/neutral-wording-scan.py.  git passes one
# line per ref:
#   <local ref> <local sha> <remote ref> <remote sha>
# Install with: bash scripts/hooks/install-neutral-wording-hook.sh
set -euo pipefail
root=$(git rev-parse --show-toplevel)
scan=(python3 -I -B "$root/scripts/neutral-wording-scan.py")
command -v python3 >/dev/null 2>&1 || scan=(python -I -B "$root/scripts/neutral-wording-scan.py")
zero=0000000000000000000000000000000000000000
status=0
while read -r local_ref local_sha remote_ref remote_sha; do
    [ "$local_sha" = "$zero" ] && continue          # deleting a ref
    "${scan[@]}" --names "$local_ref" "$remote_ref" || status=1
    if [ "$remote_sha" = "$zero" ]; then
        # New remote branch: compare with whatever main the remote has.
        base=$(git merge-base "$local_sha" origin/main 2>/dev/null || true)
        [ -n "$base" ] || base=$(git rev-list --max-parents=0 "$local_sha" | tail -n 1)
        range="$base..$local_sha"
    else
        range="$remote_sha..$local_sha"
    fi
    "${scan[@]}" --range "$range" || status=1
done
if [ "$status" -ne 0 ]; then
    echo "pre-push: refused by the neutral-wording guard (see above)." >&2
fi
exit "$status"
