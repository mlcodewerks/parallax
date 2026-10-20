# Headless interpreter benchmarks

Production RSP routing is automatic. The old HLE/LLE environment overrides in
benchmark commands are private controls for instrumented comparison cores.
See [automatic RSP support and validation](NATIVE_FALLBACK_COMPLETION.md).
The [paraLLEl-RDP update](PARALLEL_RDP_UPDATE.md) records shader generation,
GPU conformance checks and the separate limits of RDP timing validation.

## Fast-mode refinement

[Fast timing bookkeeping and FIFO arithmetic](FAST_MODE_REFINEMENT.md)
reduce x64 helper/cache-hit overhead while preserving the modeled clocks.
Knife Edge's 48 combat tasks now match all 18,119 native triangle packets;
BAR and DK64 also gain native coefficient matches. Full Fast/Accurate game
comparisons, remaining accuracy limits and speed methodology are documented.

## FIFO 2.05 HLE walker and measured RSP tasks

[The F3DEX FIFO 2.05 walker](F3DEX205.md) now recognizes Knife Edge's GBI2
variant, restores native far-plane rejection and uses its reciprocal texture
normalization. All 16 focused cases and all 48 combat packet shapes match
native execution; remaining triangle coefficient differences are documented.
The matched 20 s HLE combat span improves from 24.25 to 20.63 s without
refitting timing coefficients. [Benchmark-only task counting](HLE_TASK_CYCLES.md)
records 2,178 completed native executions and explicitly lists coverage gaps.

## Knife Edge and Beetle Adventure Racing timing

[Refined combat estimates and reproduction commands](KNIFE_COMBAT_REFINED.md)
bring a 20-second reference flight section from 15.80 to 19.81 s with native
RSP. A shared short-span read-response estimate uses no new profiles or clock
rescaling. Trigger-only and no-fire controls also improve. HLE combat
previously regressed to 24.25 s; the FIFO 2.05 correction above reduces it to
20.63 s. Use native RSP for
the improved result. The matched native intro measures 122.97 s versus 125.90 s.

[The preceding two-video/BAR analysis](VIDEO_TIMING_FOLLOWUP.md) records how
read/write turnaround costs improve Knife Edge's matched intro from 101.49 s
to 104.49 s, against 125.60-125.90 s in two recordings; it remains too fast.
BAR measures 81.169 s against 79.884 s, with audio and visual alignment both
consistent with a roughly 1.6-1.7% capture-timebase difference. No clock
rescaling or new game profile is used. Combat-input limitations and
[hardware-model limits](RDP_BUS_TIMING.md) are documented with the measurements.

## GLES3 renderer and batching

[Implementation, Top Gear Rally results and validation](GLES3_RENDERER.md).

GLRDP's embedded backend now requires OpenGL ES 3.0 and shares the GL3.3
integer renderer, uniform-buffer primitive batches, shader specialization and
immutable texture atlas. GLES2 has been removed. The GLES2 measurements below
describe historical builds. Use `gles3` for current harness backend arguments.

For Raspberry Pi 4/5 with 64-bit Raspberry Pi OS and Mesa, build with a C++20
compiler using `make -j4 platform=rpi4 ARCH=aarch64` or
`make -j4 platform=rpi5 ARCH=aarch64`. A generic Linux ES build can use
`make -j4 platform=unix ARCH=aarch64 FORCE_GLES3=1`. Select GLRDP and an ES3-capable
frontend video driver. Start at native 1x resolution. Physical Pi speed has to
be measured on-device; desktop/ANGLE validation does not establish Pi throughput.

The ES3 loader does not require desktop polygon modes, texture barriers or
multisample texture APIs. Depth/color batches use ping-pong Z/DZ textures and
bounded feedback copies. Integer readbacks use the portable ES3 RGBA pair,
and MRT output locations match their attachment indices. Batch capacity adapts
to uniform-block size, including the 16 KiB ES3 minimum.

On Windows with an EGL ES3 implementation on `PATH`:

```sh
make -f benchmarks/glrdp_checks.mk -j3 benchmarks/gles3_checks.exe benchmarks/glrdp_checks.exe benchmarks/renderer_options_checks.exe benchmarks/rdp_reference_checks.exe benchmarks/vi_reference_checks.exe benchmarks/tc_reference_checks.exe
benchmarks/gles3_checks.exe
benchmarks/renderer_options_checks.exe
benchmarks/glrdp_checks.exe
GLRDP_TEST_NO_BARRIER=1 benchmarks/glrdp_checks.exe
benchmarks/rdp_reference_checks.exe --gles3 --wrapped-tile
benchmarks/rdp_reference_checks.exe --gles3 --depth
benchmarks/vi_reference_checks.exe --gles3
benchmarks/tc_reference_checks.exe --gles3
```

`gles3_checks.exe` blocks desktop-only entry points and limits the renderer to
16 KiB uniform blocks while checking batching, atlas sampling, framebuffer
coherence, runtime accuracy changes, startup specialization and frontend UBO
state restoration. `M64P_BENCH_NATIVE_ES=1` optionally requests a native ES3 WGL
context on drivers supporting that profile; the default uses EGL/ANGLE.

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
Coverage now supports `--coverage off|2x|reduced|full`: Off skips fractional
coverage counting; the other levels use two, four or eight native samples.
`--shading fast` selects floating-point vertex colour planes.
`--shader-specialization enabled` removes unused accuracy paths by compiling
the selected settings at startup, retaining a precompiled generic fallback.
Both GL3.3 and GLES2 support these controls. Specialization preserves the
selected accuracy; fast shading and reduced coverage change the picture.
The profile now selects 1x, dynarec, graphics/audio HLE, per-cycle timing off,
component timing and the fast CPU timing approximation, 2x coverage, fast
shading and specialization. `--input path.csv` can replay
menu/gameplay input; zero input remains the default.

