# SPDX-License-Identifier: BSD-3-Clause
add_executable(crget crget.c)
target_link_libraries(crget PRIVATE crnet::crnet)

add_executable(crnet_parallel parallel.cpp)
target_compile_features(crnet_parallel PRIVATE cxx_std_17)
target_link_libraries(crnet_parallel PRIVATE crnet::crnet)
add_executable(crws crws.c)
target_link_libraries(crws PRIVATE crnet::crnet)
