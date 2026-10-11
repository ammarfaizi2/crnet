# SPDX-License-Identifier: BSD-3-Clause
#
# crnet top-level build.
#
# libcrnet.so itself must be compiled by Chromium's toolchain (its
# clang, libc++, sysroot and GN build graph), so this project:
#   1. writes an args.gn and runs Chromium's `gn gen`,
#   2. runs Chromium's `ninja` to build //crnet -> libcrnet.so,
#   3. exposes the result as the imported target crnet::crnet,
#   4. builds tests and examples with the *system* compiler against it,
#      which is exactly how third-party consumers link it.
#
# Quick start:
#   scripts/fetch-chromium.sh            # once, ~30 GB
#   cmake -S . -B build -G Ninja
#   cmake --build build
#   ctest --test-dir build
#
cmake_minimum_required(VERSION 3.16)
# The version lives in crnet.h only.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/crnet/crnet.h" _crnet_ver
     REGEX "^#define CRNET_VERSION_(MAJOR|MINOR|PATCH) [0-9]+$")
string(REGEX REPLACE ".*MAJOR ([0-9]+).*MINOR ([0-9]+).*PATCH ([0-9]+).*"
       "\\1.\\2.\\3" _crnet_ver "${_crnet_ver}")
project(crnet VERSION ${_crnet_ver} LANGUAGES C CXX)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             "${CMAKE_CURRENT_SOURCE_DIR}/include/crnet/crnet.h")

include(GNUInstallDirs)

set(CRNET_CHROMIUM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/chromium" CACHE PATH
    "Directory holding depot_tools/ and src/ (see scripts/fetch-chromium.sh)")
option(CRNET_ASAN "Build Chromium/crnet with AddressSanitizer" OFF)
option(CRNET_TSAN "Build Chromium/crnet with ThreadSanitizer" OFF)
if(CRNET_ASAN AND CRNET_TSAN)
  message(FATAL_ERROR "CRNET_ASAN and CRNET_TSAN cannot be combined.")
endif()
# A sanitizer rebuilds all of Chromium; give it its own output directory
# so that switching does not invalidate the regular build.
if(CRNET_TSAN)
  set(_crnet_default_out "out/crnet-tsan")
elseif(CRNET_ASAN)
  set(_crnet_default_out "out/crnet-asan")
else()
  set(_crnet_default_out "out/crnet")
endif()
set(CRNET_GN_OUT "${_crnet_default_out}" CACHE STRING
    "GN output directory, relative to the Chromium src/ directory")
set(CRNET_GN_EXTRA_ARGS "" CACHE STRING
    "Extra GN args appended to args.gn (newline or space separated)")
option(CRNET_CHROMIUM_DEBUG "Build Chromium/crnet with is_debug=true" OFF)
option(CRNET_BUILD_TESTS "Build the test suite" ON)
option(CRNET_BUILD_EXAMPLES "Build the examples" ON)
set(CRNET_NINJA_JOBS "" CACHE STRING "ninja -j for the Chromium build (empty = ninja default)")

set(CR_SRC "${CRNET_CHROMIUM_DIR}/src")
set(CR_OUT "${CR_SRC}/${CRNET_GN_OUT}")
set(CR_GN "${CR_SRC}/buildtools/linux64/gn")
set(CR_NINJA "${CR_SRC}/third_party/ninja/ninja")

if(NOT EXISTS "${CR_SRC}/chrome/VERSION")
  message(FATAL_ERROR
    "No Chromium checkout at ${CR_SRC}.\n"
    "Run scripts/fetch-chromium.sh first, or set CRNET_CHROMIUM_DIR.")
endif()

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/chromium/VERSION" CRNET_PINNED_CR)
file(STRINGS "${CR_SRC}/chrome/VERSION" _cr_ver_lines)
set(_cr_ver "")
foreach(_l IN LISTS _cr_ver_lines)
  string(REGEX REPLACE "^[A-Z]+=" "" _v "${_l}")
  list(APPEND _cr_ver "${_v}")
endforeach()
list(JOIN _cr_ver "." CRNET_CHECKOUT_CR)
if(NOT CRNET_CHECKOUT_CR STREQUAL CRNET_PINNED_CR)
  message(WARNING
    "Chromium checkout is ${CRNET_CHECKOUT_CR} but chromium/VERSION pins "
    "${CRNET_PINNED_CR}; re-run scripts/fetch-chromium.sh.")
