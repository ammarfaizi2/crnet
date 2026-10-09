# SPDX-License-Identifier: BSD-3-Clause
#
# crhttp top-level build.
#
# libcrhttp.so itself must be compiled by Chromium's toolchain (its
# clang, libc++, sysroot and GN build graph), so this project:
#   1. writes an args.gn and runs Chromium's `gn gen`,
#   2. runs Chromium's `ninja` to build //crhttp -> libcrhttp.so,
#   3. exposes the result as the imported target crhttp::crhttp,
#   4. lets consumers in this tree link it with the *system* compiler.
#
# Quick start:
#   scripts/fetch-chromium.sh            # once, ~30 GB
#   cmake -S . -B build -G Ninja
#   cmake --build build
#
cmake_minimum_required(VERSION 3.16)
project(crhttp VERSION 0.1.0 LANGUAGES C CXX)

include(GNUInstallDirs)

set(CRHTTP_CHROMIUM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/chromium" CACHE PATH
    "Directory holding depot_tools/ and src/ (see scripts/fetch-chromium.sh)")
set(CRHTTP_GN_OUT "out/crhttp" CACHE STRING
    "GN output directory, relative to the Chromium src/ directory")
set(CRHTTP_GN_EXTRA_ARGS "" CACHE STRING
    "Extra GN args appended to args.gn (newline or space separated)")
option(CRHTTP_CHROMIUM_DEBUG "Build Chromium/crhttp with is_debug=true" OFF)
option(CRHTTP_ASAN "Build Chromium/crhttp with AddressSanitizer" OFF)
set(CRHTTP_NINJA_JOBS "" CACHE STRING "ninja -j for the Chromium build (empty = ninja default)")

set(CR_SRC "${CRHTTP_CHROMIUM_DIR}/src")
set(CR_OUT "${CR_SRC}/${CRHTTP_GN_OUT}")
set(CR_GN "${CR_SRC}/buildtools/linux64/gn")
set(CR_NINJA "${CR_SRC}/third_party/ninja/ninja")

if(NOT EXISTS "${CR_SRC}/chrome/VERSION")
  message(FATAL_ERROR
    "No Chromium checkout at ${CR_SRC}.\n"
    "Run scripts/fetch-chromium.sh first, or set CRHTTP_CHROMIUM_DIR.")
endif()

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/chromium/VERSION" CRHTTP_PINNED_CR)
file(STRINGS "${CR_SRC}/chrome/VERSION" _cr_ver_lines)
set(_cr_ver "")
foreach(_l IN LISTS _cr_ver_lines)
  string(REGEX REPLACE "^[A-Z]+=" "" _v "${_l}")
  list(APPEND _cr_ver "${_v}")
endforeach()
list(JOIN _cr_ver "." CRHTTP_CHECKOUT_CR)
if(NOT CRHTTP_CHECKOUT_CR STREQUAL CRHTTP_PINNED_CR)
  message(WARNING
    "Chromium checkout is ${CRHTTP_CHECKOUT_CR} but chromium/VERSION pins "
    "${CRHTTP_PINNED_CR}; re-run scripts/fetch-chromium.sh.")
endif()
message(STATUS "crhttp: Chromium ${CRHTTP_CHECKOUT_CR} at ${CR_SRC}")

# //crhttp must point at our src/.
if(NOT EXISTS "${CR_SRC}/crhttp/BUILD.gn")
  file(CREATE_LINK "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CR_SRC}/crhttp" SYMBOLIC)
endif()

# ---------------------------------------------------------------------------
# GN configuration
# ---------------------------------------------------------------------------
if(CRHTTP_CHROMIUM_DEBUG)
  set(CRHTTP_GN_IS_DEBUG true)
else()
  set(CRHTTP_GN_IS_DEBUG false)
endif()
if(CRHTTP_ASAN)
  set(CRHTTP_GN_IS_ASAN true)
else()
  set(CRHTTP_GN_IS_ASAN false)
