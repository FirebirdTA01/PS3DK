cmake_minimum_required(VERSION 3.20)
project(net_init_probe C)
include(ps3-self)
# A measurement probe for libnet's shared-network holders (main.c); no GPU, no profiler.  Build it against an SDK
# whose libnet exports the raw finalize as sys_net_finalize_network (v0.27.x).
add_executable(net-init-probe main.c)
target_compile_features(net-init-probe PRIVATE c_std_11)
target_compile_options(net-init-probe PRIVATE -O2 -Wall -Wextra -Werror)
target_link_libraries(net-init-probe PRIVATE net sysmodule_stub rt lv2)
# The run-hook injection (tests/hardware/psgl-sample-boot/run-hook-inject.cmake) needs PS3DK_RUN_HOOK_DIR: the hooked
# build never tears the network or the NET module down (main.c).
if(PS3DK_RUN_HOOK_DIR)
  target_compile_definitions(net-init-probe PRIVATE NETPROBE_HOOKED=1)
endif()
ps3_add_self(net-init-probe)
