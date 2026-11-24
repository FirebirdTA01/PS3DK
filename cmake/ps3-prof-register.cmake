# Profiler opt-in options of the PPU toolchain files, and their project-include registration.  Included by
# ps3-ppu-toolchain.cmake and ps3-ppu-toolchain-lp64.cmake before project() runs; each module it selects runs from
# ps3-prof-project-include.cmake once project() has measured the actual pointer width.
#
# CMAKE_PROJECT_INCLUDE names one file only (ps3-prof-project-include.cmake), so these options configure on CMake
# 3.28 and older, where the variable takes a single file, in every combination they support.

# Wait observation is a separate library selection.
set(PS3_PROF_WAITS "OFF" CACHE STRING "Observed pthread waits: OFF, MANUAL, AUTO, EXT")
set_property(CACHE PS3_PROF_WAITS PROPERTY STRINGS OFF MANUAL AUTO EXT)
# Live viewer auto-start (opt-in): every executable that links the profiler starts the live agent with no app code.
option(PS3_PROF_LIVE_AUTOSTART "Profiler live viewer auto-start (opt-in)" OFF)
# Automatic RSX observation selects its own providers and init seam. Default builds do not include the selector and
# keep the existing library selection.
option(PS3_PROF_AUTO "Automatic RSX observation" OFF)

set(_ps3_prof_modules "")
if(NOT PS3_PROF_WAITS STREQUAL "OFF" AND NOT PS3_PROF_WAITS STREQUAL "")
    list(APPEND _ps3_prof_modules "${CMAKE_CURRENT_LIST_DIR}/ps3-prof-waits.cmake")
endif()
# Before the AUTO selector, so its deferred step runs first and the selector maps the live archive it adds.
if(PS3_PROF_LIVE_AUTOSTART)
    list(APPEND _ps3_prof_modules "${CMAKE_CURRENT_LIST_DIR}/ps3-prof-live.cmake")
endif()
if(PS3_PROF_AUTO)
    list(APPEND _ps3_prof_modules "${CMAKE_CURRENT_LIST_DIR}/ps3-prof-auto.cmake")
endif()

if(_ps3_prof_modules)
    set(_ps3_prof_chain "${CMAKE_CURRENT_LIST_DIR}/ps3-prof-project-include.cmake")
    # The toolchain file is read more than once in one directory; the project's own entries are taken the first
    # time, before CMAKE_PROJECT_INCLUDE names the chain file.
    if(NOT CMAKE_PROJECT_INCLUDE STREQUAL _ps3_prof_chain)
        set(_ps3_prof_project_include_user "${CMAKE_PROJECT_INCLUDE}")
        list(REMOVE_ITEM _ps3_prof_project_include_user ${_ps3_prof_modules})
    endif()
    set(PS3_PROF_PROJECT_INCLUDE_CHAIN ${_ps3_prof_project_include_user} ${_ps3_prof_modules})
    set(CMAKE_PROJECT_INCLUDE "${_ps3_prof_chain}")
    unset(_ps3_prof_chain)
endif()
unset(_ps3_prof_modules)
