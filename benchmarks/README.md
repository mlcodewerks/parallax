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

## GLRDP HLE intro performance

The performance core exposes timing while honoring the production CPU selector.
Unlike `port_core.mk`, it does not force the cached interpreter. Build with
MinGW tools on PATH:

```sh
make -f benchmarks/performance_core.mk -j4 platform=win DEBUG=0 TARGET=benchmarks/results/glrdp-hle-performance/core.dll
make -C benchmarks gameplay_benchmark_frametime
python benchmarks/glrdp_intro_benchmark.py --core benchmarks/results/glrdp-hle-performance/core.dll --rom "path/to/Diddy Kong Racing (USA) (En,Fr) (Rev 1).z64" --output benchmarks/results/glrdp-hle-performance/repeat --runs 3
```

Each sequential run uses a fresh save and zero input for 3,600 VIs. The harness
waits for GPU completion every VI (`M64P_BENCH_GPU_SYNC=1`), records frame times,
and reports average throughput, the slowest rolling 60-VI window, p99/max frame
time and frames exceeding 16.67 ms. No startup frames are omitted. Shader/context
initialization happens before `retro_run`; wall time and time outside
`retro_run` are reported separately. Results identify the core, harness and ROM
by SHA256, and verify the selected CPU plus actual graphics/audio HLE task counts.
These timings include synchronization overhead and do not simulate a frontend's
vsync scheduler. Driver disk caches are left untouched.

Use `--cpu dynarec`, `--backend gles2`, `--blending fast`, `--combiner fast`, or
`--vi-filter disabled` and `--texture-filter point` to compare settings. Always choose a new output directory.
Additional independent controls are `--texcoords fast`, `--texture-lod disabled`,
`--color-dither disabled` and `--alpha-dither disabled`. Their defaults preserve
the existing rendering behavior. [The DKR performance options](glrdp-dkr-fast.opt)
collect the available speed/accuracy tradeoffs; restart content after applying.
`--capture` writes images every 300 VIs and includes their readback cost in the
timings. It also hashes RDRAM every checkpoint for state comparisons. Use
separate capture runs for visual comparisons.

The GPU tests reject any shader compile/link during cycle/coverage/arithmetic
switches. Run `GLRDP_TEST_NO_BARRIER=1 benchmarks/glrdp_checks.exe` to exercise
the GL3.3 copy fallback as well as the normal optional texture-barrier path.

The [recorded ubershader/control validation](glrdp_ubershader_validation.json)
includes the production DLL hash, seven passing check suites, 620 VI cases on
each backend/precision path, and all three measured intro repeats. Default
settings match the original build at all twelve image samples and sixty
CPU/RAM/audio checkpoints. The full performance configuration averaged
112–142 VIs/s on the tested RTX 3050 laptop; two repeats stayed above 60 VIs/s
in every rolling 60-VI window, while the third retained a 338 ms late stall.
Consistent 60 VIs/s across every repeat is therefore not established. No startup
frames or outliers were discarded.

## GLRDP interlaced scanout

`wtfn64_glrdp_deinterlace=weave` retains alternating fields by default; `bob`
duplicates the current field. This setting applies independently of VI filters.
GLRDP assembles fields at the TV raster resolution before frontend scaling.
The active VI window already includes game letterboxing; it does not set a
second canvas aspect ratio. This fixes WDC's compressed intro picture.

The VI reference suite now checks 932 cases on GL3.3, GLES2 highp and GLES2
mediump, including consecutive 16/32-bit NTSC/PAL fields, 320/400/1024-pixel
strides, weave/bob, blanking and borders. `vi_field_checks.exe` also checks
repeated fields, missing-current fallback and history invalidation on mode/reset
changes. `fb_write_checks.exe` includes zero-mask stores, which must leave RAM
and framebuffer ownership untouched instead of reporting "Unknown mask".

Reproduce the real-ROM comparison with a preserved build from before the fix:

```sh
make -f benchmarks/performance_core.mk -j4 platform=win DEBUG=0 TARGET=benchmarks/results/glrdp-interlace-wdc/after.dll
make -C benchmarks gameplay_benchmark_vi
python benchmarks/glrdp_interlace_validation.py --before path/to/before.dll --after benchmarks/results/glrdp-interlace-wdc/after.dll --roms path/to/N64 --output benchmarks/results/glrdp-interlace-wdc/replay
```

Use a fresh output directory; add `--backend gles2` for ES2. Each comparison
boots WDC USA and Rogue Squadron USA Rev 1 with fresh saves, graphics/audio HLE,
cached CPU, per-cycle timing off, native resolution and zero input. It captures
41 consecutive intro fields in each game, checks the untouched sibling rows,
compares VI registers with Angrylion, and verifies unchanged guest CPU/RAM/audio
checkpoints. The benchmark-only `M64P_BENCH_VI_TRACE` records all 14 VI registers
at each video callback. These correctness captures are not performance runs.

[Recorded scanout validation](glrdp_interlace_validation.json) identifies the
builds, measurements and the scope of the visual comparison. Ordinary motion
combing remains possible with weave; bob is available for that tradeoff.

## WDC selection menu regression

WDC's menu render targets extend 482 rows but are only 480 rows apart in RAM.
Choosing an image solely because it contains VI_ORIGIN can select the last row
of the neighbouring buffer, producing a black field every fourth VI. Scanout
now prefers the view covering the displayed address span before comparing age.
Point lookups keep their existing behavior.

The VI suite includes overlapping-image selection and actual presentation at
1x/2x, 16/32-bit and progressive/interlaced output: 980 cases on GL3.3, GLES2
highp and GLES2 mediump. The prior 932-case result above predates these additions.

`wdc-menu-input.csv` skips the intro, enters Quick Race and Car Select, changes
car, and returns to the Quick Race menu. Replay the same fresh-save inputs on
the before-fix benchmark core, corrected core and Angrylion:

```sh
python benchmarks/wdc_menu_validation.py --before path/to/before-menu.dll --after path/to/after-menu.dll --rom "path/to/World Driver Championship (USA).z64" --output benchmarks/results/glrdp-interlace-wdc/menu-replay
```

Use `--backend gles2` for ES2. Build the benchmark core and VI harness as above;
ordinary production cores omit the required debug exports. The replay captures
VIs 4345–4385, compares both fields' brightness against Angrylion to detect
missing picture rows, and checks VI registers and guest CPU/RAM/audio state.
It does not require exact RGB agreement between different renderer defaults.
[Recorded menu validation](wdc_menu_validation.json) includes the builds and
before/after results.
