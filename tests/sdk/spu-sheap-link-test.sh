#!/usr/bin/env bash
# The SDK installs an SPU libsheap.a that defines the 18 public cellSheap /
# cellKeySheap functions and nothing else outside its reserved __sheap_
# prefix, and a program that calls all 18 plus every inline wrapper of the
# SPU <cell/sheap.h> links against -lsheap -lsync -ldma -latomic with no
# undefined symbols, in C and in C++.
#
# usage: spu-sheap-link-test.sh [--ps3dev DIR]
#   the archives and headers come from $PS3DK (default DIR/ps3dk)
set -u
ps3dev="${PS3DEV:-}"
while [ $# -gt 0 ]; do
    case "$1" in
        --ps3dev) ps3dev="$2"; shift 2 ;;
        *) echo "usage: $0 [--ps3dev DIR]" >&2; exit 2 ;;
    esac
done
if [ -z "$ps3dev" ]; then
    echo "spu-sheap-link: SKIP (set PS3DEV or --ps3dev)"
    exit 0
fi
sdk="${PS3DK:-$ps3dev/ps3dk}"
cc="$ps3dev/spu/bin/spu-elf-gcc"
nm="$ps3dev/spu/bin/spu-elf-nm"
status=0
fail() { echo "spu-sheap-link: FAIL: $*"; status=1; }
ok() { echo "spu-sheap-link: ok   $*"; }
[ -x "$cc" ] && [ -x "$nm" ] || { fail "no SPU compiler under $ps3dev"; exit 1; }
lib="$sdk/spu/lib/libsheap.a"
[ -f "$lib" ] || { fail "no $lib"; exit 1; }
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

public="cellSheapInitialize cellSheapAllocate cellSheapFree cellSheapQueryMax
cellSheapQueryFree cellKeySheapInitialize
cellKeySheapBufferNew cellKeySheapBufferDelete cellKeySheapMutexNew
cellKeySheapMutexDelete cellKeySheapBarrierNew cellKeySheapBarrierDelete
cellKeySheapQueueNew cellKeySheapQueueDelete cellKeySheapRwmNew
cellKeySheapRwmDelete cellKeySheapSemaphoreNew cellKeySheapSemaphoreDelete"

# The archive's global definitions: the 18 public names, the rest reserved.
"$nm" -g --defined-only "$lib" 2>&1 | awk 'NF == 3 { print $3 }' | sort > "$work/defined.txt"
for name in $public; do
    n=$(grep -cx "$name" "$work/defined.txt")
    [ "$n" = 1 ] && ok "defines $name" || fail "$name defined $n times"
done
extra=$(grep -v -x -F "$(printf '%s\n' $public)" "$work/defined.txt" | grep -v '^__sheap_' || true)
[ -z "$extra" ] && ok "no other global names outside __sheap_" || fail "unexpected globals: $(echo $extra)"
"$nm" -u "$lib" | awk 'NF == 2 { print $2 }' | sort -u > "$work/imports.txt"
echo "spu-sheap-link: info archive imports: $(comm -23 "$work/imports.txt" "$work/defined.txt" | tr '\n' ' ')"

cat > "$work/all.c" <<'EOF'
#include <cell/sheap.h>

static unsigned char element[128] __attribute__((aligned(128)));

