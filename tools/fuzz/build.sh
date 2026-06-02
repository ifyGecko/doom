#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# Build the AFL++ fuzz target.
#
# Outputs:
#   build-fuzz/doom-engine-fuzz       coverage-instrumented engine
#   build-fuzz/shmpath_preload.so     path/write LD_PRELOAD shim
# -----------------------------------------------------------------------------
set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
AFL="${AFLPLUSPLUS:-$HOME/AFLplusplus}"
BUILD="$ROOT/build-fuzz"

if [[ ! -x "$AFL/afl-clang-fast" ]]; then
  echo "error: afl-clang-fast not found at $AFL/afl-clang-fast" >&2
  echo "       set AFLPLUSPLUS=/path/to/AFLplusplus" >&2
  exit 1
fi

# Engine fuzz target ----------------------------------------------------------
if [[ ! -f "$BUILD/build.ninja" ]]; then
  echo "==> meson setup build-fuzz"
  # If the dir exists from a previous half-failed run, wipe it first - meson
  # refuses to reconfigure an existing-but-unconfigured directory.
  if [[ -d "$BUILD" ]]; then rm -rf "$BUILD"; fi
  CC="$AFL/afl-clang-fast" meson setup "$BUILD" -Dfuzzing=true
fi

echo "==> ninja"
ninja -C "$BUILD" doom-engine-fuzz

# The shim must be built with plain cc, not afl-clang-fast: instrumented
# shared libraries reference __afl_area_ptr from the AFL runtime, which
# only resolves inside the fuzz target itself. LD_PRELOAD'ing such a lib
# into, e.g. /bin/sh, fails with "undefined symbol __afl_area_ptr".
echo "==> compiling shmpath_preload.so (with plain cc)"
cc -shared -fPIC -O2 -g \
   -o "$BUILD/shmpath_preload.so" \
   "$ROOT/tools/fuzz/shmpath_preload.c" \
   -ldl

echo
echo "Built:"
echo "  $BUILD/doom-engine-fuzz"
echo "  $BUILD/shmpath_preload.so"
