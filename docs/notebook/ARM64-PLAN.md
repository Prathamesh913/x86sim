# arm64 runtime on x86_64 Linux: research and plan (2026-10-29)

Goal: run the arm64-only iOS 27.x simulator runtime on x86_64 Linux with near-native feel. Apple dropped x86_64 after
26.5 23F73. On the Mac (Xcode 27.1), the 27.0 (24A434) and 26.5 23F77 runtimes are arm64 only.

## Facts about the iOS 27.0 runtime (static inspection on the Mac)

- **Architecture:** plain arm64 (cpusubtype ALL), no arm64e. 0 PAC instructions across ~5M sampled instructions;
  chained fixups are DYLD_CHAINED_PTR_64_OFFSET with no auth fixups. The only arm64e slices are in 3 macOS host-side
  tools, which also carry an arm64 slice.
- **Shared cache:** 27 ships its dyld shared cache prebuilt inside the runtime
  (RuntimeRoot/System/Library/Caches/com.apple.dyld/dyld_sim_shared_cache_arm64*, 3.83 GB, 1826 images, ~1.78 GB of
  __text, 75% of it in Swift images). It has no slide info and maps at a fixed 0x1C0000000-0x2ABCC8000; the dylibs are no
  longer separate files.
- **Syscalls:** the runtime has **no svc and no tpidrro_el0**. libsystem_kernel/platform/pthread in the cache are
  placeholders that resolve at run time to host libraries (libsystem_sim_kernel_host → /usr/lib/system/...). So every
  syscall, TLS access and commpage access lives in three libraries **we supply**.
- **Instructions a translator must handle:** base A64, full AdvSIMD/FP, exclusives + LSE (cas/casp/ldadd/swp...),
  LRCPC ldapr, FRINTTS; SHA1/256/512/SHA3/PMULL and DIT reads (libcorecrypto); MTE ops in libsystem_malloc (likely
  gated on hardware support: decode or trap cleanly). None found: PAC, SVE, SME, FP16, dotprod, MOPS.
- **Pages:** segments are 16K-aligned (fine on a 4K host); vm_page_size seen by the guest should be 16K.
- **Simulator interfaces:** mostly unchanged from 26.5.
  - New: com.apple.CoreSimulator.NearbyInteraction, SFBDisplayGetDeviceName (add it to host/simfb), and
    UniversalHID.framework.
  - Metal sim selectors changed: new ones added; set*GlobalConstants* and supportsFP8 removed.
  - Host frameworks loaded into sim processes (SimFramebuffer, SimulatorHID and our shims) must be arm64 or thunked.
- **Device types:** no foldable in 27.0's device types yet (62 types, iPhone 11 to 18 Pro Max, iPads to M5).

## Prior work

