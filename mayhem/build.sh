#!/usr/bin/env bash
#
# mayhem/build.sh — build EternalTerminal's fuzz harnesses (sanitized, DWARF 3) and its Catch2
# test runner (normal flags). Runs in the commit image as `mayhem` in /mayhem; offline-safe: every
# dependency is an apt -dev package (installed by the Dockerfile) or a checked-in git submodule.
set -euo pipefail

[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH

: "${SANITIZER_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer}"
: "${DEBUG_FLAGS:=-g -gdwarf-3}"
: "${CC:=clang}" ; : "${CXX:=clang++}" ; : "${LIB_FUZZING_ENGINE:=-fsanitize=fuzzer}"
: "${MAYHEM_JOBS:=$(nproc)}"
: "${COVERAGE_FLAGS=}"
export SANITIZER_FLAGS DEBUG_FLAGS CC CXX LIB_FUZZING_ENGINE MAYHEM_JOBS COVERAGE_FLAGS

cd "$SRC"

# Upstream CMake switches (no vcpkg, no sentry/telemetry: all deps are system packages or submodules).
CMAKE_COMMON=(-G Ninja -DDISABLE_VCPKG=ON -DDISABLE_SENTRY=ON -DDISABLE_TELEMETRY=ON
              -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX")

# Always configure from scratch: a cache written under another PATH/compiler spelling (e.g. the
# ccache pre-fill) makes cmake discard it mid-configure and lose the vcpkg-off switch.
rm -rf build testbuild

# 1) The project libraries, sanitized + DWARF 3. fuzzer-no-link adds libFuzzer coverage counters
#    to the project code so the harness sees edges inside it.
COV_FLAGS="-fsanitize=fuzzer-no-link"
cmake -S . -B build "${CMAKE_COMMON[@]}" -DBUILD_TESTING=OFF \
  -DCMAKE_C_FLAGS="$SANITIZER_FLAGS $COV_FLAGS $DEBUG_FLAGS" \
  -DCMAKE_CXX_FLAGS="$SANITIZER_FLAGS $COV_FLAGS $DEBUG_FLAGS"
cmake --build build --target et-lib TerminalCommon -j"$MAYHEM_JOBS"

# 2) Harnesses: each linked once against libFuzzer and once against the standalone driver.
INCLUDES=(-I"$SRC/external/simpleini" -I"$SRC/external/json/include" -I"$SRC/external/cxxopts/include"
          -I"$SRC/src/base" -I"$SRC/src/terminal" -I"$SRC/src/terminal/forwarding" -I"$SRC/src/htm"
          -I"$SRC/build" -I"$SRC"
          -isystem "$SRC/external/cpp-httplib" -isystem "$SRC/external/easyloggingpp/src"
          -isystem "$SRC/external/PlatformFolders" -isystem "$SRC/external/sole"
          -isystem "$SRC/external/base64" -isystem "$SRC/external/UniversalStacktrace/ust")
DEFS=(-DET_VERSION=\"7.1.1\" -DNO_TELEMETRY -DELPP_NO_DEFAULT_LOG_FILE -DELPP_FEATURE_CRASH_LOG
      -DELPP_THREAD_SAFE -DELPP_STRICT_PERMISSIONS -DWITH_UTEMPTER -DWITH_SELINUX)
LIBS=("$SRC/build/libTerminalCommon.a" "$SRC/build/libet-lib.a"
      -lprotobuf -lsodium -lselinux -lutempter
      -Wl,--push-state,--no-as-needed -lgcc_s -Wl,--pop-state
      -lssl -lcrypto -lz -lutil -lresolv -latomic -lpthread -Wl,-z,noexecstack)

OBJ="$SRC/build/mayhem-objs"
mkdir -p "$OBJ"
$CXX $SANITIZER_FLAGS $DEBUG_FLAGS -c "$SRC/mayhem/lsan_off.cc" -o "$OBJ/lsan_off.o"
$CC  $SANITIZER_FLAGS $DEBUG_FLAGS -c "$STANDALONE_FUZZ_MAIN" -o "$OBJ/standalone_main.o"

for h in TerminalServerFuzzer TerminalServerRouterFuzzer; do
  $CXX $SANITIZER_FLAGS $COV_FLAGS $DEBUG_FLAGS -std=gnu++20 "${DEFS[@]}" "${INCLUDES[@]}" \
      -c "$SRC/mayhem/harnesses/$h.cpp" -o "$OBJ/$h.o"
  $CXX $SANITIZER_FLAGS $DEBUG_FLAGS $LIB_FUZZING_ENGINE "$OBJ/$h.o" "$OBJ/lsan_off.o" \
      "${LIBS[@]}" -o "$SRC/$h"
  $CXX $SANITIZER_FLAGS $DEBUG_FLAGS "$OBJ/$h.o" "$OBJ/standalone_main.o" "$OBJ/lsan_off.o" \
      "${LIBS[@]}" -o "$SRC/$h-standalone"
done

# 3) The project's own Catch2 suite, built with NORMAL flags (no sanitizers) for mayhem/test.sh.
cmake -S . -B testbuild "${CMAKE_COMMON[@]}" -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS="$COVERAGE_FLAGS" -DCMAKE_CXX_FLAGS="$COVERAGE_FLAGS"
cmake --build testbuild --target et-test -j"$MAYHEM_JOBS"
