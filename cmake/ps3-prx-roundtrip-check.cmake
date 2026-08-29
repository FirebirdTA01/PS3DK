# Compare two canonicalised export tables and explain the failure.
#
# Run via `cmake -P`. `cmake -E compare_files` would do the comparison, but it
# exits non-zero with no output, which leaves whoever hit it staring at a wall
# of quoted command line. This says what disagreed and what that means.
#
# Inputs (-D):
#   PS3_RT_SOURCE  canonical form of the module's exports.yml
#   PS3_RT_MODULE  canonical form of what the built module actually exports
#   PS3_RT_TARGET  target name, for the message

foreach(_v PS3_RT_SOURCE PS3_RT_MODULE)
    if(NOT DEFINED ${_v})
        message(FATAL_ERROR "ps3-prx-roundtrip-check: ${_v} not set")
    endif()
    if(NOT EXISTS "${${_v}}")
        message(FATAL_ERROR "ps3-prx-roundtrip-check: ${${_v}} does not exist")
    endif()
endforeach()

file(READ "${PS3_RT_SOURCE}" _src)
file(READ "${PS3_RT_MODULE}" _mod)

if(_src STREQUAL _mod)
    return()
endif()

message("")
message("Export round trip FAILED for ${PS3_RT_TARGET}.")
message("")
message("The module's own export table does not describe the same library as the")
message("exports.yml it was built from. Anything linking against a stub archive")
message("generated from that YAML would fail to resolve at load time.")
message("")
message("Both sides below are the canonical form (nidgen entgen output), so only")
message("differences that matter to a linker appear -- library name, export names,")
message("NIDs, and their order.")
message("")
message("--- from exports.yml (${PS3_RT_SOURCE})")
message("${_src}")
message("--- from the built module (${PS3_RT_MODULE})")
message("${_mod}")
message("Usual causes: exports.yml edited without rebuilding, a NID changed in one")
message("place only, or an export added to the module's .S but not to the YAML.")
message(FATAL_ERROR "export round trip mismatch")