int main(void)
{
    const uint64_t heap = 0x10000, keyed = 0x20000;
    CellKeySheapBuffer buffer;
    CellKeySheapMutex mutex;
    CellKeySheapBarrier barrier;
    CellKeySheapQueue queue;
    CellKeySheapRwm rwm;
    CellKeySheapSemaphore semaphore;
    uint64_t block;
    int rc = 0;

    rc |= cellSheapInitialize(heap, 10240, 1);
    block = cellSheapAllocate(heap, 512);
    rc |= cellSheapQueryMax(heap) + cellSheapQueryFree(heap);
    rc |= cellSheapFree(heap, block);

    rc |= cellKeySheapInitialize(keyed, 10240, 2);
    block = cellKeySheapAllocate(keyed, 128);
    rc |= cellKeySheapQueryMax(keyed) + cellKeySheapQueryFree(keyed);
    rc |= cellKeySheapFree(keyed, block);

    rc |= cellKeySheapBufferNew(&buffer, keyed, 1, 256);
    rc |= (int)(cellKeySheapBufferGetEa(&buffer) + cellKeySheapBufferGetSize(&buffer));
    cellKeySheapBufferDelete(&buffer);

    rc |= cellKeySheapMutexNew(&mutex, keyed, 2);
    rc |= cellKeySheapMutexLock(&mutex) | cellKeySheapMutexTryLock(&mutex)
        | cellKeySheapMutexUnlock(&mutex);
    cellKeySheapMutexDelete(&mutex);

    rc |= cellKeySheapBarrierNew(&barrier, keyed, 3, 2);
    rc |= cellKeySheapBarrierNotify(&barrier) | cellKeySheapBarrierTryNotify(&barrier)
        | cellKeySheapBarrierWait(&barrier) | cellKeySheapBarrierTryWait(&barrier);
    cellKeySheapBarrierDelete(&barrier);

    rc |= cellKeySheapQueueNew(&queue, keyed, 4, 128, 4);
    rc |= cellKeySheapQueuePush(&queue, element, 3) | cellKeySheapQueueTryPush(&queue, element, 3)
        | cellKeySheapQueuePop(&queue, element, 3) | cellKeySheapQueueTryPop(&queue, element, 3)
        | cellKeySheapQueuePeek(&queue, element, 3) | cellKeySheapQueueTryPeek(&queue, element, 3)
        | (int)cellKeySheapQueueSize(&queue) | cellKeySheapQueueClear(&queue);
    cellKeySheapQueueDelete(&queue);

    rc |= cellKeySheapRwmNew(&rwm, keyed, 5, 128);
    rc |= cellKeySheapRwmReadBegin(&rwm, element, 4) | cellKeySheapRwmTryReadBegin(&rwm, element, 4)
        | cellKeySheapRwmReadEnd(&rwm, 4) | cellKeySheapRwmWrite(&rwm, element, 4)
        | cellKeySheapRwmTryWrite(&rwm, element, 4);
    cellKeySheapRwmDelete(&rwm);

    rc |= cellKeySheapSemaphoreNew(&semaphore, keyed, 6, 1);
    cellKeySheapSemaphoreP(&semaphore);
    cellKeySheapSemaphoreV(&semaphore);
    rc |= cellKeySheapSemaphoreTryP(&semaphore);
    cellKeySheapSemaphoreDelete(&semaphore);

    return rc != 0;
}
EOF
cp "$work/all.c" "$work/all.cpp"
libs="-L$sdk/spu/lib -lsheap -lsync -ldma -latomic"
for src in all.c all.cpp; do
    extra_flags=""
    [ "$src" = all.cpp ] && extra_flags="-fno-exceptions -std=c++17"
    if "$cc" -O2 -Wall -Wextra -Werror $extra_flags -I"$sdk/spu/include" "$work/$src" $libs \
           -o "$work/$src.elf" > "$work/$src.log" 2>&1; then
        # Weak references (w) from the startup files are allowed.
        undefined=$("$nm" -u "$work/$src.elf" | awk '$1 == "U" { print $2 }' | tr '\n' ' ')
        [ -z "$undefined" ] && ok "$src links with no undefined symbols" \
                            || fail "$src leaves undefined: $undefined"
        missing=""
        for name in $public; do
            "$nm" "$work/$src.elf" | grep -q " T $name\$" || missing="$missing $name"
        done
        [ -z "$missing" ] && ok "$src defines all 18 public functions" || fail "$src lacks:$missing"
    else
        fail "$src: $(grep -m3 -E 'error|undefined' "$work/$src.log" | tr '\n' ' ')"
    fi
done

[ "$status" -eq 0 ] && echo "spu-sheap-link: PASS"
exit $status
