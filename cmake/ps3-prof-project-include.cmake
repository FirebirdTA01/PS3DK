# The one CMAKE_PROJECT_INCLUDE file of the PPU toolchain files (registered by ps3-prof-register.cmake).
#
# CMAKE_PROJECT_INCLUDE takes a list only from CMake 3.29; older CMake stops at configure when it names two files.
# The toolchain files therefore name this file alone, and it includes, in order, whatever the project already had in
# CMAKE_PROJECT_INCLUDE (a -D on the command line, for example) and then the profiler modules the options select:
# ps3-prof-waits.cmake, ps3-prof-live.cmake, ps3-prof-auto.cmake.  The live module is included before the AUTO
# selector, so its deferred step runs first and the selector maps the live archive it adds.
foreach(_ps3_prof_project_include IN LISTS PS3_PROF_PROJECT_INCLUDE_CHAIN)
    include("${_ps3_prof_project_include}")
endforeach()
unset(_ps3_prof_project_include)
