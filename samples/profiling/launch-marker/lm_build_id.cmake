# LM_BUILD_ID: the marker tag written into every record, exactly 16 lowercase hex digits (the recipe passes the first
# 16 hex digits of main.c's sha256).  A tag only: the artifact manifest (ELF/SELF/PKG/toolchain hashes) is the build
# identity.  Also run standalone by the host tests: cmake -DLM_BUILD_ID=<id> -P lm_build_id.cmake
string(LENGTH "${LM_BUILD_ID}" _lm_id_len)
if(NOT LM_BUILD_ID MATCHES "^[0-9a-f]+$" OR NOT _lm_id_len EQUAL 16)
  message(FATAL_ERROR "LM_BUILD_ID must be exactly 16 lowercase hex digits (got '${LM_BUILD_ID}')")
endif()
