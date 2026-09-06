#!/usr/bin/env bash
# Fetch external Cg shader corpora at pinned commits.
#
# Public metadata deliberately excludes private tool identities and paths.
set -euo pipefail

SCRIPT_NAME="$(basename "$0")"
DEFAULT_ROOT="build/shader-corpus"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd -P)"

die() {
    printf '%s: ERROR: %s\n' "$SCRIPT_NAME" "$*" >&2
    exit 1
}

say() {
    printf '[shader-corpus] %s\n' "$*"
}

usage() {
    cat <<EOF
Usage:
  $SCRIPT_NAME --list
  $SCRIPT_NAME --dry-run
  $SCRIPT_NAME --check-root <dir>
  $SCRIPT_NAME [--all | --source <id>] [--root <dir>]

Options:
  --list         Print the source/license/count table as CSV.
  --dry-run      Validate source metadata without network access.
  --check-root   Validate whether a destination is safe for fetched sources.
  --all          Fetch every non-excluded source. This is the default action.
  --source <id>  Fetch one source by id.
  --root <dir>   Destination root. Default: $DEFAULT_ROOT.
  -h, --help     Show this help.

Fetched files are local test inputs only. GPL and ambiguous-license sources are
never committed; keep the destination under a gitignored build directory.
EOF
}

SOURCE_IDS=(
    ps3-open-graphics-toolkit
    ioquake3-ps3
    th06-ps3
    classicube
    crystalct-psl1ght
    rsxgl
    libretro-common-shaders
    ogre-cg-samples
    ogre-examples
    shader-tut
)

declare -A NAME REPO REF PATHS LICENSE USAGE TRACKED DISTINCT CG VCG FCG NOTES

NAME[ps3-open-graphics-toolkit]="PS3 OpenGraphics Toolkit"
REPO[ps3-open-graphics-toolkit]="https://github.com/Cruslan/PS3-OpenGraphics-Toolkit.git"
REF[ps3-open-graphics-toolkit]="77532781e45f0d7734edf094e33d03fbc97c7024"
PATHS[ps3-open-graphics-toolkit]="testsuite/shaders"
LICENSE[ps3-open-graphics-toolkit]="GPL-2.0"
USAGE[ps3-open-graphics-toolkit]="fetch-run-only"
TRACKED[ps3-open-graphics-toolkit]=87
DISTINCT[ps3-open-graphics-toolkit]=87
CG[ps3-open-graphics-toolkit]=0
VCG[ps3-open-graphics-toolkit]=1
FCG[ps3-open-graphics-toolkit]=86
NOTES[ps3-open-graphics-toolkit]="86 feature shaders plus one vertex shader"

NAME[ioquake3-ps3]="IoQuake3-PS3"
REPO[ioquake3-ps3]="https://github.com/Mayo1970/IoQuake3-PS3.git"
REF[ioquake3-ps3]="4725af4aa4c179e516010eebcf32b75428c0ae40"
PATHS[ioquake3-ps3]="code/gl/shaders"
LICENSE[ioquake3-ps3]="GPL-2.0"
USAGE[ioquake3-ps3]="fetch-run-only"
TRACKED[ioquake3-ps3]=8
DISTINCT[ioquake3-ps3]=8
CG[ioquake3-ps3]=0
VCG[ioquake3-ps3]=1
FCG[ioquake3-ps3]=7
NOTES[ioquake3-ps3]="port-validation shader set"

NAME[th06-ps3]="th06_ps3"
REPO[th06-ps3]="https://github.com/kan8223-dotcom/th06_ps3.git"
REF[th06-ps3]="a58159b9928254bc9a59fd8028c929f12c86b02c"
PATHS[th06-ps3]="ps3/shaders"
LICENSE[th06-ps3]="GPL-3.0"
USAGE[th06-ps3]="fetch-run-only"
TRACKED[th06-ps3]=11
DISTINCT[th06-ps3]=11
CG[th06-ps3]=0
VCG[th06-ps3]=1
FCG[th06-ps3]=10
NOTES[th06-ps3]="port shader set"

NAME[classicube]="ClassiCube"
REPO[classicube]="https://github.com/ClassiCube/ClassiCube.git"
REF[classicube]="d7ee58bdbccb1f9f72750bdf79f96550dfdd3ea0"
PATHS[classicube]="misc/ps3"
LICENSE[classicube]="BSD-3-Clause-style-with-GitHub-NOASSERTION"
USAGE[classicube]="fetch-run-only"
TRACKED[classicube]=5
DISTINCT[classicube]=5
CG[classicube]=0
VCG[classicube]=3
FCG[classicube]=2
NOTES[classicube]="ambiguous-license-vendoring-requires-director-determination"

