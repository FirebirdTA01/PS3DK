# ps3dk-run: the console side of the network launcher, built with the PS3DK toolchain like any sample.
#   loader/  ps3dk-loader, installed as the PKG PSDKLOADR (EBOOT.BIN for the XMB, USRDIR/RELOAD.SELF for relaunch)
#   child/   ps3dk-run-child, the loader acceptance test child (links hook/)
# The host tool (host/) is a member of the tools/ Rust workspace; hook/ is linked into applications by
# tests/hardware/psgl-sample-boot/run-hook-inject.cmake.  See docs/tools/ps3dk-run.md.
#
#   cmake -S tools/ps3dk-run -B build/ps3dk-run -G Ninja -DCMAKE_TOOLCHAIN_FILE=$PS3DK/cmake/ps3-ppu-toolchain.cmake
#   cmake --build build/ps3dk-run          (or tools/ps3dk-run/build.sh)
cmake_minimum_required(VERSION 3.20)
project(ps3dk-run C CXX)

add_subdirectory(loader)
add_subdirectory(child)