- **Nothing public** runs the simulator runtime outside macOS.
- [darling PR #1753](https://github.com/darlinghq/darling/pull/1753) (GPL-3): Darling on arm64 Linux running arm64 and
  arm64e macOS binaries (CMake TARGET_ARM64, arm64 headers/libm, mldr <2^47 mapping fix). Companion PRs #1754 and #1450;
  xchemtina/darling-arm64.
- **dynarmic** (0BSD): an embeddable A64→x86-64 JIT (yuzu/Eden/touchHLE). No PAC (not needed), LSE incomplete.
- touchHLE (dynarmic + native host frameworks), ipasim (guest app code + native frameworks; its auto-generated glue
  broke on callbacks, blocks and varargs), FEX/Box64 thunks, ARM64EC entry/exit thunks, Rosetta 2 (AOT + JIT).
- **QEMU/Unicorn:** complete but slow (3-10x). Use as a correctness oracle, not to ship.

## Architecture

One x86_64 Linux process per simulator process:
- mldr (x86) maps the guest image and the prebuilt cache at their fixed addresses in its own address space, so guest
  and host share one identity-mapped address space and loads/stores need no translation.
- A dynarmic JIT per guest thread runs the arm64 code.
- The three libsystem host libraries (kernel/platform/pthread) are **native x86 code behind thunks**: the natural ABI
  boundary, since the runtime never makes a syscall itself. Darling's existing x86 syscall/Mach emulation is reused as is.
- darlingserver and all host services (simfb, simhid, iosurfaced, metalhostd→mtlvk, mdns, FileProvider, FSEvents)
  stay native x86 and unchanged (sockets/shm). Only protocol structs that carry thread state need care.

Performance tiers (estimates, not measured):
- JIT only: ~3-6x slower than native on UI code.
- Plus native hot paths (memcpy/malloc/objc_msgSend/libdispatch thunks, an indirect-branch cache, a persistent
  translation cache keyed by the cache UUID): ~1.5-2.5x.
- AOT "x86 shadow cache": translate the fixed 1.78 GB of cache text once per runtime build (rellume/remill → LLVM),
  with the JIT as fallback (app code, JavaScriptCore JIT output): possibly 1.2-1.5x (Rosetta 2 territory).

## Stages

0. **PoC:** dynarmic + a minimal loader runs one arm64 iOS-simulator command-line binary on x86 Linux, with native
   x86 libsystem thunks. Measures the boundary design and raw JIT speed.
1. **Integration:** mldr hosts the JIT for arm64 Mach-O; maps the prebuilt cache; thunks libsystem_*; per-thread JIT
   state; signals/exceptions (host fault at a JIT PC → guest context); guest vm_page_size 16K.
2. **Boot:** launchd_sim → SpringBoard on the 27.0 runtime; port the host-side shims to the new interfaces
   (SFBDisplayGetDeviceName, Metal selectors).
3. **Fast:** persistent JIT cache, native objc_msgSend/indirect-branch cache, hot-library thunks.
4. **Near-native:** AOT shadow cache.

## Risks

- **JIT completeness:** LSE, NEON corner cases, MTE paths.
- **Signals and exceptions** through JIT frames; Swift async and unwinding (unwind info is guest arm64).
- **Glue across the native boundary:** callbacks, blocks, varargs. Keep the boundary at plain C functions.
- **JavaScriptCore / WebKit** generates arm64 code at run time (self-modifying-code detection).
- **The AOT cache is a big build:** CPU and disk. It must run under the IO cap and only with approval.

## Stage 0 results (2026-10-29)

**What runs.** `linux/arm64run` (standalone, ~600 lines C++) runs plain-arm64 iOS-simulator Mach-Os built with
`xcrun -sdk iphonesimulator` (`build/mac-build-arm64-tests.sh`, sources in `tests/arm64/`) on the Ryzen 7 9800X3D:
- (a) `hello.sim`: printf with stack varargs, puts, exit code 7. OK.
- (b) `bench.sim`: all 8 sub-benchmarks give the same results as the Mac and x86 builds.
- (d) `threads.sim`: 4 pthreads, each with its own Jit; inline LSE (`ldadd`, `casal`, `swpal`, which clang emits for
  the simulator target, CPU apple-m1), a mutex (static initializer); all counters correct.
- (c) `objcswift.sim` (libobjc + libswiftCore) **does not run yet**: it needs guest libobjc/libswiftCore from the
  runtime's shared cache plus a real dyld (classic binds, initializers, objc image registration, TLS). Decision: don't
  extract a subset or copy the 3.8 GB cache for stage 0. A dsc_extractor subset would mean writing a second dyld in
  arm64run. The right path is stage 1: run the runtime's own dyld_sim under the JIT against the mapped prebuilt cache,
  with libsystem_kernel/platform/pthread as the thunked host libraries. Mac baselines for (c) are below.

Build: `external/dynarmic` = azahar-emu/dynarmic a466015 (unmodified, so no fork yet; 0BSD). The arm64run CMake builds
it in-tree; needs boost headers (pacman `boost`). Example: `cmake -G Ninja -DCMAKE_BUILD_TYPE=Release
-DCMAKE_CXX_COMPILER=clang++ linux/arm64run` in a scratch directory, then `ARM64RUN_STATS=1 arm64run bench.sim 5`.

**Picking dynarmic.** Neither azahar-emu (standalone repo, active, A64 frontend and x64 backend kept) nor Eden
(in-tree `src/dynarmic`, 643 A64 patterns, same as azahar) decodes any LSE, CASP or LDAPR: azahar has them only as
commented-out patterns; Eden deleted the comments. touchHLE's fork is A32 only (iPhone OS armv7); merryhime's is
archived. Azahar was picked because it's a standalone repo with submodules (cheap to vendor and fork).

### Benchmark (best of 5, ms; lower is better)

| sub-benchmark | x86 native -O2 | x86 native -march=native | Mac M1 arm64 native | arm64 under JIT | JIT / x86 native(1) | JIT / M1 |
|---|---:|---:|---:|---:|---:|---:|
| int (xorshift, mul, div, branches) | 331 | 322 | 389 | 475 | 1.47x | 1.22x |
| calls (fib 32) | 3.9 | 4.0 | 6.6 | 14.4 | 3.6x | 2.2x |
| indirect (fn-pointer dispatch) | 28.9 | 33.8 | 28.1 | 68.1 | 2.0x | 2.4x |
| memcpy 16 MB (native thunk) | 8.4 | 8.4 | 22.1 | 8.4 | 1.0x | 0.38x |
| copyloop (guest loads/stores) | 2.1 | 2.1 | 5.3 | 4.6 | 2.2x | 0.87x |
| sort (quicksort 2M ints) | 99.7 | 100.0 | 114.5 | 187.0 | 1.87x | 1.63x |
| fp (scalar double mandelbrot) | 78.3(2) | 35.9 | 56.5 | 242.9 | 6.8x | 4.3x |
| simd (auto-vectorized float) | 40.8 | 80.7 | 99.1 | 438.2 | 5.4x (10.7x vs -O2) | 4.4x |
| **total** | 593 | 587 | 722 | 1438 | **2.45x** | **2.0x** |

(1) vs `-march=native` (FMA, like the arm64 build); same clang, same source. (2) Without FMA the x86 -O2 build gives a
different fp checksum (21959260), so -march=native is the fair baseline.
Mac (c) baseline (`objcswift.mac`, ms): msgSend 20M 53.1, objc alloc/release 2M 40.7, generic sum Int 0.68 / Double
2.0, existential 10M 28.8, ARC list 2M nodes 92.6, Dictionary 1M 20.4.
`ARM64RUN_UNSAFE=1` (dynarmic's InaccurateNaN, IgnoreStandardFPCR, ReducedErrorFP) changed nothing.

**qemu-aarch64:** skipped. qemu user mode runs Linux ELF only, so it would need an aarch64-linux build of bench.c
(sysroot + libc) plus a 114 MB qemu-user install. Not "easy"; it stays the correctness-oracle idea from the plan.

**Startup and compile overhead.**
- hello.sim end to end is about 36 ms: Mach-O load 0.03 ms, Jit construction 12 ms, run 23 ms.
- perf shows nearly all of that is one-time dynarmic setup, not guest code: the decode-table build (std::function
  matchers, malloc-heavy, 40% malloc), the per-Jit fastmem fallback and prelude generation (GenFastmemFallbacks,
  xbyak), and the first block compiles.
- The **12 ms is paid per Jit, so per guest thread.**
- JIT compile overhead in the benchmark is small: "first" vs "best" differ by under 0.5 ms per sub-benchmark (memcpy's
  +3 ms first-run delta is page faults and happens natively too).

**Where the time goes.**
- perf: >95% of samples are in JIT code; translation, callbacks and thunks are noise.
- The pattern points at dynarmic's per-block overhead: guest registers live in JitState memory between blocks, and
  every block end spills and reloads.
  - Big straight-line loop bodies are close to native: int 1.47x.
  - Tight loops and call/return pay heavily: fp 6.8x (a ~10-instruction mandelbrot loop is one block per iteration),
    simd 5.4x, calls 3.6x.
  - NEON is a second cost: AdvSIMD float ops go through dynarmic's FPCR/NaN-accurate sequences, which the unsafe
    flags didn't remove.
- This is a hypothesis from the profile shape, not yet confirmed by disassembling blocks (needs a perf map, which
  dynarmic writes to /tmp, or Jit::Disassemble).

### Boundary design (as implemented)

- **Identity mapping.** Segments are mmapped at their link addresses (MAP_FIXED_NOREPLACE). The Jit uses
  `fastmem_pointer = 0` with 47 address bits, so guest loads and stores are host loads and stores. The slow-path
  callbacks are plain host accesses. Exclusives go through dynarmic's global ExclusiveMonitor (shared by all Jits),
  with `fastmem_exclusive_access` and cmpxchg-based MemoryWriteExclusive*.
- **Thunks = patched imports, crossed with SVC.**
  - Chained-fixup binds (DYLD_CHAINED_PTR_64/_64_OFFSET, import formats 1-3) of imported functions resolve to
    8-byte stubs `svc #i; ret` in a thunk page.
  - dynarmic's CallSVC(i) runs native handler i with guest state flushed. The handler reads x0-x7 and the guest SP and
    writes x0 or d0. Execution resumes at the `ret`, so there's no exit from the dispatcher.
  - Data imports (`___stack_chk_guard`) bind to native data. Unknown imports bind to a stub that names the symbol and
    aborts on first call (lazy failure).
  - The runtime never executes svc, so svc is unambiguous; `svc #0xffff` is the "guest returned to host" stub
    (LR of every host-to-guest call). Cost: 293 thunk calls in bench.sim.
- **ABI translation in handlers.**
  - Integer and pointer functions pass straight through (x0-x5 → rdi..r9).
  - Apple arm64 varargs are on the stack in 8-byte slots, and va_list is a plain pointer, so printf, vprintf,
    snprintf and vsnprintf share one slot-walking formatter.
  - Struct layout differences are handled per function. Example: Darwin's 64-byte pthread_mutex_t with static sig
    0x32AAABA7 holds a lazily initialized glibc mutex at offset 8.
  - mach_absolute_time is in ns with a 1/1 timebase. CNTVCT runs at 24 MHz.
- **Threads.** pthread_create starts a host pthread that builds its own GuestThread (Jit + 8 MB guest stack) and
  calls the start routine with LR = exit stub. One Jit per guest thread; `thread_local current`.
- **LSE / LDAPR.** These are undecodable in dynarmic, so they reach InterpreterFallback, where `lse.cpp` performs them
  as host `__atomic` ops on the identity-mapped address and sets PC+4. The ops covered are LDADD/CLR/EOR/SET/SMAX/
  SMIN/UMAX/UMIN and SWP (all sizes), CAS (all sizes), CASP (32/64, cmpxchg16b) and LDAPR. This is correct and
  interoperates with exclusives (both are cmpxchg on the same memory). Cost: about 28 ns per op (3.03M ops in
  threads.sim in about 85 ms).

### Gaps found

1. **LSE, CASP, LDAPR (and LDAPUR/STLUR, RCPC2) are not in dynarmic's A64 decoder.** Emulated through
   InterpreterFallback for now. Stage 1 should add frontend decoders that emit IR mapping to `lock xadd`/`cmpxchg`/
   `xchg`. This needs a designgears fork of dynarmic. LSE is everywhere in simulator code: clang targets apple-m1.
2. **No callback into guest code from native code on the same thread** (qsort comparators, blocks, dispatch,
   atexit). Nested Jit::Run on a running Jit isn't allowed, so stage 1 needs a re-entrant scheme: a second Jit per
   thread for nested calls, or a host-to-guest trampoline that saves and restores Jit state. This is the main
   boundary design item.
3. **Signals:** none handled. Host faults in JIT code (beyond fastmem's own recovery) aren't translated into guest
   contexts; there's no sigaltstack or guest signal delivery.
4. **Per-Jit startup 12 ms** (fastmem fallbacks and prelude per instance) plus about 20 ms of one-time decoder
   setup. With hundreds of threads in a UI process, share the generated prelude or pool Jits.
5. **Code cache per Jit** (64 MB reserved each, translations not shared across threads). A shared and persistent
   translation cache is a stage-3 item, and becomes necessary early with many threads.
6. **FP/NEON speed:** 4-7x slower than native; the unsafe flags don't help. Investigate the emitted code before
   stage 3.
7. **Not exercised yet:** SHA/PMULL/SHA3 crypto, FRINTTS, DIT, MTE (libsystem_malloc), TPIDRRO_EL0 (the runtime has
   none; tpidr_el0 points at a per-thread host slot), DC ZVA (dczid 4 = 64 B), unwinding through JIT frames.
8. **arm64run's libSystem is about 30 glibc-backed functions.** Stage 1 replaces it with Darling's x86
   libsystem_kernel/platform/pthread behind the same svc boundary. memcpy/memset/str* stay native, since
   libsystem_platform is ours. That is why memcpy shows 1.0x.

### Recommendation for stage 1

- Keep the design: identity mapping + fastmem, one Jit per thread, svc-stub thunks at the import binds. The boundary
  costs nothing measurable, and raw JIT speed is about 2.0x the M1 and 2.45x native x86 on mixed code.
- Order of work:
  1. Fork dynarmic (designgears, private) and add LSE/LDAPR/RCPC to the frontend.
  2. Load dyld_sim + the prebuilt cache in mldr and bind the cache's libsystem_kernel/platform/pthread placeholders
     to x86 thunks (svc stubs), then grow the native libsystem from Darling's.
  3. Design re-entrancy (native → guest callbacks) before any ObjC/Swift/dispatch code runs; (c) is the
     acceptance test.
- **Design change the numbers suggest: native hot paths matter more than expected.**
  - Tight loops and calls are 3-7x slower under dynarmic, while thunked libsystem code is free (memcpy 1.0x).
  - So beyond the three mandatory libraries, move objc_msgSend's cache lookup, malloc/free, memmove/str*,
    swift_retain/release and the libdispatch fast paths to native earlier (stage 1-2, not stage 3).
  - Start the AOT/LLVM shadow cache investigation in parallel: dynarmic's block-at-a-time register model caps
    tight-loop performance.
