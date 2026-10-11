# Building and linking crnet

## How the build works

`libcrnet.so` is compiled *inside* a Chromium source tree, by Chromium's
own build system, because `//net` can only be built with Chromium's
toolchain: its pinned clang, its private libc++, its Debian sysroot and
its GN build graph. The crnet sources (`src/`) are linked into the tree
as `//crnet` and built as a GN `shared_library`.

A top-level CMake project hides this:

1. it renders `args.gn` from `cmake/args.gn.in` and runs Chromium's
   `gn gen` (only when the arguments change),
2. it runs Chromium's `ninja` for `//crnet:crnet` on every build (a
   no-op when nothing changed),
3. it lays out `build/lib/libcrnet.so.0.1.0` with the soname
   `libcrnet.so.0` and exposes it as the imported target `crnet::crnet`,
4. it builds the examples and tests with the **system** compiler
   against that library, exactly as an external project would.

The result is a single self-contained shared library with a plain C ABI.
Only `crnet_*` symbols are exported (linker version script
`src/crnet.map`); Chromium, BoringSSL, ICU data and libc++ are linked
in privately, so there are no symbol or C++ ABI clashes with the host
program.

## Prerequisites

- x86-64 Linux. Ubuntu 22.04 is what crnet is developed on; any distro
  supported by Chromium's `install-build-deps.sh` should work.
- About 35 GB of disk for the Chromium checkout plus about 5 GB for the
  build output.
- git, python3, CMake 3.16 or newer, and ninja or make for the CMake
  side.
- A C/C++ compiler for your own code and the tests (gcc or clang; any
  version that supports C99/C++17).
- For the tests: GoogleTest (`libgtest-dev`), nlohmann-json
  (`nlohmann-json3-dev`), OpenSSL headers (`libssl-dev`), `python3-h2`,
  and optionally `python3-brotli` and `zstd` for the content-encoding
  tests.

```bash
sudo apt install git python3 cmake ninja-build g++ \
    libgtest-dev nlohmann-json3-dev libssl-dev python3-h2 python3-brotli zstd
```

## 1. Fetch Chromium

```bash
CRNET_INSTALL_DEPS=1 scripts/fetch-chromium.sh
```

This clones depot_tools and does a shallow checkout of the release in
`chromium/VERSION` into `chromium/src`. It then syncs the dependencies
needed for a Linux build and runs the hooks, which download Chromium's
clang, sysroot, gn and ninja. Finally it links `src/` to
`chromium/src/crnet` (a relative link). The CMake build checks that
link before every build and re-points it if it names another crnet
tree, as it can after copying a checkout or sharing one Chromium
checkout between trees; it then recompiles crnet's own sources. `CRNET_INSTALL_DEPS=1` additionally runs Chromium's
`build/install-build-deps.sh` (it needs sudo); this is only required
once per machine.

The script is idempotent: run it again after changing
`chromium/VERSION`, or if a step was interrupted. The checkout location
can be changed by passing a directory (`scripts/fetch-chromium.sh
/data/chromium`) and setting `-DCRNET_CHROMIUM_DIR=/data/chromium` when
configuring CMake. `CRNET_JOBS` limits gclient's parallelism.

## 2. Build

```bash
cmake -S . -B build -G Ninja
cmake --build build
```

The first build compiles the parts of Chromium that `//net` needs (about
4,500 compile steps, roughly 90 CPU-minutes); on a 128-core machine that
takes under two minutes. Later builds only recompile what changed.

Outputs:

| Path | What |
|---|---|
| `build/lib/libcrnet.so.0.1.0` | the library (with debug info, ~125 MB; ~27 MB stripped) |
| `build/lib/libcrnet.so.0`, `libcrnet.so` | soname and development symlinks |
| `build/examples/crget` | curl-like CLI on the C API |
| `build/examples/crnet_parallel` | concurrency demo on the C++ wrapper |
| `build/tests/crnet_tests` | test suite (see [testing.md](testing.md)) |

### CMake options

| Option | Default | Meaning |
|---|---|---|
| `CRNET_CHROMIUM_DIR` | `<repo>/chromium` | directory containing `src/` and `depot_tools/` |
| `CRNET_GN_OUT` | `out/crnet` (`out/crnet-asan`, `out/crnet-tsan` with a sanitizer) | GN output directory, relative to `src/` |
| `CRNET_GN_EXTRA_ARGS` | empty | extra `args.gn` lines, e.g. `symbol_level=0` |
| `CRNET_CHROMIUM_DEBUG` | OFF | build Chromium and crnet with `is_debug=true` (DCHECKs on, slow) |
| `CRNET_ASAN` | OFF | build with AddressSanitizer; consumers must link the ASan runtime too |
| `CRNET_TSAN` | OFF | build with ThreadSanitizer, see below |
| `CRNET_NINJA_JOBS` | ninja default | parallelism of the Chromium build |
| `CRNET_BUILD_TESTS` | ON | build the test suite |
| `CRNET_BUILD_EXAMPLES` | ON | build the examples |

