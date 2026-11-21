# shellcheck shell=bash
# Helpers for scripts/build-cell-stub-archives.sh (sourced; tested by tests/sdk/stub-archive-helpers-test.sh).

# make_clean_all <dir>: `make clean`, then `make all`, as two make invocations.  One `make clean all` lets a
# parallel make (MAKEFLAGS=-jN from the caller) run the two goals' recipes at once, so `all` can build objects that
# `clean` then deletes.  Variables assigned before the call (VAR=x make_clean_all dir) reach both makes.
make_clean_all() {
    make -C "$1" clean >/dev/null && make -C "$1" all >/dev/null
}

# nidgen_bin <toolchain root>: where `cargo build --release --manifest-path <root>/tools/nidgen/Cargo.toml` puts
# nidgen: $CARGO_TARGET_DIR/release/nidgen when it is set (relative to the current directory, as cargo reads it),
# otherwise the tools workspace's own target directory.
nidgen_bin() {
    local root="$1" target="${CARGO_TARGET_DIR:-}"
    if [[ -z "$target" ]]; then
        target="$root/tools/target"
    elif [[ "$target" != /* ]]; then
        target="$PWD/$target"
    fi
    printf '%s/release/nidgen\n' "$target"
}