endif()
message(STATUS "crnet: Chromium ${CRNET_CHECKOUT_CR} at ${CR_SRC}")

# crnet relies on its patches to Chromium (chromium/patches/, applied by
# scripts/fetch-chromium.sh); refuse to build a tree without them, which
# would compile but behave differently.
find_package(Git REQUIRED)
file(GLOB _crnet_patches "${CMAKE_CURRENT_SOURCE_DIR}/chromium/patches/*.patch")
foreach(_p IN LISTS _crnet_patches)
  execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${CR_SRC}" apply --reverse
                          --check "${_p}"
                  RESULT_VARIABLE _rc OUTPUT_QUIET ERROR_QUIET)
  if(NOT _rc EQUAL 0)
    get_filename_component(_name "${_p}" NAME)
    message(FATAL_ERROR
      "Chromium patch ${_name} is not applied to ${CR_SRC}.\n"
      "Run: scripts/apply-patches.sh ${CR_SRC}")
  endif()
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_crnet_patches})

# //crnet must point at our src/; checked again before every build.
set(CRNET_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/link-src.cmake")

# ---------------------------------------------------------------------------
# GN configuration
# ---------------------------------------------------------------------------
if(CRNET_CHROMIUM_DEBUG)
  set(CRNET_GN_IS_DEBUG true)
else()
  set(CRNET_GN_IS_DEBUG false)
endif()
if(CRNET_ASAN)
  set(CRNET_GN_IS_ASAN true)
else()
  set(CRNET_GN_IS_ASAN false)
endif()
if(CRNET_TSAN)
  set(CRNET_GN_IS_TSAN true)
  # libcrnet.so then calls into the TSan runtime, which only an
  # executable built by clang with -fsanitize=thread provides. GCC's
  # libtsan is a different runtime.
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR
     NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR
      "CRNET_TSAN needs clang for the code linking libcrnet.so, e.g.\n"
      "  -DCMAKE_C_COMPILER=${CR_SRC}/third_party/llvm-build/Release+Asserts/bin/clang\n"
      "  -DCMAKE_CXX_COMPILER=${CR_SRC}/third_party/llvm-build/Release+Asserts/bin/clang++")
  endif()
else()
  set(CRNET_GN_IS_TSAN false)
endif()
string(REPLACE ";" "\n" CRNET_GN_EXTRA_ARGS_NL "${CRNET_GN_EXTRA_ARGS}")
set(CRNET_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")

file(MAKE_DIRECTORY "${CR_OUT}")
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/args.gn.in"
               "${CMAKE_CURRENT_BINARY_DIR}/args.gn" @ONLY)
# Only touch args.gn when it changes, or ninja would regenerate the
# whole Chromium build graph on every configure.
file(READ "${CMAKE_CURRENT_BINARY_DIR}/args.gn" _new_args)
set(_old_args "")
if(EXISTS "${CR_OUT}/args.gn")
  file(READ "${CR_OUT}/args.gn" _old_args)
endif()
if(NOT _new_args STREQUAL _old_args OR NOT EXISTS "${CR_OUT}/build.ninja"
   OR CRNET_SRC_RELINKED)
  file(WRITE "${CR_OUT}/args.gn" "${_new_args}")
  message(STATUS "crnet: gn gen ${CR_OUT}")
  execute_process(COMMAND "${CR_GN}" gen "${CR_OUT}"
                  WORKING_DIRECTORY "${CR_SRC}"
                  RESULT_VARIABLE _gn_rc)
  if(NOT _gn_rc EQUAL 0)
    message(FATAL_ERROR "gn gen failed")
  endif()
endif()

# ---------------------------------------------------------------------------
# libcrnet.so (built by Chromium's ninja; always invoked, it is a no-op
# when up to date and tracks its own dependencies precisely)
# ---------------------------------------------------------------------------
set(CRNET_SOVERSION 0)
set(CRNET_LIB_DIR "${CMAKE_CURRENT_BINARY_DIR}/lib")
set(CRNET_LIB_REAL "${CRNET_LIB_DIR}/libcrnet.so.${PROJECT_VERSION}")
set(CRNET_LIB_SONAME "${CRNET_LIB_DIR}/libcrnet.so.${CRNET_SOVERSION}")
set(CRNET_LIB_LINK "${CRNET_LIB_DIR}/libcrnet.so")

