# Ari64 host emitters

`ari64.h` is the common emitter interface. `HAVE_ARI64` exposes the dynarec
CPU option on x64 and AArch64 Linux. The original x64 implementation is
described below; ARM64 uses the same decoded blocks, invalidation rules and
precise instruction handlers. See [ARM64 builds and validation](../../../../benchmarks/ARI64_ARM64.md).

`ari64_arm64.c` and `arm64/*.inc` follow the x64 backend's module structure:
integer/multiply/divide, next-use allocation and constant folding, guarded RAM,
branches and safe integer delay slots, COP1 arithmetic/conversions/comparisons
and transfers, emitted timing, and guarded successor links. Seven preserved
host registers cache GPR/HI/LO values. Scalar A64 FP instructions save/restore
FPCR/FPSR and use the same guest operand and exception validation as x64.
Native regions preserve the x64 guard order, precise prefix writebacks,
continuation snapshots, event boundaries and half-rate COUNT accounting.
MMIO, alignment/cache misses, tracked code writes, trapping ALU, COP0, CACHE,
unsafe delay slots and other sensitive cases retain shared precise handlers.

The ARM64 arena is 8 MiB split into sixteen 512 KiB segments. Entries carry
segment generations; recycling invalidates old entries before reuse.
Publication switches RW pages to RX and clears the instruction cache.
Helpers follow AAPCS64 with a 16-byte aligned stack and preserved x19–x30.
Validated direct links and C-linker successors reuse the generated frame,
so chaining does not grow the stack. Compilation never occurs
inside a generated frame. Linux is supported; Apple JIT entitlements and
Windows ARM64 allocation are outside this backend.

## x64 implementation

