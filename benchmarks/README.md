# Headless interpreter benchmarks

This directory contains the deterministic POSIX headless harness used to validate and benchmark the pure and cached R4300 interpreters.

## Build the harness

```sh
make -C benchmarks/headless
```

## Build benchmark-enabled cores

The normal core build is unchanged. Benchmark state export and forced CPU mode are enabled only with `HEADLESS_BENCHMARK=1`.

```sh
benchmarks/headless/build_core.sh pure
benchmarks/headless/build_core.sh cached
```

The generated benchmark cores export `retro_debug_timing()` solely for read-only timing/state logging. A normal `make` does not compile that export and retains the normal cached-interpreter default.

## 120-second equivalence run

```sh
benchmarks/headless/run_pair.sh \
    benchmarks/headless/core_pure.so \
    benchmarks/headless/core_cached.so \
    /path/to/game.z64 \
    7200 60
```

The harness uses zero controller input, counts video callbacks as VIs, hashes system RAM and generated audio, and records PC/CP0/event state every 60 VIs. `run_pair.sh` ignores only the expected `emumode` selector difference (pure=0, cached=1); every architectural, timing, audio, and RDRAM field must otherwise be identical.

## Repeated speed measurement

```sh
benchmarks/headless/bench_repeat.sh benchmarks/headless/core_pure.so /path/to/game.z64 1800 5
```

`NO_RAM_HASH=1` is used for performance runs so hashing 8 MiB of RDRAM does not dominate host timing. Correctness runs should leave RAM hashing enabled.

## Included results

`results/` contains representative 7,200-VI logs from the pure-interpreter optimization work. ROM files and built core binaries are intentionally not included.

The harness is POSIX-oriented (`dlopen`, `/usr/bin/time`) and was used under Linux. The core itself remains portable as before.