endif()
string(REPLACE ";" "\n" CRHTTP_GN_EXTRA_ARGS_NL "${CRHTTP_GN_EXTRA_ARGS}")
set(CRHTTP_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")

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
if(NOT _new_args STREQUAL _old_args OR NOT EXISTS "${CR_OUT}/build.ninja")
  file(WRITE "${CR_OUT}/args.gn" "${_new_args}")
  message(STATUS "crhttp: gn gen ${CR_OUT}")
  execute_process(COMMAND "${CR_GN}" gen "${CR_OUT}"
                  WORKING_DIRECTORY "${CR_SRC}"
                  RESULT_VARIABLE _gn_rc)
  if(NOT _gn_rc EQUAL 0)
    message(FATAL_ERROR "gn gen failed")
  endif()
endif()

# ---------------------------------------------------------------------------
# libcrhttp.so (built by Chromium's ninja; always invoked, it is a no-op
# when up to date and tracks its own dependencies precisely)
# ---------------------------------------------------------------------------
set(CRHTTP_SOVERSION 0)
set(CRHTTP_LIB_DIR "${CMAKE_CURRENT_BINARY_DIR}/lib")
set(CRHTTP_LIB_REAL "${CRHTTP_LIB_DIR}/libcrhttp.so.${PROJECT_VERSION}")
set(CRHTTP_LIB_SONAME "${CRHTTP_LIB_DIR}/libcrhttp.so.${CRHTTP_SOVERSION}")
set(CRHTTP_LIB_LINK "${CRHTTP_LIB_DIR}/libcrhttp.so")

set(_ninja_jobs "")
if(CRHTTP_NINJA_JOBS)
  set(_ninja_jobs -j ${CRHTTP_NINJA_JOBS})
endif()

add_custom_target(crhttp_chromium_build ALL
  COMMAND "${CR_NINJA}" -C "${CR_OUT}" ${_ninja_jobs} crhttp:crhttp
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${CRHTTP_LIB_DIR}"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${CR_OUT}/libcrhttp.so" "${CRHTTP_LIB_REAL}"
  COMMAND "${CMAKE_COMMAND}" -E create_symlink
          "libcrhttp.so.${PROJECT_VERSION}" "${CRHTTP_LIB_SONAME}"
  COMMAND "${CMAKE_COMMAND}" -E create_symlink
          "libcrhttp.so.${CRHTTP_SOVERSION}" "${CRHTTP_LIB_LINK}"
  BYPRODUCTS "${CRHTTP_LIB_REAL}" "${CRHTTP_LIB_SONAME}" "${CRHTTP_LIB_LINK}"
  WORKING_DIRECTORY "${CR_SRC}"
  USES_TERMINAL
  COMMENT "Building libcrhttp.so with the Chromium toolchain")

add_library(crhttp SHARED IMPORTED GLOBAL)
set_target_properties(crhttp PROPERTIES
  IMPORTED_LOCATION "${CRHTTP_LIB_REAL}"
  IMPORTED_SONAME "libcrhttp.so.${CRHTTP_SOVERSION}"
  INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/include")
add_dependencies(crhttp crhttp_chromium_build)
add_library(crhttp::crhttp ALIAS crhttp)

# Convenience: (re)fetch / update the Chromium checkout.
add_custom_target(chromium-fetch
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/fetch-chromium.sh" "${CRHTTP_CHROMIUM_DIR}"
  USES_TERMINAL
  COMMENT "Fetching Chromium ${CRHTTP_PINNED_CR}")

# ---------------------------------------------------------------------------
# Install: library, headers, pkg-config, CMake package
# ---------------------------------------------------------------------------
install(FILES "${CRHTTP_LIB_REAL}" DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(FILES "${CRHTTP_LIB_SONAME}" "${CRHTTP_LIB_LINK}"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY include/crhttp DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

configure_file(cmake/crhttp.pc.in "${CMAKE_CURRENT_BINARY_DIR}/crhttp.pc" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/crhttp.pc"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")

include(CMakePackageConfigHelpers)
configure_package_config_file(cmake/crhttpConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/crhttpConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/crhttp"
  PATH_VARS CMAKE_INSTALL_INCLUDEDIR CMAKE_INSTALL_LIBDIR)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/crhttpConfigVersion.cmake"
  COMPATIBILITY SameMinorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/crhttpConfig.cmake"
              "${CMAKE_CURRENT_BINARY_DIR}/crhttpConfigVersion.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/crhttp")
