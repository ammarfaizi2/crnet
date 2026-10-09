# SPDX-License-Identifier: BSD-3-Clause
#
# Test suite. Everything runs against a local Python test server
# (tests/server/test_server.py: python3 + python3-h2 [+ python3-brotli,
# zstd]); no network access is needed.

find_package(GTest REQUIRED)
find_package(nlohmann_json 3 REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

include(GoogleTest)

add_executable(crhttp_tests
  common.cc
  test_async.cc
  test_basic.cc
  test_cpp.cc
  test_headers.cc
  test_http.cc
  test_proxy.cc)
target_compile_features(crhttp_tests PRIVATE cxx_std_17)
target_compile_options(crhttp_tests PRIVATE -Wall -Wextra)
target_compile_definitions(crhttp_tests PRIVATE
  CRHTTP_TEST_SERVER="${CMAKE_CURRENT_SOURCE_DIR}/server/test_server.py"
  CRHTTP_TEST_CHROMIUM_VERSION="${CRHTTP_CHECKOUT_CR}")
target_link_libraries(crhttp_tests PRIVATE
  crhttp::crhttp GTest::gtest GTest::gtest_main nlohmann_json::nlohmann_json)

# Each test runs in its own process with its own test server.
gtest_discover_tests(crhttp_tests
  DISCOVERY_MODE PRE_TEST
  PROPERTIES TIMEOUT 120)

add_executable(crhttp_c_api_smoke c_api_smoke.c)
set_target_properties(crhttp_c_api_smoke PROPERTIES C_STANDARD 99 C_EXTENSIONS OFF)
target_compile_options(crhttp_c_api_smoke PRIVATE -Wall -Wextra -pedantic -Werror)
target_link_libraries(crhttp_c_api_smoke PRIVATE crhttp::crhttp)
add_test(NAME c_api_smoke
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_c_smoke.sh ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/server/test_server.py
          $<TARGET_FILE:crhttp_c_api_smoke>)
