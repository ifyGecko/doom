#!/usr/bin/env bash
# -----------------------------------------------------------------------------
# Launch afl-fuzz against build-fuzz/doom-engine-fuzz with the LD_PRELOAD
# shim required by this harness.
#
# Expected layout:
#   $ROOT/build-fuzz/doom-engine-fuzz
#   $ROOT/build-fuzz/shmpath_preload.so
#   $ROOT/fuzz/in/                                  (seed corpus)
#   $ROOT/fuzz/out/                                 (created by afl-fuzz)
#   $HOME/AFLplusplus/afl-fuzz
#
# Build it first:
#   ./tools/fuzz/build.sh        # builds the engine + the path shim
#   tools/fuzz/make_seed.py /home/user/Downloads/miniwad.wad fuzz/in
# -----------------------------------------------------------------------------
set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
AFL="${AFLPLUSPLUS:-$HOME/AFLplusplus}"
BUILD="$ROOT/build-fuzz"
SHMPATH="$BUILD/shmpath_preload.so"

if [[ ! -x "$AFL/afl-fuzz"           ]]; then echo "no $AFL/afl-fuzz"; exit 1; fi
if [[ ! -f "$SHMPATH"                ]]; then echo "no $SHMPATH; run: ninja -C $BUILD"; exit 1; fi
if [[ ! -x "$BUILD/doom-engine-fuzz" ]]; then echo "no $BUILD/doom-engine-fuzz"; exit 1; fi
if [[ ! -d "$ROOT/fuzz/in"           ]]; then echo "no $ROOT/fuzz/in; run make_seed.py first"; exit 1; fi

mkdir -p "$ROOT/fuzz/out"

# __wrap_I_Supervise (in harness_wraps.c) bypasses the supervisor and
# wires the engine's client_fd to fd 0 directly. shmpath_preload.so then:
#   - rewrites /tmp/doom-net-* to /dev/shm/doom-net-* (RAM-only iterations)
#   - discards writes to fd 0 (the engine's HELLO_ACK / WAD_ACK / MSG_READY
#     writes are bit-bucketed so reads on the same fd keep delivering AFL's
#     input until exhaustion)
#   - translates recv/recvfrom on fd 0 to read (fd 0 is a pipe, not a socket)
#
# AFL flags:
#   -t 5000   : per-iteration timeout (ms). Conservative; the cold-start
#               WAD upload + R_Init is the slow phase.
#   -m none   : no memory cap (engine zone allocator wants several MB).
#   -x dict   : token dictionary biased toward wire/WAD/argv/cfg surface.

export AFL_PRELOAD="$SHMPATH"
export AFL_SKIP_CPUFREQ=1
export AFL_AUTORESUME=1

exec "$AFL/afl-fuzz" \
  -i "$ROOT/fuzz/in" \
  -o "$ROOT/fuzz/out" \
  -x "$ROOT/tools/fuzz/doom.dict" \
  -t 5000 -m none \
  -- "$BUILD/doom-engine-fuzz"