NAME[crystalct-psl1ght]="crystalct PSL1GHT develop"
REPO[crystalct-psl1ght]="https://github.com/crystalct/PSL1GHT.git"
REF[crystalct-psl1ght]="f7eda8960670cf67a63fcc22e11c9b4e485b6e9d"
PATHS[crystalct-psl1ght]="samples/graphics"
LICENSE[crystalct-psl1ght]="MIT"
USAGE[crystalct-psl1ght]="vendor-eligible"
TRACKED[crystalct-psl1ght]=14
DISTINCT[crystalct-psl1ght]=6
CG[crystalct-psl1ght]=0
VCG[crystalct-psl1ght]=7
FCG[crystalct-psl1ght]=7
NOTES[crystalct-psl1ght]="six-distinct-blobs-across-fourteen-paths"

NAME[rsxgl]="RSXGL"
REPO[rsxgl]="https://github.com/gzorin/RSXGL.git"
REF[rsxgl]="835ecd3b39b0fc96bd09a31b5fe1e93c090bf5f3"
PATHS[rsxgl]="src/samples/rsxgltest"
LICENSE[rsxgl]="BSD-2-Clause-style"
USAGE[rsxgl]="vendor-eligible"
TRACKED[rsxgl]=10
DISTINCT[rsxgl]=10
CG[rsxgl]=0
VCG[rsxgl]=5
FCG[rsxgl]=5
NOTES[rsxgl]="permissive-corpus-candidate"

# Internet survey, 2026-09-06 (t_e5233251).  Programs are (source, entry,
# profile) triples: libretro files carry main_fragment AND main_vertex, and
# the OGRE files name their entries by hand, so a sweep must pass -e.

NAME[libretro-common-shaders]="libretro common-shaders"
REPO[libretro-common-shaders]="https://github.com/libretro/common-shaders.git"
REF[libretro-common-shaders]="9c0d839a19651dffc9898da7673574a20fb39415"
PATHS[libretro-common-shaders]="."
LICENSE[libretro-common-shaders]="mixed-per-file-no-repo-license"
USAGE[libretro-common-shaders]="fetch-run-only"
TRACKED[libretro-common-shaders]=577
DISTINCT[libretro-common-shaders]=572
CG[libretro-common-shaders]=577
VCG[libretro-common-shaders]=0
FCG[libretro-common-shaders]=0
NOTES[libretro-common-shaders]="no-repo-license;191-files-GPL-marked;146-permissive-marked;rest-unmarked;entries-main_fragment-main_vertex"

NAME[ogre-cg-samples]="OGRE sample Cg programs"
REPO[ogre-cg-samples]="https://github.com/OGRECave/ogre.git"
REF[ogre-cg-samples]="d491e96bfe7bb57effcc57521bed4fd55fa7cfe3"
PATHS[ogre-cg-samples]="Samples/Media/materials/programs"
LICENSE[ogre-cg-samples]="MIT"
USAGE[ogre-cg-samples]="vendor-eligible"
TRACKED[ogre-cg-samples]=41
DISTINCT[ogre-cg-samples]=41
CG[ogre-cg-samples]=41
VCG[ogre-cg-samples]=0
FCG[ogre-cg-samples]=0
NOTES[ogre-cg-samples]="MIT-repo-LICENSE-Torus-Knot;hand-named-entries;vendoring-awaits-director-word"

NAME[ogre-examples]="ogre-examples"
REPO[ogre-examples]="https://github.com/qknight/ogre-examples.git"
REF[ogre-examples]="6aa4c56e89b5808b996da4be2b961d40e3d1a582"
PATHS[ogre-examples]="."
LICENSE[ogre-examples]="none"
USAGE[ogre-examples]="excluded"
TRACKED[ogre-examples]=4
DISTINCT[ogre-examples]=3
CG[ogre-examples]=4
VCG[ogre-examples]=0
FCG[ogre-examples]=0
NOTES[ogre-examples]="no-license-file-anywhere-in-repo;surveyed-locally-only"

NAME[shader-tut]="Shader-Tut"
REPO[shader-tut]="https://github.com/Craig-Macomber/Shader-Tut.git"
REF[shader-tut]="7bca8438a398529894d0cf63d731cb5bf7d3bdb5"
PATHS[shader-tut]="."
LICENSE[shader-tut]="none"
USAGE[shader-tut]="excluded"
TRACKED[shader-tut]=0
DISTINCT[shader-tut]=0
CG[shader-tut]=0
VCG[shader-tut]=0
FCG[shader-tut]=0
NOTES[shader-tut]="no-Cg-sources-at-this-ref-and-no-license;listed-so-the-survey-is-not-repeated"