With per-cycle timing off, `--component-timing disabled` and
`--fast-cpu-timing disabled` independently disable the new timing estimates.
Both default to enabled. [The DK64 timing audit](DK64_FAST_TIMING.md) documents
the tags-only CPU model, the successful second-vine crossing, regression checks
and measured DKR race performance.

The DKR USA Rev 1 [race input](dkr-race-input.csv) selects Diddy, Tracks and
Ancient Lake, then holds accelerate without steering. Run it to at least
VI 7,200; the final sample is about 35 seconds into the race. For example:

```sh
python benchmarks/glrdp_intro_benchmark.py --core path/to/benchmark.dll --rom "path/to/Diddy Kong Racing (USA) (En,Fr) (Rev 1).z64" --output benchmarks/results/glrdp-hle-performance/dkr-race --cpu dynarec --vis 7200 --runs 3 --input benchmarks/dkr-race-input.csv --coverage 2x --shader-specialization enabled --shading fast --blending fast --combiner fast --texture-filter point --vi-filter disabled --texcoords fast --texture-lod disabled --color-dither disabled --alpha-dither disabled
```

Use a separate `--capture --runs 1` run for screenshots and RAM hashes.
Measurements and limits for these controls are recorded in
[sampling validation](glrdp_sampling_validation.json). On the RTX 3050 laptop,
three intro repeats averaged 157.7–158.8 VIs/s, with slowest rolling 60-VI
windows of 83.2–85.2 VIs/s. Two 7,200-VI menu/Ancient Lake repeats averaged
143.1–146.4 VIs/s and kept every such window above 64.7 VIs/s. A separate
captured race run also stayed above 60. Individual VI calls still exceed
16.67 ms; this establishes throughput headroom for those replays, rather than
a latency guarantee for all frontends or tracks. Default rendering matches
the preceding build at all twelve captured images and sixty CPU/RAM/audio
checkpoints. GL3.3, its copy fallback, GLES2 highp/mediump and 980 VI reference
cases per backend/precision path pass. The older ubershader measurements below
predate these additions.
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

## Audio/video clock validation

The audio diagnostic core retains the production CPU selector and logs the
monotonic guest clock, VI rate, advertised frontend rate, delivered PCM count,
and audio queue depth. Build it separately from the release DLL:

```sh
make -f benchmarks/audio_sync_core.mk -j4 platform=win DEBUG=0 TARGET=benchmarks/.optimization-baseline/audio-clock.dll
make -C benchmarks gameplay_benchmark_audio
```

For Knife Edge USA, run beyond the first boss with a fresh save and the included
input replay. These are MSYS shell commands, run from the repository root:

```sh
out=benchmarks/results/audio-clock/knife-boss
mkdir -p "$out"
env wtfn64_renderer=glrdp wtfn64_cpu_core=cached \
    wtfn64_rsp_hle=enabled wtfn64_lle_graphics=disabled \
    wtfn64_lle_audio=disabled wtfn64_per_cycle_timing=enabled NO_RAM_HASH=1 \
    M64P_BENCH_DIR="$out/save" M64P_BENCH_CAPTURE="$out/frame" \
    M64P_BENCH_CAPTURE_INTERVAL=900 M64P_BENCH_VI_TRACE="$out/vi.csv" \
    M64P_AUDIO_SYNC_TRACE="$out/sync.csv" \
    M64P_BENCH_INPUT=benchmarks/knife-boss-input.csv \
    M64P_BENCH_PIN_BYTE=1500,11d464,100 \
    benchmarks/gameplay_benchmark_audio.exe \
    benchmarks/.optimization-baseline/audio-clock.dll \
    "/path/to/Knife Edge - Nose Gunner (USA).z64" 18600 60 "$out/run.csv" \
    2>"$out/run.log"
python benchmarks/audio_clock_checks.py --after "$out" --output "$out/checks.json"
```

The optional RAM pin syntax is `start_vi,hex_host_offset,byte`. This replay pins
only USA ship armor at guest byte `0x11d467` (word-swapped host byte `0x11d464`)
to 100 so an unattended run survives to the boss. It is a harness-only fixture;
the release core has no health override. Inspect the captured boss footage as
well as checking the elapsed frame count.

`audio_clock_checks.py` requires bounded audio latency, no outstanding PCM debt,
frontend pacing matching the VI oscillator, and delivered PCM within one sample
of elapsed guest time. Supplying `--before <folder>` additionally compares every
VI register trace, CPU checkpoint and captured image, allowing only the intended
audio delivery changes. `knife-audio-input.csv` and `blues-audio-input.csv`
reproduce the shorter before/after intro and menu comparisons. Normal release
builds omit the trace and benchmark exports. Repeat the boss run with
`wtfn64_cpu_core=dynarec` to cover both CPU implementations in per-cycle mode.

## Knife Edge guest pacing and full LLE

The [ROM disassembly audit and replay instructions](KNIFE_EDGE_TIMING.md)
cover FullSync scheduling, HLE producer overlap, native RSP slice deadlines,
fractional clocks, savestate continuity and automatic GLRDP API selection.
`knife_pacing_checks.py` defaults to HLE disabled for both graphics and audio.

## Streaming HLE continuation dispatch

Active Gauntlet, Rogue Squadron, Naboo/Indiana Jones and BOSS tasks reuse their
validated handler between slices. Polling clocks and handshakes are unchanged.
See [measurements and validation](STREAMING_HLE_PERFORMANCE.md) and the
[recorded comparison metadata](streaming-hle-validation.json).
