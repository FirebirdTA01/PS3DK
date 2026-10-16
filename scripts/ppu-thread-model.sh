# Thread-model rules for the PPU GCC build, sourced by build-ppu-toolchain.sh and by
# tests/host/ppu-thread-model-test.sh.  Defines functions only.
#
# The model (single or posix) changes libstdc++'s ABI (shared_ptr's lock policy, static-init guards), so a GCC
# build tree, an installed prefix and, for a Windows-hosted (--host) build, the native stage whose target libraries
# it copies must all carry the same one.  Each is stamped: <obj>/.threads-model, <prefix>/.ps3dk-thread-model.

# ppu_threads_default <gcc major>: posix for GCC 16, single otherwise.
ppu_threads_default() {
    if [[ "$1" == 16 ]]; then echo posix; else echo single; fi
}

# ppu_gcc_threads_arg <model>: GCC's configure argument for the model.  Both host configurations (the native
# combined build and the --host all-host build) pass it: GCC's configure defaults an omitted --enable-threads to
# "single" for this target, whatever the target libraries were built with.
ppu_gcc_threads_arg() {
    echo "--enable-threads=$1"
}

# ppu_stamp <file>: the stamp's model, or "missing".
ppu_stamp() {
    if [[ -f "$1" ]]; then head -n 1 "$1"; else echo missing; fi
}

# ppu_stage_model_decision <want> <obj> <prefix> [<native prefix>]
#   One word for the GCC step, checked before any reuse of <obj> and before its early return:
#     native     the --host build's native source stage is unstamped or has another model (fatal: rebuild it first);
#     rebuild    <obj> exists and its stamp is missing or differs: remove the tree;
#     reinstall  the tree matches but <prefix> is unstamped or has another model: configure, build and install again;
#     ok         everything carries <want>.
ppu_stage_model_decision() {
    local want=$1 obj=$2 prefix=$3 native=${4:-}
    if [[ -n "$native" && "$(ppu_stamp "$native/.ps3dk-thread-model")" != "$want" ]]; then
        echo native; return 0
    fi
    if [[ -d "$obj" && "$(ppu_stamp "$obj/.threads-model")" != "$want" ]]; then
        echo rebuild; return 0
    fi
    if [[ -f "$obj/.installed" && "$(ppu_stamp "$prefix/.ps3dk-thread-model")" != "$want" ]]; then
        echo reinstall; return 0
    fi
    echo ok
}

# ppu_configured_threads <obj>: the --enable-threads value GCC's top-level configure was actually run with (from
# config.status), or "none".
ppu_configured_threads() {
    local v
    v=$(grep -o -- "--enable-threads=[a-z0-9]*" "$1/config.status" 2>/dev/null | head -n 1)
    if [[ -n "$v" ]]; then echo "${v#--enable-threads=}"; else echo none; fi
}
