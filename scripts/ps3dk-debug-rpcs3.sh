#!/bin/sh
# ps3dk-debug-rpcs3.sh -- boot a .fake.self in RPCS3 ready for GDB and attach.
#
#   ps3dk-debug-rpcs3.sh --self app.fake.self --gdbinit app.gdbinit
#                        [--gdb-server 127.0.0.1:2345] [--rpcs3 path/rpcs3]
#                        [--gdb path/powerpc64-ps3-elf-gdb] [--no-gdb]
#                        [--timeout 60]
#
# Normally run through the `debug-<target>` target that ps3_add_debug_rpcs3()
# adds. RPCS3 is started with a per-run copy of its configuration in which
# only two settings differ: the PPU decoder is the interpreter (RPCS3 ignores
# breakpoints under the LLVM recompiler), and the GDB server listens on
# --gdb-server. A marker beside the SELF enables the CMake helper's pre-main
# wait until GDB attaches. The shared RPCS3 configuration is never written.
#
# --rpcs3 defaults to $PS3DK_RPCS3, then `rpcs3` on PATH. The configuration
# is read from ${XDG_CONFIG_HOME:-$HOME/.config}/rpcs3/config.yml and the log
# from the same directory. --gdb defaults to the SDK's powerpc64-ps3-elf-gdb.
# RPCS3 has no detach. The launcher closes its RPCS3 process when local GDB exits.
set -eu

fail() { echo "ps3dk-debug-rpcs3: $*" >&2; exit 2; }

self= gdbinit= server=127.0.0.1:2345 rpcs3=${PS3DK_RPCS3:-} gdb= nogdb=0 timeout=60
while [ $# -gt 0 ]; do
    case $1 in
        --self) self=$2; shift 2 ;;
        --gdbinit) gdbinit=$2; shift 2 ;;
        --gdb-server) server=$2; shift 2 ;;
        --rpcs3) rpcs3=$2; shift 2 ;;
        --gdb) gdb=$2; shift 2 ;;
        --no-gdb) nogdb=1; shift ;;
        --timeout) timeout=$2; shift 2 ;;
        *) fail "unknown argument '$1'" ;;
    esac
done

[ -f "$self" ] || fail "no SELF at '$self' (build the target first)"
[ -f "$gdbinit" ] || fail "no GDB command file at '$gdbinit'"
echo "$server" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+:[0-9]+$' \
    || fail "--gdb-server must be IPv4 address:port, got '$server'"
[ -n "$rpcs3" ] || rpcs3=$(command -v rpcs3 || true)
[ -n "$rpcs3" ] && [ -x "$rpcs3" ] || fail "set --rpcs3 or PS3DK_RPCS3 to the rpcs3 executable"
if [ "$nogdb" -eq 0 ]; then
    if [ -z "$gdb" ]; then
        root=${PS3DK:-${PS3DEV:-}}
        [ -n "$root" ] && gdb=$root/ppu/bin/powerpc64-ps3-elf-gdb
    fi
    [ -n "$gdb" ] && [ -x "$gdb" ] || fail "no powerpc64-ps3-elf-gdb found (set --gdb, or use --no-gdb)"
fi

cfgdir=${XDG_CONFIG_HOME:-$HOME/.config}/rpcs3
shared=$cfgdir/config.yml
[ -f "$shared" ] || fail "no RPCS3 configuration at '$shared' (start RPCS3 once first)"
[ "$(grep -c '^[[:space:]]*PPU Decoder:' "$shared")" -eq 1 ] \
    && [ "$(grep -c '^[[:space:]]*GDB Server:' "$shared")" -eq 1 ] \
    || fail "unexpected RPCS3 configuration layout (expected one 'PPU Decoder' and 'GDB Server' line)"

selfdir=$(CDPATH= cd "$(dirname "$self")" && pwd)
waitmarker=$selfdir/ps3dk-debug-wait
owns_marker=0 pid= runcfg=
cleanup() {
    [ -z "$pid" ] || kill "$pid" 2>/dev/null || true
    [ "$owns_marker" -eq 0 ] || rm -f -- "$waitmarker"
    [ -z "$runcfg" ] || rm -f -- "$runcfg"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
(set -C; : > "$waitmarker") 2>/dev/null || fail "cannot create wait marker '$waitmarker'. Remove an existing marker only after confirming no debug session is running."
owns_marker=1
runcfg=$(mktemp "${TMPDIR:-/tmp}/ps3dk-debug-rpcs3.XXXXXX.yml")
sed -e 's/^\([[:space:]]*\)PPU Decoder:.*/\1PPU Decoder: Interpreter (static)/' \
    -e "s/^\\([[:space:]]*\\)GDB Server:.*/\\1GDB Server: $server/" "$shared" > "$runcfg"

log=$cfgdir/RPCS3.log
# The unique configuration path logged in argv identifies this launch;
# filesystem timestamps can lag or differ across mounts.
"$rpcs3" --no-gui --config "$runcfg" "$self" &
pid=$!

ready=0 waited=0
while [ "$waited" -lt $((timeout * 4)) ]; do
    kill -0 "$pid" 2>/dev/null || fail "RPCS3 exited before its GDB server started; see $log"
    if [ -f "$log" ] && grep -qF -- "$runcfg" "$log" \
        && grep -qF "GDB: Started listening on '$server'" "$log"; then
        ready=1; break
    fi
    sleep 0.25; waited=$((waited + 1))
done
[ "$ready" -eq 1 ] || fail "RPCS3's GDB server did not report listening on $server within ${timeout}s; see $log"

if [ "$nogdb" -eq 1 ]; then
    echo "ps3dk-debug-rpcs3: RPCS3 is waiting for GDB on $server."
    echo "  attach with: powerpc64-ps3-elf-gdb -x '$gdbinit'"
    echo "  Press Ctrl-C in this launcher terminal to end the session and close RPCS3."
    status=0
    wait "$pid" || status=$?
    pid=
    exit "$status"
fi
"$gdb" -x "$gdbinit"
