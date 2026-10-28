#!/usr/bin/env bash
# git pre-push hook: refuse a push whose ref names or newly published commits
# (messages, added paths, and lines added by any of the commits, read from
# the committed blobs) fail scripts/neutral-wording-scan.py.
#
# "Newly published" means not reachable from anything the remote already
# has: git rev-list <local sha> --not --remotes=<remote> <old remote sha>.
# So a rebased topic pushed with --force-with-lease is checked only for its
# own new commits, not for the main-branch history it now sits on, and a new
# branch is checked the same way.  Deleting a ref checks nothing.
#
# git passes the remote name and URL as arguments and one line per ref:
#   <local ref> <local sha> <remote ref> <remote sha>
# Install with: bash scripts/hooks/install-neutral-wording-hook.sh
set -euo pipefail
remote=${1:-origin}
root=$(git rev-parse --show-toplevel)
scan=(python3 -I -B "$root/scripts/neutral-wording-scan.py")
command -v python3 >/dev/null 2>&1 || scan=(python -I -B "$root/scripts/neutral-wording-scan.py")
zero=0000000000000000000000000000000000000000
status=0
while read -r local_ref local_sha remote_ref remote_sha; do
    [ "$local_sha" = "$zero" ] && continue          # deleting a ref
    "${scan[@]}" --names "$local_ref" "$remote_ref" || status=1
    exclude=(--not "--remotes=$remote")
    if [ "$remote_sha" != "$zero" ] && git cat-file -e "$remote_sha^{commit}" 2>/dev/null; then
        exclude+=("$remote_sha")
    fi
    "${scan[@]}" --commits "$local_sha" "${exclude[@]}" || status=1
done
if [ "$status" -ne 0 ]; then
    echo "pre-push: refused by the neutral-wording guard (see above)." >&2
fi
exit "$status"
