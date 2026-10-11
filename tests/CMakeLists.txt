# SPDX-License-Identifier: BSD-3-Clause
#
# Test suite. Everything runs against a local Python test server
# (tests/server/test_server.py: python3 + python3-h2 [+ python3-brotli,
# zstd]); no network access is needed.

find_package(GTest REQUIRED)
find_package(nlohmann_json 3 REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(OpenSSL REQUIRED)

include(GoogleTest)

add_executable(crnet_tests
  common.cc
  test_async.cc
  test_basic.cc
  test_cache.cc
  test_cookies.cc
  test_cpp.cc
  test_dns.cc
  test_headers.cc
  test_http.cc
  test_proxy.cc
  test_stream.cc
  test_tls.cc
  test_websocket.cc)
target_compile_features(crnet_tests PRIVATE cxx_std_17)
target_compile_options(crnet_tests PRIVATE -Wall -Wextra)
target_compile_definitions(crnet_tests PRIVATE
  CRNET_TEST_SERVER="${CMAKE_CURRENT_SOURCE_DIR}/server/test_server.py"
  CRNET_TEST_CHROMIUM_VERSION="${CRNET_CHECKOUT_CR}")
target_link_libraries(crnet_tests PRIVATE
  crnet::crnet GTest::gtest GTest::gtest_main nlohmann_json::nlohmann_json
  OpenSSL::Crypto)

# Each test runs in its own process with its own test server.
gtest_discover_tests(crnet_tests
  DISCOVERY_MODE PRE_TEST
  PROPERTIES TIMEOUT 120)

add_executable(crnet_c_api_smoke c_api_smoke.c)
set_target_properties(crnet_c_api_smoke PROPERTIES C_STANDARD 99 C_EXTENSIONS OFF)
target_compile_options(crnet_c_api_smoke PRIVATE -Wall -Wextra -pedantic -Werror)
target_link_libraries(crnet_c_api_smoke PRIVATE crnet::crnet)
add_test(NAME c_api_smoke
  COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_c_smoke.sh ${Python3_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/server/test_server.py
          $<TARGET_FILE:crnet_c_api_smoke>)
