# Checks the PSGL samples' private temporary-file helper; boots in RPCS3 or
# on hardware and prints PSGL_TMPFILE_CHECK_OK checks=N failures=0.
cmake_minimum_required(VERSION 3.20)
project(psgl_tmpfile_check C)

include(ps3-self)

add_executable(psgl-tmpfile-check main.c)
target_compile_features(psgl-tmpfile-check PRIVATE c_std_11)
target_compile_options(psgl-tmpfile-check PRIVATE -Wall -Wextra -Werror)
target_link_libraries(psgl-tmpfile-check PRIVATE sysutil m)
ps3_add_self(psgl-tmpfile-check)