set(_ninja_jobs "")
if(CRNET_NINJA_JOBS)
  set(_ninja_jobs -j ${CRNET_NINJA_JOBS})
endif()

add_custom_target(crnet_chromium_build ALL
  COMMAND "${CMAKE_COMMAND}" "-DCR_SRC=${CR_SRC}" "-DCR_OUT=${CR_OUT}"
          "-DCR_GN=${CR_GN}" "-DCRNET_SRC=${CRNET_SRC}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/link-src.cmake"
  COMMAND "${CR_NINJA}" -C "${CR_OUT}" ${_ninja_jobs} crnet:crnet
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${CRNET_LIB_DIR}"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${CR_OUT}/libcrnet.so" "${CRNET_LIB_REAL}"
  COMMAND "${CMAKE_COMMAND}" -E create_symlink
          "libcrnet.so.${PROJECT_VERSION}" "${CRNET_LIB_SONAME}"
  COMMAND "${CMAKE_COMMAND}" -E create_symlink
          "libcrnet.so.${CRNET_SOVERSION}" "${CRNET_LIB_LINK}"
  BYPRODUCTS "${CRNET_LIB_REAL}" "${CRNET_LIB_SONAME}" "${CRNET_LIB_LINK}"
  WORKING_DIRECTORY "${CR_SRC}"
  USES_TERMINAL
  COMMENT "Building libcrnet.so with the Chromium toolchain")

add_library(crnet SHARED IMPORTED GLOBAL)
set_target_properties(crnet PROPERTIES
  IMPORTED_LOCATION "${CRNET_LIB_REAL}"
  IMPORTED_SONAME "libcrnet.so.${CRNET_SOVERSION}"
  INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/include")
add_dependencies(crnet crnet_chromium_build)
if(CRNET_TSAN)
  # Code using an instrumented library is instrumented too and links
  # the runtime, including its C++ part (operator new and delete),
  # which the C driver leaves out by default.
  set_property(TARGET crnet APPEND PROPERTY
               INTERFACE_COMPILE_OPTIONS -fsanitize=thread)
  set_property(TARGET crnet APPEND PROPERTY
               INTERFACE_LINK_OPTIONS -fsanitize=thread
               -fsanitize-link-c++-runtime)
endif()
add_library(crnet::crnet ALIAS crnet)

# Convenience: (re)fetch / update the Chromium checkout.
add_custom_target(chromium-fetch
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/fetch-chromium.sh" "${CRNET_CHROMIUM_DIR}"
  USES_TERMINAL
  COMMENT "Fetching Chromium ${CRNET_PINNED_CR}")

# ---------------------------------------------------------------------------
# Tests and examples (system toolchain)
# ---------------------------------------------------------------------------
if(CRNET_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(CRNET_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Install: library, headers, pkg-config, CMake package
# ---------------------------------------------------------------------------
install(FILES "${CRNET_LIB_REAL}" DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(FILES "${CRNET_LIB_SONAME}" "${CRNET_LIB_LINK}"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY include/crnet DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

# Relocatable: the prefix is derived from where the .pc file ends up, so
# `cmake --install --prefix ...` works without reconfiguring.
file(RELATIVE_PATH CRNET_PC_TO_PREFIX
     "/prefix/${CMAKE_INSTALL_LIBDIR}/pkgconfig" "/prefix")
string(REGEX REPLACE "/$" "" CRNET_PC_TO_PREFIX "${CRNET_PC_TO_PREFIX}")
configure_file(cmake/crnet.pc.in "${CMAKE_CURRENT_BINARY_DIR}/crnet.pc" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/crnet.pc"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")

include(CMakePackageConfigHelpers)
configure_package_config_file(cmake/crnetConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/crnetConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/crnet"
  PATH_VARS CMAKE_INSTALL_INCLUDEDIR CMAKE_INSTALL_LIBDIR)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/crnetConfigVersion.cmake"
  COMPATIBILITY SameMinorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/crnetConfig.cmake"
              "${CMAKE_CURRENT_BINARY_DIR}/crnetConfigVersion.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/crnet")
