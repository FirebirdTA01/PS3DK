# ps3-prx-identity.cmake - argument helper for ps3_add_prx (ps3-self.cmake).
#
# _ps3_prx_identity_args(<out-var> <prefix>)
#   Builds the identity arguments for `prx-gen build' from <prefix>_NAME,
#   <prefix>_VERSION and <prefix>_ATTRIBUTES (as set by cmake_parse_arguments).
#   A value is forwarded whenever it was supplied and is not empty - including
#   strings CMake treats as false, such as OFF or 0 - so prx-gen can compare it
#   with the module's SYS_MODULE_INFO record and validate it.  Omitted values
#   are left to the record or to prx-gen's defaults.

function(_ps3_prx_identity_args out prefix)
    set(_args)
    foreach(_field NAME VERSION ATTRIBUTES)
        string(TOLOWER "${_field}" _flag)
        if(DEFINED ${prefix}_${_field} AND NOT "${${prefix}_${_field}}" STREQUAL "")
            list(APPEND _args "--${_flag}" "${${prefix}_${_field}}")
        endif()
    endforeach()
    set(${out} "${_args}" PARENT_SCOPE)
endfunction()