csv_escape() {
    local value="${1//\"/\"\"}"
    case "$value" in
        *[,\"]*) printf '"%s"' "$value" ;;
        *)       printf '%s' "$value" ;;
    esac
}

print_list() {
    printf 'id,name,repo,ref,path,license,usage,tracked_files,distinct_blobs,cg,vcg,fcg,notes\n'
    local id
    for id in "${SOURCE_IDS[@]}"; do
        csv_escape "$id"; printf ','
        csv_escape "${NAME[$id]}"; printf ','
        csv_escape "${REPO[$id]}"; printf ','
        csv_escape "${REF[$id]}"; printf ','
        csv_escape "${PATHS[$id]}"; printf ','
        csv_escape "${LICENSE[$id]}"; printf ','
        csv_escape "${USAGE[$id]}"; printf ','
        csv_escape "${TRACKED[$id]}"; printf ','
        csv_escape "${DISTINCT[$id]}"; printf ','
        csv_escape "${CG[$id]}"; printf ','
        csv_escape "${VCG[$id]}"; printf ','
        csv_escape "${FCG[$id]}"; printf ','
        csv_escape "${NOTES[$id]}"; printf '\n'
    done
}

validate_metadata() {
    local id total_sources=0 total_tracked=0 total_distinct=0
    for id in "${SOURCE_IDS[@]}"; do
        [[ -n "${NAME[$id]:-}" ]] || die "$id missing name"
        [[ -n "${REPO[$id]:-}" ]] || die "$id missing repo"
        [[ -n "${REF[$id]:-}" ]] || die "$id missing ref"
        [[ -n "${PATHS[$id]:-}" ]] || die "$id missing path"
        [[ -n "${LICENSE[$id]:-}" ]] || die "$id missing license"
        [[ -n "${USAGE[$id]:-}" ]] || die "$id missing usage"
        [[ "${USAGE[$id]}" == "fetch-run-only" || "${USAGE[$id]}" == "vendor-eligible" \
            || "${USAGE[$id]}" == "excluded" ]] \
            || die "$id has unsupported usage ${USAGE[$id]}"
        # An excluded source stays in the table so the survey is not repeated,
        # and it must say why it is excluded.
        if [[ "${USAGE[$id]}" == "excluded" ]]; then
            [[ -n "${NOTES[$id]:-}" ]] || die "$id is excluded without a reason in NOTES"
        fi

        local extension_total=$(( CG[$id] + VCG[$id] + FCG[$id] ))
        [[ "$extension_total" -eq "${TRACKED[$id]}" ]] \
            || die "$id extension total $extension_total does not match tracked ${TRACKED[$id]}"
        [[ "${DISTINCT[$id]}" -le "${TRACKED[$id]}" ]] \
            || die "$id distinct count exceeds tracked count"

        # Vendoring is redistribution, so this is an ALLOWLIST, not a
        # denylist: a vendor-eligible source must carry a licence the project
        # has decided it can ship, and everything else - including a licence
        # string nobody has classified yet - is refused by default.  A
        # denylist would green-light a GPL set the moment someone flipped its
        # usage, because the guard would not recognise the string to reject
        # it.  Add a licence here only as a deliberate decision.
        if [[ "${USAGE[$id]}" == "vendor-eligible" ]]; then
            # EXACT values only, never a prefix glob: `BSD-3-Clause-*` would
            # admit "BSD-3-Clause-style-with-GitHub-NOASSERTION", which carries
            # the very NOASSERTION ambiguity this guard exists to reject.  The
            # allowlist is the set of licence strings the project has actually
            # reviewed and decided it can redistribute; adding one is a
            # deliberate edit, and the string must be the exact metadata value.
            case "${LICENSE[$id]}" in
                MIT) ;;
                BSD-2-Clause) ;;
                BSD-2-Clause-style) ;;  # rsxgl, reviewed
                BSD-3-Clause) ;;
                Zlib) ;;
                zlib) ;;
                *) die "$id is vendor-eligible but its licence '${LICENSE[$id]}' is not on the vendorable allowlist (exact match: MIT, BSD-2-Clause, BSD-2-Clause-style, BSD-3-Clause, Zlib); classify it fetch-run-only, or add the exact licence string here on purpose" ;;
            esac
        fi

        total_sources=$((total_sources + 1))
        total_tracked=$((total_tracked + TRACKED[$id]))
        total_distinct=$((total_distinct + DISTINCT[$id]))
    done
    printf 'metadata ok: %d sources, %d tracked files, %d distinct shader blobs\n' \
        "$total_sources" "$total_tracked" "$total_distinct"
}

