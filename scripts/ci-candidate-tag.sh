#!/usr/bin/env bash
# Release-candidate builds (release.yml workflow_dispatch with candidate_tag):
# give this job's checkout the version inputs of the tag build without a
# public tag.  Tags HEAD locally with a lightweight tag that is never pushed,
# then requires scripts/version.sh to emit exactly that tag - the same check
# a tag push runs.  The zip built from the dispatch then carries the version
# the tag build will, so the pre-release gate tests the same inputs.
#
# usage: ci-candidate-tag.sh vMAJOR.MINOR.PATCH
set -euo pipefail
tag="${1:?usage: $0 vMAJOR.MINOR.PATCH}"
if ! [[ "$tag" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "::error::candidate_tag '$tag' is not vMAJOR.MINOR.PATCH" >&2
    exit 1
fi
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
head=$(git rev-parse HEAD)
if existing=$(git rev-parse -q --verify "refs/tags/$tag^{commit}"); then
    # A published tag of that name must already be this very commit.
    if [ "$existing" != "$head" ]; then
        echo "::error::tag $tag already exists at $existing, not at HEAD $head" >&2
        exit 1
    fi
else
    git tag "$tag" "$head"
fi
version=$(./scripts/version.sh --format=plain)
if [ "$version" != "$tag" ]; then
    echo "::error::with $tag at HEAD, version.sh emits $version" >&2
    exit 1
fi
echo "candidate: $tag at $head (local tag, not pushed); version.sh = $version"
