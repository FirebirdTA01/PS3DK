# ps3dk-loader: network SELF launcher (protocol 2: paired host key, authenticated frames; trusted LAN only).
# Installs as PKG PSDKLOADR: EBOOT.BIN starts it from the XMB; after each run the hook relaunches USRDIR/RELOAD.SELF
# (EBOOT.BIN only if RELOAD.SELF is missing).
cmake_minimum_required(VERSION 3.20)
project(ps3dk-loader C)

include(ps3-self)

add_executable(ps3dk-loader
    source/main.c
    source/session.c
    source/files.c
    source/state.c
    ../common/prn_net.c
    ../common/prn_sha256.c
    ../common/prn_hmac.c)
target_compile_features(ps3dk-loader PRIVATE c_std_11)
target_compile_options(ps3dk-loader PRIVATE -Wall -Wextra -Wno-unused-parameter)

target_link_libraries(ps3dk-loader PRIVATE
    dbgfont gcm_cmd gcm_sys io net netctl_stub sysmodule_stub sysutil rt lv2 m
)

ps3_add_self(ps3dk-loader)

# USRDIR/RELOAD.SELF: the loader again as a plain-signed SELF, which the run hook relaunches after a run (the NPDRM
# EBOOT.BIN is for the XMB).  Copied after ps3_add_self's own post-build steps have written ps3dk-loader.self.
set(_reload_overlay "${CMAKE_CURRENT_BINARY_DIR}/pkg_overlay")
file(MAKE_DIRECTORY "${_reload_overlay}/USRDIR")
add_custom_command(TARGET ps3dk-loader POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_BINARY_DIR}/ps3dk-loader.self"
            "${_reload_overlay}/USRDIR/RELOAD.SELF"
    VERBATIM)

ps3_add_pkg(ps3dk-loader
    TITLE     "PS3DK Loader"
    APPID     "PSDKLOADR"
    CONTENTID "UP0001-PSDKLOADR_00-0000000000000000"
    PKGFILES  "${_reload_overlay}")