require_id() {
    local want="$1" id
    for id in "${SOURCE_IDS[@]}"; do
        [[ "$id" == "$want" ]] && return 0
    done
    die "unknown source id: $want"
}

resolve_path_for_guard() {
    local root="$1"

    if [[ -d "$root" ]]; then
        cd "$root" && pwd -P
        return 0
    fi

    local parent base
    parent="$(dirname "$root")"
    base="$(basename "$root")"
    while [[ ! -d "$parent" ]]; do
        base="$(basename "$parent")/$base"
        parent="$(dirname "$parent")"
    done

    local abs_parent
    abs_parent="$(cd "$parent" && pwd -P)"
    printf '%s/%s\n' "$abs_parent" "$base"
}

nearest_existing_parent() {
    local path="$1"
    local parent
    if [[ -d "$path" ]]; then
        parent="$path"
    else
        parent="$(dirname "$path")"
    fi
    while [[ ! -d "$parent" ]]; do
        parent="$(dirname "$parent")"
    done
    cd "$parent" && pwd -P
}

guard_destination_root() {
    local root="$1"

    local abs_root
    abs_root="$(resolve_path_for_guard "$root")"

    local owning_worktree=""
    local existing_parent
    existing_parent="$(nearest_existing_parent "$abs_root")"
    if git -C "$existing_parent" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        owning_worktree="$(git -C "$existing_parent" rev-parse --show-toplevel)"
        owning_worktree="$(cd "$owning_worktree" && pwd -P)"
        # Checking the root is enough: Git cannot re-include files beneath an
        # ignored parent directory, so a nested negation cannot make fetched
        # shader files trackable after this passes.
        git -C "$owning_worktree" check-ignore -q -- "$abs_root" \
            || die "destination is not gitignored - refusing to fetch licensed sources into a trackable path: $abs_root"
    fi

    if [[ "$owning_worktree" == "$REPO_ROOT" ]]; then
        case "$abs_root" in
            "$REPO_ROOT/build/shader-corpus"|"$REPO_ROOT/build/shader-corpus"/*) ;;
            *) die "destination inside this repository must be under $REPO_ROOT/build/shader-corpus: $abs_root" ;;
        esac
    fi

    mkdir -p "$root"
    printf 'destination ok: %s\n' "$abs_root"
}

copy_license_file() {
    local checkout="$1" dest="$2"
    local candidate
    for candidate in LICENSE LICENSE.txt COPYING COPYING.txt license.txt; do
        if [[ -f "$checkout/$candidate" ]]; then
            cp "$checkout/$candidate" "$dest/$candidate"
            return 0
        fi
    done
    say "warning: no root license file found in $(basename "$dest")"
}

copy_shader_paths() {
    local checkout="$1" dest="$2" paths="$3"
    local path
    mkdir -p "$dest/files"
    for path in $paths; do
        [[ -e "$checkout/$path" ]] || die "missing path in checkout: $path"
        (cd "$checkout" && find "$path" -type f \( -name '*.cg' -o -name '*.vcg' -o -name '*.fcg' \) -print) |
            while IFS= read -r shader; do
                mkdir -p "$dest/files/$(dirname "$shader")"
                cp "$checkout/$shader" "$dest/files/$shader"
            done
    done
}

count_files() {
    local dir="$1" ext="$2"
    find "$dir" -type f -name "*.$ext" | wc -l | tr -d ' '
}

write_source_manifest() {
    local id="$1" dest="$2"
    cat >"$dest/SOURCE.txt" <<EOF
id=${id}
name=${NAME[$id]}
repo=${REPO[$id]}
ref=${REF[$id]}
path=${PATHS[$id]}
license=${LICENSE[$id]}
usage=${USAGE[$id]}
tracked_files=${TRACKED[$id]}
distinct_blobs=${DISTINCT[$id]}
cg=${CG[$id]}
vcg=${VCG[$id]}
fcg=${FCG[$id]}
notes=${NOTES[$id]}
EOF
}

verify_fetched_counts() {
    local id="$1" dest="$2"
    local cg vcg fcg total
    cg="$(count_files "$dest/files" cg)"
    vcg="$(count_files "$dest/files" vcg)"
    fcg="$(count_files "$dest/files" fcg)"
    total=$((cg + vcg + fcg))
    [[ "$cg" -eq "${CG[$id]}" ]] || die "$id fetched .cg=$cg expected ${CG[$id]}"
    [[ "$vcg" -eq "${VCG[$id]}" ]] || die "$id fetched .vcg=$vcg expected ${VCG[$id]}"
    [[ "$fcg" -eq "${FCG[$id]}" ]] || die "$id fetched .fcg=$fcg expected ${FCG[$id]}"
    [[ "$total" -eq "${TRACKED[$id]}" ]] || die "$id fetched total=$total expected ${TRACKED[$id]}"
}

fetch_source() {
    local id="$1" root="$2"
    require_id "$id"
    # An excluded source is a decision, not an oversight, so naming it
    # explicitly is refused rather than silently honoured; its NOTES say why
    # it was excluded.  Fetching is not redistribution, so un-excluding one a
    # surveyor genuinely needs is a one-line edit here, made on purpose.
    [[ "${USAGE[$id]}" != "excluded" ]] || die "$id is excluded (${NOTES[$id]:-see the licence table}); it is not fetched"

    command -v git >/dev/null 2>&1 || die "git not found"
    command -v find >/dev/null 2>&1 || die "find not found"

    local abs_root
    guard_destination_root "$root" >/dev/null
    abs_root="$(cd "$root" && pwd -P)"

    local work="$abs_root/_work/$id"
    local dest="$abs_root/$id"
    rm -rf "$work" "$dest.tmp"
    mkdir -p "$abs_root/_work"

    say "fetching $id @ ${REF[$id]}"
    git init --quiet "$work"
    git -C "$work" remote add origin "${REPO[$id]}"
    git -C "$work" fetch --quiet --depth=1 origin "${REF[$id]}"
    git -C "$work" checkout --quiet --detach FETCH_HEAD

    mkdir -p "$dest.tmp"
    copy_shader_paths "$work" "$dest.tmp" "${PATHS[$id]}"
    copy_license_file "$work" "$dest.tmp"
    write_source_manifest "$id" "$dest.tmp"
    verify_fetched_counts "$id" "$dest.tmp"

    rm -rf "$dest"
    mv "$dest.tmp" "$dest"
    say "ok $id: ${TRACKED[$id]} shader files"
}

ACTION="fetch"
ROOT="$DEFAULT_ROOT"
CHECK_ROOT=""
ONLY=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --list) ACTION="list"; shift ;;
        --dry-run) ACTION="dry-run"; shift ;;
        --check-root) [[ $# -ge 2 ]] || die "--check-root needs a directory"; ACTION="check-root"; CHECK_ROOT="$2"; shift 2 ;;
        --check-root=*) ACTION="check-root"; CHECK_ROOT="${1#*=}"; shift ;;
        --all) ACTION="fetch"; ONLY=(); shift ;;
        --source) [[ $# -ge 2 ]] || die "--source needs an id"; ONLY+=("$2"); shift 2 ;;
        --source=*) ONLY+=("${1#*=}"); shift ;;
        --root) [[ $# -ge 2 ]] || die "--root needs a directory"; ROOT="$2"; shift 2 ;;
        --root=*) ROOT="${1#*=}"; shift ;;
        -h|--help) usage; exit 0 ;;
        *) die "unknown argument: $1" ;;
    esac
done

# An explicit --source naming an excluded source is refused for every action
# that consumes the selection, dry-run included: the first question a careful
# user asks is "what would this do?", and the answer for an excluded source is
# "nothing", not "ok".  require_id also rejects an unknown id here.
for id in "${ONLY[@]:-}"; do
    [[ -n "$id" ]] || continue
    require_id "$id"
    [[ "${USAGE[$id]}" != "excluded" ]] \
        || die "$id is excluded (${NOTES[$id]:-see the licence table}); it is not fetched"
done

case "$ACTION" in
    list)
        print_list
        ;;
    dry-run)
        validate_metadata
        ;;
    check-root)
        [[ -n "$CHECK_ROOT" ]] || die "--check-root needs a directory"
        guard_destination_root "$CHECK_ROOT"
        ;;
    fetch)
        validate_metadata
        if [[ ${#ONLY[@]} -eq 0 ]]; then
            # --all means every non-excluded source, as the help text says.
            for id in "${SOURCE_IDS[@]}"; do
                [[ "${USAGE[$id]}" == "excluded" ]] || ONLY+=("$id")
            done
        fi
        for id in "${ONLY[@]}"; do
            fetch_source "$id" "$ROOT"
        done
        say "complete: $ROOT"
        ;;
    *)
        die "internal action error: $ACTION"
        ;;
esac
