# SPDX-License-Identifier: BSD-3-Clause
add_executable(crget crget.c)
target_link_libraries(crget PRIVATE crhttp::crhttp)

add_executable(crhttp_parallel parallel.cpp)
target_compile_features(crhttp_parallel PRIVATE cxx_std_17)
target_link_libraries(crhttp_parallel PRIVATE crhttp::crhttp)