Build without debug info, for a smaller library:

```bash
cmake -S . -B build -G Ninja -DCRNET_GN_EXTRA_ARGS="symbol_level=0"
```

To use different debug and release configurations side by side, give
each its own `CRNET_GN_OUT` (e.g. `out/crnet-debug`).

### ThreadSanitizer

`CRNET_TSAN=ON` builds Chromium and crnet with ThreadSanitizer, so that
races between your threads and crnet's are reported with crnet's side
of the stack. The library then expects the TSan runtime from the
executable, which only clang provides, so build your code (and crnet's
tests) with clang and `-fsanitize=thread`; the `crnet::crnet` CMake
target adds that flag, and `-fsanitize-link-c++-runtime` for C
programs. Chromium's own clang will do:

```bash
CL=chromium/src/third_party/llvm-build/Release+Asserts/bin
cmake -S . -B build-tsan -G Ninja -DCRNET_TSAN=ON \
      -DCMAKE_C_COMPILER=$CL/clang -DCMAKE_CXX_COMPILER=$CL/clang++
cmake --build build-tsan
ctest --test-dir build-tsan
```

It rebuilds all of Chromium's network stack in its own output directory
(`out/crnet-tsan`). The test suite runs clean under it, without
suppressions; a race report makes a test fail.

### GN arguments crnet relies on

`cmake/args.gn.in` sets, among others:

- `use_allocator_shim = false`, `use_partition_alloc_as_malloc = false`:
  a library must never replace the host process' `malloc`.
- `icu_use_data_file = false`: ICU data is embedded, so no
  `icudtl.dat` has to be shipped next to the library.
- `is_component_build = false`: everything is linked into one `.so`.

## 3. Install

```bash
cmake --install build --prefix /usr/local
```

This installs:

```
/usr/local/lib/libcrnet.so.0.1.0, libcrnet.so.0, libcrnet.so
/usr/local/include/crnet/crnet.h, crnet.hpp
/usr/local/lib/pkgconfig/crnet.pc
/usr/local/lib/cmake/crnet/crnetConfig.cmake, crnetConfigVersion.cmake
```

Strip the installed library if you do not need debug info:
`strip --strip-unneeded /usr/local/lib/libcrnet.so.0.1.0`.

## Linking crnet into your project

The library is a regular shared object with a C ABI, so any of these
work.

**CMake** (after installing):

```cmake
find_package(crnet 0.1 REQUIRED)
target_link_libraries(myapp PRIVATE crnet::crnet)
```

**CMake** (crnet as a subdirectory of your project, building Chromium
as part of your build):

```cmake
add_subdirectory(third_party/crnet)
target_link_libraries(myapp PRIVATE crnet::crnet)
```

**pkg-config**:

```bash
cc -o myapp myapp.c $(pkg-config --cflags --libs crnet)
```

**Directly**, without installing:

```bash
cc -I crnet/include myapp.c -L crnet/build/lib -lcrnet \
   -Wl,-rpath,'$ORIGIN/../lib'
```

C programs include `<crnet/crnet.h>`; C++ programs can additionally use
`<crnet/crnet.hpp>` (C++17). Your code may use any C++ standard library
(libstdc++ or libc++, any version): nothing C++ crosses the library
boundary.

## Deployment

At run time `libcrnet.so.0` needs, besides glibc, the system's NSS and
GLib libraries (Chromium's Linux certificate and platform layers):
`libnss3`, `libnssutil3`, `libnspr4`, `libplc4`, `libplds4` and
`libglib-2.0`. On Debian/Ubuntu: `apt install libnss3 libglib2.0-0`.
Nothing else is needed: no ICU data file, no Chromium resources.

The library honours these environment variables:

| Variable | Effect |
|---|---|
| `CRNET_LOG_LEVEL` | Chromium log level on stderr: 0 info, 1 warning, 2 error, 3 fatal only (default) |
| `CRNET_ENABLE_FEATURES` | Chromium features to force on (comma separated), like `--enable-features` |
| `CRNET_DISABLE_FEATURES` | Chromium features to force off |

The feature variables are read once, when the first client is created.
`crnet_global_init()` sets the same thing programmatically.
