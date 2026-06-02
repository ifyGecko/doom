# AFL++ harness for `doom-engine`

This directory builds a coverage-instrumented `doom-engine-fuzz` binary plus
one `LD_PRELOAD` shim so AFL++ can stream a "client connection's worth of
bytes" at the engine on stdin. Each iteration drives the engine through
`I_NetBootstrap` (HELLO, ARGS, CONFIG, WAD upload framing) and
`D_DoomMain`'s init phase (M_LoadDefaults, W_InitMultipleFiles, R_Init,
P_Init, M_Init, HU_Init, ST_Init), then exits when AFL's input pipe is
drained.

The engine source is **unmodified**. All wiring is done through:

| Layer                                | Mechanism                                  |
|--------------------------------------|--------------------------------------------|
| Bypass the TCP supervisor / forkserver | `-Wl,--wrap=I_Supervise` (jumps straight into `I_NetBootstrap` + `D_DoomMain` with fd 0) |
| Stop after init                      | AFL input exhausts → `recv` EOF → `I_StartTic` "client disconnected" → `I_Quit` → `__wrap_exit` |
| Defensive `D_DoomLoop` short-circuit | `-Wl,--wrap=D_DoomLoop` (dead in current build because the only caller is intra-TU; kept for resilience) |
| Skip modifiedgame banner prompt      | `-Wl,--wrap=getchar`                       |
| Run engine atexit handlers (cleanup) | `-Wl,--wrap=exit` → `__real_exit`          |
| Keep `/tmp` off-disk                 | `shmpath_preload.so` (rewrite `/tmp/doom-net-*` → `/dev/shm/doom-net-*`) |
| Make `recv()` work on a pipe         | `shmpath_preload.so` (`recv` and `recvfrom` on fd 0 → `read`) |
| Discard writes to the "client"       | `shmpath_preload.so` (`write`/`writev`/`send` on fd 0) |
| Hide `/tmp` reads from `access()`    | `shmpath_preload.so` (`access`/`stat`/`lstat`) |
| Coverage instrumentation             | `CC=$AFL/afl-clang-fast`                   |

## File map

| File | Role |
|---|---|
| `harness_wraps.c`        | The `__wrap_*` stubs. Linked into `doom-engine-fuzz`. |
| `shmpath_preload.c`      | Path-rewrite, write-discard, recv-as-read `LD_PRELOAD`. |
| `make_seed.py`           | Builds a binary seed corpus from a WAD (+ optional cfg). |
| `capture_default_cfg.sh` | Produces `defaults.cfg.bin` for use by `make_seed.py --cfg`. |
| `split_corpus.py`        | Interpretive extraction of corpus components (WAD, cfg, args, HELLO). Reports a verdict on whether doom-client replay is byte-faithful. |
| `replay_raw.py`          | Byte-faithful TCP replay of a corpus item against a running engine. Decodes engine replies on stderr. Always exact. |
| `doom.dict`              | AFL token dictionary (wire bytes, lump names, CLI flags, cfg keys). |
| `build.sh`               | One-shot build helper. |
| `run.sh`                 | Launches `afl-fuzz` with the right env. |

## Replaying a finding against the real engine

There are two reproduction paths, designed for different jobs. Choose the
right one based on the verdict in `report.txt` (or always pick the raw one
if you're unsure).

### 1. Byte-faithful raw replay — `replay_raw.py`

Sends the corpus bytes verbatim over TCP. Zero parsing on the send side,
so every fuzz-mutated wire field (oversize lengths, bad chunk offsets,
forbidden frame types during upload, etc.) hits the engine exactly as the
fuzzer found it. This is the canonical reproduction path for any fuzz
finding.

```bash
# (in one terminal) start the real engine supervisor
./build/doom-engine -port 6666

# (in another) replay the corpus byte-for-byte
python3 tools/fuzz/replay_raw.py fuzz/out/default/crashes/id:000000* \
    --host 127.0.0.1 --port 6666
```

The engine's reply stream is decoded on stderr (HELLO_ACK, WAD_ACK,
READY, LOG lines, ERROR, BYE_S, etc.) so you can see what it made of the
corpus. `--dump-rx FILE` also saves the raw reply bytes for diff'ing
between runs. Decoding the replies does **not** affect what gets sent;
TX remains byte-identical to the corpus.

### 2. Interpretive component extraction — `split_corpus.py`