Source: [libretro/parallel-n64](https://github.com/libretro/parallel-n64/tree/ef73c7e6fa356262f88e05f85cdfad092f8f90e0/mupen64plus-core/src/device/r4300/new_dynarec),
commit `ef73c7e6fa356262f88e05f85cdfad092f8f90e0` (retrieved 2026-10-07).
The GPL-2.0-or-later notices are retained for the imported x64 emitter.

`ari64_x64.c` adapts the upstream x64 output, register-copy, immediate and
shift emitters. `x64/assem_x64.h` is the upstream header. The extraction can
be reproduced with `benchmarks/ari64_extract_emitter.py`; it writes only a
temporary emitter prefix, never overwrites the completed integration.

The frontend adapts Ari64's register caching, next-use allocation, constant
propagation, dirty-code verification, hot-block compilation and block linking
to this repository's CPU layout and decoded cache. It is an Ari64-emitter-based
JIT, not a build of upstream's complete global-state recompiler. Eight host
registers on Windows (six on System V) cache GPR/HI/LO values; dirty values
are written before helpers and exits. Dead overwritten values avoid
unnecessary spills in ALU-only regions.
Common ALU operations work directly on allocated registers. Short immediates,
folded constants and discarded nontrapping r0 results reduce emitted code.
Cold execution reuses the decoded block already found by the dispatcher.
Compilation starts after eight visits for blocks with native coverage and
three for precise-only blocks, preserving frequently used linking wrappers.

Native coverage includes integer arithmetic, comparisons, shifts, HI/LO
transfers, all eight multiply/divide instructions, ordinary conditional
branches (including REGIMM link and likely/annulled slots) and jumps with
safe integer delay slots, and aligned integer and COP1 RAM loads/stores.
SSE2 emits single/double arithmetic, square roots,
sign operations, conversions, fixed-rounding conversions and all comparison
predicates. COP1 register transfers use the live FR-dependent register layout.
Floating-point validation shares the pure interpreter's NaN, denormal, FCR31,
exception and latency rules. One outward preparation helper performs the
instruction timing entry and input validation. Ordinary result/flag/latency commits are emitted;
exceptional outputs, enabled exceptions and event boundaries use shared
result handling. Generated SSE instructions
save/restore host MXCSR and disable DAZ/FTZ. MOV.S retains this interpreter's
full-register copy behavior. FCR31 writes, BC1, trapping arithmetic, COP0,
CACHE, MMIO and other sensitive cases use precise
shared instruction bodies. Shared delay slots cannot enter native compilation.

Native ALU/RAM regions guard scheduler deadlines and CPU mode before changing
state, then emit the existing instruction-fetch/cache and two-entry micro-TLB
costs, load/DCB history, COUNT phase and instruction-boundary commits directly.
RAM paths check live translation, cacheability, installed memory handlers,
alignment, cache tags and physical code-page protection. Misses/faults commit
the completed prefix and execute the current precise instruction; a validated
continuation reloads host registers and resumes the same generated block.
Events and exceptional paths exit at the same instruction boundary as pure
execution. COP1 latency is charged only after successful result commit.
The model retains this repository's instruction-boundary approximation;
it does not introduce a hardware pipeline-stage simulator.

Word arithmetic truncates and sign-extends explicitly. Logical and 64-bit
operations retain the full GPR value. SRA/SRAV retain this core's full-input
shift behavior before word truncation. Shift masks, register aliases and r0
destinations are covered by full-width interpreter differentials. Mode checks
run before native instructions, including disallowed 64-bit operations.
Multiply/divide preserve the pure interpreter's full-width operand behavior,
including divide-by-zero and signed-overflow results. Emitter fixes cover
64-bit copies, zero-immediate copies, extended-register word shifts, RSP/R12
LEA addressing and byte-register SETcc encodings. Emitter word writes use
`memcpy` to avoid unaligned C pointer accesses.

The shared cache's virtual mapping, physical page generation and epoch checks
cover CPU writes, DMA, TLB remapping, resets and save-state restores. A write
or mapping change exits the current native block before its next instruction.
Unchanged source words, including delay/idle lookahead, retain compiled code
after a page-generation or mapping-epoch change only when the physical mapping
still matches. Changed source forces a rebuild. On 64-bit hosts, both cached
interpreter and JIT use 131,072 decoded block slots. The JIT uses a 256 MiB
native arena divided into thirty-two independently
recycled segments; entry validity checks the owning segment's generation.
The recycling granularity remains 8 MiB. The cached interpreter does not
allocate the native arena, and 32-bit decoded-cache capacity remains 65,536.
Recycling only happens outside native execution. Code pages are writable
during compilation and executable after publication, never both together.
Validated tail links reuse one native stack frame and never compile from that
frame. Recurring successors jump through emitted guards for entry identity,
page generation, epoch and segment generation. Mapped successors also check
their live read-TLB entry before jumping; cacheability checks stay in the body.
Misses use the C linker; link caches are data, not writable code patches.
On Windows the arena prefers an address near the core module, allowing the
upstream five-byte rel32 helper-call encoding. Calls outside its signed 32-bit
range retain the absolute fallback. ModRM/SIB addressing uses zero/byte
displacements where possible, and adjacent statistics share one address load.
Calls preserve the required registers, stack alignment and Microsoft
x64 shadow space; System V x64 support has not been tested on a Unix host.

`HAVE_ARI64` is set for `WITH_DYNAREC=x86_64` or Linux `aarch64`. It deliberately does
not set `NEW_DYNAREC`: those old hooks expect upstream's different hot-state
layout and memory map. The existing CPU/memory accessors stay authoritative.
Builds with `WITH_DYNAREC=` retain the interpreter-only implementation.

The restart-only `wtfn64_cpu_core` option selects `dynarec`, `cached`, or
`pure`. Cached interpreter is the default; `dynarec` is available only in
x64 and AArch64 Linux JIT builds. Invalid or unavailable values select cached interpreter.
The shared Fast/Accurate timing choice, HLE/LLE routing, renderers and video
options apply to all CPU implementations. Issue timing is selected automatically
using validated ROM profiles; there is no manual issue-policy control.
Startup allocation failure selects the pure interpreter and is visible in
the startup log. A later code-publication failure logs an error and stops the
CPU instead of silently claiming native execution. Content unload frees the
arena; generated code is not included in save states.

See [validation](../../../../benchmarks/ARI64_X64.md) for commands and results.
See [performance measurements](../../../../benchmarks/ARI64_SPEED.md) for the
restored emitter's linkage changes and Rare-game intro checks. The
[focused audit](../../../../benchmarks/ARI64_FOCUS_SPEED.md) records the
latest scheduler guards, register allocation and matched performance results.
