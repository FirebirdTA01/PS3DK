cmake_minimum_required(VERSION 3.20)
project(net_init_probe C)
include(ps3-self)
# A measurement probe for libnet's shared-network holders (main.c); no GPU, no profiler.  Build it against an SDK
# whose libnet exports the raw finalize as sys_net_finalize_network (v0.27.x).
add_executable(net-init-probe main.c)
target_compile_features(net-init-probe PRIVATE c_std_11)
target_compile_options(net-init-probe PRIVATE -O2 -Wall -Wextra -Werror)
target_link_libraries(net-init-probe PRIVATE net sysmodule_stub rt lv2)
ps3_add_self(net-init-probe)
