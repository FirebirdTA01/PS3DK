# Native host tests for the portable DS3 bench logic. They include the SDK's
# cell/pad.h for its constants and structs only; no PS3 code runs here.
cmake_minimum_required(VERSION 3.20)
project(ds3-bench-host-tests CXX)
enable_testing()
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(repo "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
set(sample "${CMAKE_CURRENT_LIST_DIR}/../source")
add_library(ds3b-logic STATIC "${sample}/pad_lifecycle.cpp" "${sample}/ds3_bench.cpp")
# -idirafter: host system headers win; only cell/*.h comes from the SDK tree.
target_compile_options(ds3b-logic PUBLIC -Wall -Wextra -Werror "-idirafter${repo}/sdk/include")
foreach(t truncated no_new_data failed_actuator failed_restore disconnect held_buttons exit
          capability deadlines reporter switch_cleanup)
    add_executable(test_${t} test_${t}.cpp)
    target_link_libraries(test_${t} PRIVATE ds3b-logic)
    add_test(NAME ${t} COMMAND test_${t})
endforeach()