Pulls the corpus apart into its components (WAD, default.cfg, args list,
decoded HELLO) for human inspection and for "loose" replay via the real
`doom-client`. This path is *lossy*: every length and ordering field gets
recomputed by the client when it re-sends, so corpora whose interesting
behavior comes from a mutated header field will NOT reproduce here.

`split_corpus.py` is up-front about this. Every run writes a verdict
line at the top of `report.txt`:

```
verdict: BYTE-FAITHFUL VIA doom-client REPLAY: YES|NO
         reason: ...
```

The verdict is computed by re-encoding the extracted components in
canonical client order and `memcmp`'ing against the original corpus. YES
means the doom-client replay path will reproduce the engine's exact
behavior; NO means use `replay_raw.py`.

Findings are graded:

| Severity   | Meaning |
|---|---|
| `CRITICAL` | Reproduction via doom-client will *not* match (size mismatch, forbidden frame during upload, malformed HELLO, etc.). |
| `WARNING`  | Reproduction probably differs in detail (e.g. WAD_DONE.total_size mismatch that the engine ignores). |
| `INFO`     | Informational only (unknown message types post-bootstrap, trailing bytes, stream-level resync artifacts). |

```bash
python3 tools/fuzz/split_corpus.py fuzz/out/default/crashes/id:000000* /tmp/repro
ls /tmp/repro
#   <wad_basename>  - reconstructed WAD bytes (gaps zero-filled)
#   default.cfg     - reconstructed config blob (if MSG_CONFIG was present)
#   args.txt        - MSG_ARGS strings, one per line
#   hello.txt       - decoded HELLO fields
#   report.txt      - verdict, graded findings, stream summary
#   replay.sh       - launches the engine and runs BOTH replays

bash /tmp/repro/replay.sh
```

The generated `replay.sh` always offers the raw replay first; if the
verdict was NO it comments out the doom-client invocation so you don't
mislead yourself into thinking the doom-client path will reproduce the
finding.

## Prerequisites

* AFL++ checked out at `$HOME/AFLplusplus` (override with `AFLPLUSPLUS=`).
* `meson`, `ninja`, `clang` available on `PATH`.
* `/home/user/Downloads/miniwad.wad` (the seed WAD).

## Build

```bash
cd /home/user/doom-port
./tools/fuzz/build.sh
```

That script:

1. `CC=$AFL/afl-clang-fast meson setup build-fuzz -Dfuzzing=true` (idempotent).
2. `ninja -C build-fuzz doom-engine-fuzz`.
3. Compiles `shmpath_preload.so` directly with plain `cc` (the shim
   must not be coverage-instrumented).

## Seed corpus

```bash
cd /home/user/doom-port
./tools/fuzz/capture_default_cfg.sh
mkdir -p fuzz/in
python3 tools/fuzz/make_seed.py \
    /home/user/Downloads/miniwad.wad \
    fuzz/in \
    --cfg tools/fuzz/defaults.cfg.bin
ls -l fuzz/in
```

You should see `seed_full.bin`, `seed_noargs.bin`, `seed_nocfg.bin`,
`seed_short.bin`, `seed_tiny.bin`.

## Run

```bash
./tools/fuzz/run.sh
```

That sets `AFL_PRELOAD=$PWD/build-fuzz/shmpath_preload.so` (plus the
usual `AFL_SKIP_CPUFREQ=1`, `AFL_AUTORESUME=1`) and execs
`afl-fuzz -i fuzz/in -o fuzz/out -x tools/fuzz/doom.dict -t 5000 -m none
-- build-fuzz/doom-engine-fuzz`.

---

## Verification checklist

Run these in order. Stop at the first one that fails and report.

For standalone runs (outside of `afl-fuzz`) use `LD_PRELOAD`. `AFL_PRELOAD`
is only consulted by `afl-fuzz` itself, which then injects it as `LD_PRELOAD`
on its forked target.

### 1. Plumbing sanity — does a seed actually drive the engine through init?

```bash
cd /home/user/doom-port
LD_PRELOAD="$PWD/build-fuzz/shmpath_preload.so" \
  ./build-fuzz/doom-engine-fuzz < fuzz/in/seed_full.bin
echo "exit=$?"
```

Expected: stderr shows the engine log walking through
`HELLO → ARGS → CONFIG → WAD received OK → V_Init → M_LoadDefaults
(N keys parsed) → Z_Init → W_Init / adding miniwad.wad → DOOM 2 banner →
M_Init → R_Init (InitTextures/Flats/Sprites/Colormaps) → P_Init → HU_Init
→ ST_Init → [init complete] → [engine] client disconnected`. Exit code 0.

