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

## Native GLRDP integration checks

The Windows GPU harness negotiates actual GL3.3 or GLES2 contexts, reads back
frames and compares matching VI checkpoints against Angrylion. Build from the
repository root with MinGW tools on PATH:

```sh
make -f benchmarks/glrdp_checks.mk -j4 benchmarks/renderer_options_checks.exe benchmarks/fb_write_checks.exe benchmarks/rsp_dma_checks.exe benchmarks/rsp_memory_checks.exe benchmarks/glrdp_checks.exe benchmarks/gles2_checks.exe benchmarks/vi_reference_checks.exe
make -f benchmarks/cpu_compat_checks.mk -j3 benchmarks/cpu_compat_checks.exe cpu_timing cpu_fpu_diff
```

Run the option, framebuffer/DMA and CPU binaries directly. Run
`benchmarks/glrdp_checks.exe` and `benchmarks/gles2_checks.exe`; the GLES2 binary
checks both highp and forced mediump. Run `benchmarks/vi_reference_checks.exe`
with no argument, with `--gles2`, and with `--gles2 --mediump`.

The isolated `benchmarks/port_core.mk` adds timing exports to a separate frontend
object while retaining production common objects. It forces the cached CPU for
renderer comparisons, and leaves the normal DLL's CPU selection intact:

```sh
make -f benchmarks/port_core.mk -j4 platform=win DEBUG=0 TARGET=benchmarks/.optimization-baseline/core_glrdp_port.dll
make -C benchmarks gameplay_benchmark_interval
```

`gameplay_sweep.py` exposes independent coverage and VI flags and records all
settings in replay metadata. Fresh saves and matching inputs are required for
comparisons. Run native GPU measurements sequentially.

[Integration validation](GLRDP_INTEGRATION.md).
