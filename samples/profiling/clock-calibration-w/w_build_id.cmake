# W_BUILD_ID: the marker tag written into every record, exactly 16 lowercase hex digits (the recipe passes the first
# 16 hex digits of main.c's sha256).  A tag only: the artifact manifest (ELF/SELF/PKG/toolchain hashes) is the build
# identity.  Also run standalone by the host tests: cmake -DW_BUILD_ID=<id> -P w_build_id.cmake
string(LENGTH "${W_BUILD_ID}" _w_id_len)
if(NOT W_BUILD_ID MATCHES "^[0-9a-f]+$" OR NOT _w_id_len EQUAL 16)
  message(FATAL_ERROR "W_BUILD_ID must be exactly 16 lowercase hex digits (got '${W_BUILD_ID}')")
endif()