If you see only `bad HELLO`, the shim isn't loaded (check `LD_PRELOAD`
path). If parsing stops mid-way with an unexplained error, the seed is
malformed — re-run `make_seed.py`.

### 2. Doesn't hang on garbage

```bash
head -c 256K /dev/urandom > /tmp/garbage.bin
time LD_PRELOAD="$PWD/build-fuzz/shmpath_preload.so" \
  ./build-fuzz/doom-engine-fuzz < /tmp/garbage.bin
echo "exit=$?"
```

Expected: exit code 1, real time on the order of milliseconds. (Random
bytes don't satisfy the HELLO checks; `I_NetBootstrap` aborts with
`bad HELLO`.)

### 3. /tmp untouched, /dev/shm cleaned up

```bash
ls /tmp/doom-net-* 2>/dev/null && echo "BAD: /tmp was touched" || echo "OK"
ls /dev/shm/doom-net-* 2>/dev/null && echo "BAD: /dev/shm leak" || echo "OK"
```

Both should print `OK`. The engine writes to `/tmp/doom-net-<pid>/` from
its own perspective, the shim transparently redirects that to
`/dev/shm/doom-net-<pid>/`, and the engine's `cleanup_temp` atexit
handler removes the dir on shutdown.

### 4. Coverage actually grows

```bash
./tools/fuzz/run.sh
# Let it run 1-2 minutes, then Ctrl-C.
$HOME/AFLplusplus/afl-whatsup fuzz/out
```

Expected: `corpus count` greater than the 5 seeds in `fuzz/in`, and the
afl-fuzz UI's `total paths` field rises over time. Both indicate AFL is
finding new edges in `w_wad.c`, `r_data.c`, `m_misc.c`, etc.

### 5. Bug-finding actually works (planted-bug test)

Optional but the most convincing demonstration. Temporarily patch a
deliberate OOB read into one of the parsers, e.g. in `w_wad.c`'s
`W_GetNumForName` or in `m_misc.c`'s `M_LoadDefaults`. Rebuild with ASan:

```bash
rm -rf build-fuzz
CC=$HOME/AFLplusplus/afl-clang-fast \
  meson setup build-fuzz -Dfuzzing=true -Db_sanitize=address
ninja -C build-fuzz
./tools/fuzz/run.sh
```

AFL+ASan should find the planted bug in a few minutes. Revert the plant.

### 6. Crash replay

```bash
ls fuzz/out/default/crashes 2>/dev/null
LD_PRELOAD="$PWD/build-fuzz/shmpath_preload.so" \
  ./build-fuzz/doom-engine-fuzz < fuzz/out/default/crashes/id:000000*
```

Reproduces deterministically => crashes are real artifacts of the input,
not harness state.

---

## Troubleshooting

* `bad HELLO` on a known-good seed: shim isn't loaded. For standalone
  runs use `LD_PRELOAD=$PWD/build-fuzz/shmpath_preload.so`. For
  `afl-fuzz`, use `AFL_PRELOAD=$PWD/build-fuzz/shmpath_preload.so`.
* `undefined symbol: __afl_area_ptr` when `LD_PRELOAD`ing the shim:
  the shim got coverage-instrumented. It must be built with plain `cc`,
  not `afl-clang-fast`. `build.sh` already handles this.
* `IWAD file '...' not found`: an `access()`/`stat()` call hit `/tmp/...`
  before the shim rewrote it. Add the missing libc entry point to
  `shmpath_preload.c`.
* `exec speed` stuck at ~1/s: each iteration is timing out (5 s). Confirm
  step 1 completes well under that. If `R_Init` is slow because the WAD
  is huge, swap the seed WAD for something smaller; AFL will still expand
  from there.
* `[engine] WAD overrun` / `WAD size mismatch`: HELLO `wad_size` and the
  chunk total disagree. Regenerate seeds with `make_seed.py`.
* `/dev/shm/doom-net-*` directories piling up: `__wrap_exit` is calling
  `_exit` (which skips atexit). The current `harness_wraps.c` uses
  `__real_exit` to let the engine's `cleanup_temp` run.

## What's out of scope for v1

* `__AFL_LOOP` persistent mode (would need engine state reset).
* Custom mutator that understands the wire framing (would skip the
  framing-trivial mutations and focus on payload bytes).
* Post-`MSG_READY` traffic (`MSG_INPUT_EVENT`, `MSG_PING`, `MSG_BYE`).
* Fuzzing the supervisor itself (port parsing, signalfd, accept logic).
