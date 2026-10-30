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

- [joshuaswarren/metal2vk](https://github.com/joshuaswarren/metal2vk) (MIT): Metal Shading Language compute kernels (source, not AIR) → Vulkan SPIR-V via clang + a metal_stdlib shim + patched clspv; no graphics pipelines or Metal API. A reference for compute features in our AIR path (simdgroup_matrix, cooperative matrix, Metal 4 tensor ops) and a possible compute test corpus.
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

## Stage 1 results (2026-10-29)

**What runs.** Inside Darling on the Ryzen 7 9800X3D, `tests/arm64/run.sh <binary>` (own prefix
`$X86SIM_SCRATCH/a64prefix`, so a booted x86 simulator is untouched) runs the stage-0 test binaries through mldr ->
`host/arm64host` -> `libarm64jit.so`, with the iOS 27.0 runtime's own `dyld_sim` and its prebuilt shared cache:
- (a) `hello.sim`: output correct, exit code 7.
- (b) `bench.sim`: all results identical to stage 0; total 1429 ms (stage 0: 1438 ms).
- (d) `threads.sim`: all counters correct (native pthreads, one Jit per thread, LSE now translated by the fork).
- (c) `objcswift.sim`: libobjc + libswiftCore from the cache, all results correct (ObjC msgSend dispatch, alloc/release,
  Swift generics, existentials, ARC, Dictionary).

Startup: stub dylibs 11 ms, setup 23 ms, `_dyld_sim_prepare` done at 150-165 ms (about 9.3k thunk calls).

### Design as built
- **mldr** (darling, `x86code`): an arm64-only Mach-O makes mldr re-exec itself on `arm64host` (installed next to mldr
  with `libarm64jit.so`, `linux/arm64jit/install.sh`). It renames `DYLD_*` to `X86CODE_A64_DYLD_*`, sets
  `X86CODE_A64_EXE`/`X86CODE_A64_JIT`, reserves 0x1C0000000 (+4 GB) and registers the arm64 program as the executable
  path with darlingserver (proc_pidpath, platform 7). libsystem_kernel (darling-xnu) treats processes with
  `X86CODE_A64_EXE` as foreign-libSystem/simulator processes (kevent workqueues, workloops, kern.osrelease 25.0.0,
  which `libsystem_sim_kernel` requires to be >= 23).
- **arm64host** (x86_64 Mach-O, built on Linux with Darling's toolchain): normal Darling startup, then dlopens the ELF
  library via `elf_calls` and passes Darwin's dlopen/dlsym/dladdr/__error.
- **libarm64jit**: maps the main executable and `dyld_sim`, writes a 16K arm64 commpage at 0xFFFFFC000, builds
  KernelArgs below argv and calls `_dyld_sim_prepare` with a version-18 SyscallHelpers table of svc thunks (open/stat
  redirect the three host-library paths to our stubs; amfi allows everything). dyld_sim maps the cache itself.
- **Stub dylibs**: generated per launch into memfds from the cache placeholders' export tries (kernel 1477 functions /
  28 data, platform 157/6, pthread 204/3; 251 names have no Darling implementation and return -1/ENOSYS with a one-time
  log). Functions are `svc #i; ret`. Data exports are *copies* in the stub's `__DATA` (dyld adds the image base even to
  absolute exports when it patches cache clients); `vm_page_*` are 16K; `_os_alloc_once_table` starts empty.
  setjmp/longjmp family are emitted as arm64 code. `__libkernel_init`/`__pthread_init`/`__libplatform_init` are no-ops.
- **Boundary**: generic handler passes x0-x7 (+ stack slots) and d0-d7 straight to SysV (`callconv.S`); variadic
  (open/fcntl/ioctl/syscall/..., fixed-arg table), bool results masked, 64-bit-inode names (`stat$INODE64`), packed
  stack args for >8-arg VM calls, `_simple_*` formatters, `__sigaction`, pthread_create/once/atfork/key, workqueue
  setup, os_once -> per-function native trampolines (`guest_callable`, cached, reversible).
- **Guest memory**: anonymous `mmap`/`vm_allocate`/`vm_map`/`vm_remap` "anywhere" requests are placed 16K-aligned (or
  the requested mask) first-fit in [12 GB, 63 GB): xzone malloc asserts that segments are below 64 GB.
- **Re-entrancy**: dynarmic fork `Jit::Run` has a depth counter; nested calls save/restore the full guest state, start
  128 bytes below the guest SP, and only the outermost run frees code.
- **Time**: CNTVCT counts ns (CNTFRQ 1 GHz) so it agrees with Darling's mach_absolute_time (timebase 1/1).
- Debug aids: `X86CODE_A64_TRACE=1` (every thunk call), `X86CODE_A64_STEP=1` (single-step, exact fault PCs),
  `X86CODE_A64_STATS=1`, a fault reporter with the guest PC, `tools/debug/dscsym.py` (symbolize/disassemble cache
  addresses).

### Performance (objcswift, best of 3, ms)
| test | Mac M1 native | stage 1 JIT | ratio |
|---|---:|---:|---:|
| msgSend 20M | 53.1 | 134.0 | 2.5x |
| objc alloc/release 2M | 40.7 | 645.3 | 15.9x |
| generic sum Int / Double | 0.68 / 2.0 | 2.0 / 5.3 | 3.0x / 2.6x |
| existential 10M | 28.8 | 61.0 | 2.1x |
| ARC list 2M nodes | 92.6 | 887.1 | 9.6x |
| Dictionary 1M | 20.4 | 42.7 | 2.1x |

Pure compute matches stage 0 (2-3x the M1). Allocation-heavy code is 10-16x: malloc/free run as guest code (xzone
malloc under the JIT, plus its atomics); **native malloc was not done in this round** although the plan wanted it.

### Gaps
1. Native malloc/free (patch libsystem_malloc entry points to thunks) and retain/release fast paths: the biggest win.
2. Signals: `__sigaction` installs trampolines, but async signals are not yet deferred to block boundaries and the
   handler sees an x86 ucontext; synchronous faults only get a report.
3. Two write faults per launch on libobjc `__DATA_CONST` (0x22241dxxx) are absorbed somewhere in the host's handler chain
   and execution continues correctly; understand before stage 2.
4. 251 libsystem exports missing in Darling (skywalk/nexus/exclaves/etc.; ENOSYS). `___kernelVersion*`, pthread qos
   encode/decode helpers are stubs.
5. Struct-by-value and >8-argument functions beyond the VM ones are not audited (list being collected).
6. Per-thread Jit cost (12 ms) and per-thread 64 MB code cache unchanged; data-export values are snapshots (fine for
   constants, wrong if Darling changes one later).
7. Blocks passed to native APIs, thread_get_state with arm64 flavors, ucontext functions: not handled.

### Readiness for stage 2 (launchd_sim -> SpringBoard)
The mechanism works end to end: any arm64 simulator binary exec'd inside Darling runs through dyld_sim and the cache,
with Darling's simulator-mode kernel paths. Before booting launchd_sim: native malloc (allocation speed decides UI
feel), async signal deferral, the >8-arg/struct-by-value audit, and Mach exception/crash reporting at guest PCs. Then
start launchd_sim in the a64prefix with `sim.env` pointed at the 27.0 runtime and port the host shims (simfb, simhid,
metalhostd: they are loaded into sim processes and must become arm64 or be thunked like libsystem).

## Stage 2 results (2026-10-29)

**Result: the iOS 27.0 (24A434) home screen renders** (docs/screens/arm64/03-home-screen.png): launchd_sim,
the core daemons, backboardd (window server, simfb display, simhid input), SpringBoard and PosterBoard, every process
an arm64 iOS-simulator binary under libarm64jit, software rendering, iPhone 17 (iPhone18,3, 1206x2622) device type.
About 7 minutes from `cli/libexec/arm64/boot-a64.sh` to the home screen; the stack stays around 5-5.5 GB, close to the
6 GB budget.

Boot: `SUDO_PASS=... SECS=60 A64_KEEP=1 X86CODE_A64_CODECACHE_MB=8 cli/libexec/arm64/boot-a64.sh`, viewer
`SIMFB_RING_LINUX=/dev/shm/x86code-a64-simfb.ring cli/libexec/simview.py var/device-a64/data/framebuffer.bgra 3`.
Separate prefix ($X86SIM_SCRATCH/a64prefix), device (var/device-a64), state (var/run-a64) and frame ring; it stops only
its own container's launchers (found by command line) and never runs killsim.sh, mtlvk, mdns or jetsam.

### Prerequisites (1a-1e)
- **Native malloc (1a).** Every libsystem_malloc export in the mapped cache becomes a thunk when `__libkernel_init`
  runs (libSystem's initializer calls it before `__malloc_init`). 4-byte tail-call exports (`free`, `vfree`,
  `realloc`, `malloc_type_free`, ...) are followed to their internal targets. Backing store is `gheap.cpp`, a size-class
  allocator over the guest VM range below 64 GB: Darling's own malloc returns 0x7f.. addresses and Swift/ObjC pack flag
  bits above bit 36 (Swift's `_StringGuts.grow` crashed on them). Zones are guest-side `malloc_zone_t`s with svc stubs.
  `pthread_getspecific`/`setspecific` are guest code.

  objcswift (best of 3, ms): | test | M1 | stage 1 | stage 2 |
  |---|---:|---:|---:|
  | objc alloc/release 2M | 40.7 | 645.3 | 265-273 (6.6x M1) |
  | ARC list 2M nodes | 92.6 | 887.1 | 286-294 (3.1x M1) |
  | msgSend 20M | 53.1 | 134.0 | 133-137 |
  | existential / Dictionary | 28.8 / 20.4 | 61.0 / 42.7 | 60 / 43 |

  ObjC retain/release stay guest code (inline isa CAS; a native path would need the isa layout).
- **Signals (1b).** `__sigaction` and iOS 27's `__platform_sigaction` install one native handler that records the
  signal and halts the thread's Jit; `GuestThread::call()` delivers at the block boundary with an arm64 Darwin
  ucontext/mcontext64 on the guest stack, sa_mask applied, and resumes from the (possibly modified) context
  (tests/arm64/signals.c).
- **>8 args / by-value structs (1c).** From the audit list: packed stack arguments for exception_raise_state(_identity),
  *_swap_exception_ports, kext_request, mach_msg_overwrite_trap; `task_register_dyld_shared_cache_image_info` copies
  its 40-byte struct to the SysV stack; host_security_* are missing. mach_timespec_t/mach_right_* fit a register.
- **Blocks (1d).** None of the three host libraries' exports takes a block (Haiku audit,
  $X86SIM_SCRATCH/a64s2/block_exports.txt). thread_get_state arm64 flavors and ucontext calls are still not handled.
- **libobjc read-only faults (1e).** dynarmic implemented acquire loads (LDAR/LDAPR) as `lock xadd 0`, a write cycle:
  every acquire load from `__DATA_CONST` (constant CFStrings, objc data) faulted and went through the fastmem fallback.
  Ordered reads up to 64 bits are now plain loads (ordered writes are xchg, so STLR->LDAR order holds).

### Translation and boundary bugs found by the boot
- dynarmic: FEAT_FP16 scalar arithmetic/compares/fcsel (via binary32); FP16 *vector* (fmla/fmul by element, three
  same, conversions; ~70k instructions in the cache, RenderBox/CoreImage) run in `fp16.cpp` from the interpreter
  fallback (tests/arm64/fp16v.c hash identical to the M1); constant propagation no longer folds ops with pseudo-ops
  (`ands x, y, #-1` asserted in IdentityRemovalPass); 64K-entry fast-dispatch table (was 16 MB per thread).
- TBI: slow-path memory callbacks drop the top byte (Swift `_StringObject` pointers with discriminator bits).
- Guest TSD is private (tpidrro_el0 -> own array): Darling's native libobjc/libdispatch TSD destructors freed guest
  values at thread exit (SIGABRT in every daemon). Reserved slots 1-9 (special reply port, QoS, MIG reply, ...) are
  copied across at every guest/native transition (dispatch's sync IPC got MACH_RCV_INVALID_NOTIFY otherwise).
- pthread_exit: guest threads start in our wrapper and unwind to it (`__builtin_longjmp`; glibc's fortified longjmp and
  Darling's pthread_exit aborted); the main thread parks (dispatch_main). Exited threads free their GuestThread
  through a native TSD destructor (each leaked ~20 MB before).
- Hint instructions (YIELD/WFE) continue; mldr's Linux exe path is turned into the Darwin path (CFBundleGetMainBundle
  was NULL); the guest-VM cursor stays 16K-aligned (an unaligned cursor made every later mmap EINVAL: lsd's database
  build failed); special handlers without a native function are "missing" instead of calling NULL.
- Darling: darlingserver `task_info` answers unknown flavors KERN_INVALID_ARGUMENT instead of aborting the server;
  libsystem_kernel caps arm64 processes at 24 workqueue threads (launchd_sim grew to 66 threads).

### Shims (stage 2 item 3)
| shim | decision | why |
|---|---|---|
| simshim (interposers) | arm64 Mach-O (build/mac-build-arm64-shims.sh) | it interposes guest frameworks' functions |
| SimFramebuffer (simfb) | arm64 Mach-O, own ring /dev/shm/x86code-a64-simfb.ring, 1206x2622, `SFBDisplayGetDeviceName` added | loaded by backboardd, calls guest CF/IOSurface; the Linux futex wake is `svc #0xfffe` (libarm64jit's Linux-syscall escape) |
| SimulatorHID (simhid) | arm64 Mach-O | loaded by backboardd, guest IOKit/Foundation |
| iosurfaced, metalhostd, fpsim2host, netconfig, openurl, fseventsd | not built yet; mksimdevice leaves jobs without an arm64 build out (IOSurface emulated in process, software rendering) | they are simulator processes too: arm64 builds next |
| libkqueue_sim | not needed | the guest's libsystem_kernel *is* Darling's, behind the thunks |

Host-side Linux daemons (mtlvk, mdnsresponder, simview/simstream) are unchanged.

### 27.0 interface changes (item 4)
SFBDisplayGetDeviceName is in simfb. Metal (new useInternalResidencySet(s):, genericAddressSpaceDispatchInfo, removed
set*GlobalConstants*/supportsFP8), NearbyInteraction and UniversalHID are not done: the boot renders in software.

### Gaps
1. Memory: each thread has its own Jit (code cache + metadata, ~10-30 MB); processes cost 100-450 MB, the boot runs at
   5-5.5 GB of 6 GB. The lean job set (no chronod widgets, no FileProvider, no CloudKit/accounts daemons) and
   `X86CODE_A64_CODECACHE_MB=8` keep it inside the budget.
2. Speed: ~7 min to the home screen; lsd's database build and SpringBoard's first layout dominate.
3. Intermittent crashes not yet understood: backboardd SIGSEGV calling NULL from a libsystem_kernel thunk (once),
   SpringBoard SIGSEGV in UserNotificationsServer on a freed object (once, before the TSD fixes), lsd SIGABRT in binary
   plist parsing (before the VM cursor fix). Each process's log: /private/var/tmp/x86code-a64/<exe>.<pid>.log in the
   prefix.
4. No Metal, no iosurfaced/metalhostd/fpsim2host arm64 builds, no time zone link (status bar shows UTC), process names
   in launchd's logs are "exe".
5. thread_get_state arm64 flavors, ucontext functions, Mach exception reporting at guest PCs; guest TSD destructors
   don't run at thread exit.

### Stage 3 should start with
Shared translations across a process's threads (one code cache + block map per process, per-thread register state):
it is both the memory problem (per-thread duplicate code/metadata) and a large part of the speed problem (every new
thread re-translates libdispatch/objc/CF). Then arm64 builds of iosurfaced/metalhostd and the 27.0 Metal selectors.

## Stage 3 results (in progress, 2026-10-29)

### Shared per-process translation cache
Committed and pushed (dynarmic `b5134744` on designgears/x86code, top `ef0ac83`):
- One `SharedCode` (BlockOfCode + block map + fast-dispatch) per process; per-thread `A64JitState` for tpidr/exclusives.
- Atomic 16-byte fast-dispatch updates; `X86CODE_A64_SHARED=0` restores per-thread caches.
- Shared cache size floor: `X86CODE_A64_CODECACHE_MB` values below 64 become 256 MB (shared code is never freed).
- **Lock narrowing:** `GetBlock` translates IR unlocked; re-checks the map under the recursive mutex before `Emit`;
  `invalidate_gen` discards stale IR after a concurrent Invalidate. Earlier boots held the mutex across Translate and
  could stall other threads' LookupBlock.

**Sanity (tests/arm64, via `tests/arm64/run.sh`):**
| test | shared | noshared (`SHARED=0`) |
| --- | --- | --- |
| threads.sim | pass; new Jit +0–8 kB | pass; new Jit +18–21 MB |
| objcswift.sim | ~3.0 s, identical results | ~3.1 s, identical results |

### Boot-to-home measurement
Stage 2 baseline: ~7 min, 5.4–5.9 GB (cgroup; guests actually escape it), `X86CODE_A64_CODECACHE_MB=8`, per-thread caches.

| mode | boot-to-home | sum guest RSS at home / end | notes |
| --- | --- | --- | --- |
| Stage 2 (per-thread, 8 MB) | ~7 min | 5.4–5.9 GB (cgroup) | software home, black wallpaper |
| **SHARED=0, 8 MB** (control) | **372 s (~6.2 min)** | **~9.1 GB** (sum VmRSS) | home OK; docs/screens/arm64/04-noshared-home.png |
| shared (256 MB floor, narrow lock) | **FAIL** (~9 min black) | ~6.2 GB while stuck | SB crash-loop ~30 s then one instance idle on black frames |

Shared-cache boot failure modes seen:
- SpringBoard `brk #0` in BackBoardServices near `_BKSRestartActionOptionsDescription` (pc `0x1c8e3e420`).
- SpringBoard SIGSEGV at libobjc / thunk boundary (`guest pc` in cache, `in thunk -`).
- backboardd / PosterBoard SIGSEGV in `dyld_sim+0x234f0` during binding (same family as stage 2 NULL-thunk notes).
- After lock narrowing, one SB instance can survive 5+ min at ~670 MB RSS / low CPU with seq advancing but
  `bright≈0` (identical black grids) — UI never paints.

Conclusion: shared cache is correct for the microbenchmarks; the full SpringBoard multi-thread translate/link path
still misbehaves. Default boot remains `X86CODE_A64_SHARED=0` until that is fixed. Live sim left at home on
`SHARED=0` / `CODECACHE_MB=8`.

### Small fixes landed
- Time zone: `boot-a64.sh` links `/private/etc/localtime` to the host zone (verified `America/Denver` in the prefix).
- Process names: Darling `mldr` `PR_SET_NAME` (darling `af50adbba`); `/proc/.../comm` shows `SpringBoard`.
- `framecheck.py`: only runs CLI `main` when `__name__ == "__main__"` (importing it used to `sys.exit` and kill waiters).

### Commits
- dynarmic `73c75bf7` — shared translation cache; `b5134744` — narrow GetBlock mutex / invalidate_gen
- darling `af50adbba` — mldr `PR_SET_NAME` (darlingserver memory.c/h left dirty, separate item)
- top `ef0ac83` / `9481b78` — submodule pointer, framecheck import fix, Stage 3 docs

### Cgroup containment (MemoryMax)
Root cause: `boot-a64.sh` ran `darling shell ln … localtime` *before* `limited.sh`, so darlingserver was born in the caller's cgroup (T3 ACP) and every guest forked from it escaped `MemoryMax`. Only the `timeout`+`darling` launcher stayed in `x86sim-*.scope` (`memory.current` ~400 kB while sum guest VmRSS was ~9 GB).

Fix: TZ link + simhost/launchd start both run inside `limited.sh` via `cli/libexec/arm64/boot-inner.sh`; a join loop moves darlingserver and `X86CODE_A64` guests into the scope's `cgroup.procs`. `limited.sh` sets `Delegate=yes`. Default `X86SIM_MEM_MAX` for arm64 is **10G** (was 6G based on the false cgroup reading; real SHARED=0 home needs ~9 GB). Drop back toward 6–8G once shared cache works.

Verification (SECS=55, A64_JOBS=min, SHARED=0, MEM_MAX=10G): darlingserver IN; guests IN=31 OUT=0; `memory.current`≈904 MB / `memory.max`=10 GB.

### Contained SHARED=0 home (noshared8)
Recipe: `X86CODE_A64_SHARED=0` `CODECACHE_MB=8` `X86SIM_MEM_MAX=14G` std jobs + PosterBoard, device wiped.
- **Home at 199 s** (seq 6→43, bright 0.2097 / dark 0.7414). Screenshot: `docs/screens/arm64/05-noshared-home-contained.png`.
- Containment: scope `x86sim-2352419`, guests IN=34 OUT=0, darlingserver IN.
- At home: sum guest VmRSS **8382 MB**; cgroup `memory.current` **~3500 MB** (shared pages once); `memory.max`=14G.
- Faster than the earlier uncontained 372 s control (same recipe, host less loaded / warmer caches).
- Note: `memory.current` under-counts host pressure; boot waiters also abort if sum VmRSS > ~14 GB.

### Shared-cache boot: thunk table race (fix in 2bb1649)
Full boot under `SHARED=1` (and even `SAFE=1`) SIGSEGVed in `GuestThread::CallSVC` at
`call *0x20(%rax,%r14,1)` — same host rip offset `…c90ae` in SpringBoard, PosterBoard, and
WidgetRenderer. `g_thunks` is a `vector` that `add_thunk` grows during dyld stub binding with no
synchronization; a concurrent `CallSVC` can use a reallocated `data()` pointer.

Fix (`linux/arm64jit`): `reserve(kMaxThunks)` so `data()` never moves; mutex around `push_back`;
publish length via `std::atomic` release/acquire (`thunk_count()`). CallSVC bounds-checks against
the published count. Microtests (sharestress) already passed; re-measure shared boot-to-home next.


### Shared-cache boot after thunk + IC fixes (still open)
Commits on top `main`: `2bb1649` (thunk publish race), `ba741aa` (IC invalidate + RSB off under shared).

| run | result |
| --- | --- |
| sharedsafe (SAFE=1, before thunk fix) | CallSVC SIGSEGV + BKS |
| shared6 (thunk fix only) | CallSVC SIGSEGV **gone**; BKS crash-loop every ~2 min |
| shared7 (thunk + IC + no RSB) | one BKS then **idle black** ~12 min (seq stuck, bright≈0.014, SB ~1.6 GB) |

CallSVC race was real (same rip `…c90ae` across SB/PosterBoard/WidgetRenderer) but not sufficient for home.

### Shared ClearCache + BlockLinking → home (shared9)
BKS `brk #0` at `0x1c8e3e420` is `_BKSRestartActionOptionsDescription+0x18a0` → assert
`failure in … _connectionLock_connectToRemoteServiceIfNeeded` (XPC connect). Registers from
shared7: x0 tagged ptr, x1=`0x8000100`. It fired under shared when IALLU stalled every thread.

**Root causes (two, both required):**
1. **Shared `ClearCache` (IALLU) walked every block** via `InvalidateCacheRanges([0,~0])` under the
   process mutex and left `block_ranges` populated (dynarmic TODO). After minutes of SpringBoard that
   stalls all guest threads for seconds → BKS/XPC assert. Fix: `emitter.ClearCache()` (O(1) map/FD
   clear); erase invalidated intervals from `block_ranges`; atomic 16-byte FD clears.
   dynarmic `3a31a903`.
2. **BlockLinking was off under shared.** Without linking, SB burns billions of dispatcher lookups on
   a few thousand blocks and never paints (idle black, bright≈0.014). Atomic 8-byte patch heads make
   linking safe; default is now **on** (`X86CODE_A64_LINK=0` to disable).

| run | result |
| --- | --- |
| shared8 (ClearCache fix, linking still off) | **no BKS**; still idle black (lookup storm) |
| **shared9 (ClearCache + LINK=1)** | **HOME 186 s**; bright 0.20; sum VmRSS 8.4→9.4 GB; PSS≈4.6 GB = memory.current; OUT=0 |

Screenshot: `docs/screens/arm64/06-shared-home.png`. Bisect knobs: `FASTDISPATCH`, `IVAU_CLEAR`,
`EXEC_LOCK`, `ICLOG`. Reproducer source: `tests/arm64/icstress.c` (needs Mac rebuild of `.sim`).

**Default boot is now shared** (`X86CODE_A64_SHARED` unset). `MEM_MAX` stays **12G**: settled sum
VmRSS ≈9.4 GB (PSS/cgroup ≈4.6 GB); 6–8G would OOM the sum-RSS guests. PosterBoard / chronod /
FileProvider still deferred (+several GB).

### Slim reclaim + cold-boot stabilizers (2026-10-29 evening)
After SHARED=1 home, Stage 3 focused on memory attribution and process inventory.

**smaps attribution (baseline at home, before reaper):** SpringBoard PSS ≈1.28 GB dominated by
`anon_host` (~585 MB), `dyld_TEXT` (~184 MB), `jit_code` (~158 MB), `host_heap` (~140 MB).
Spotlight alone was +630 MB PSS / ~1.0 GB RSS; WidgetRenderer similar. sleepd and
usernotificationsd ~400–450 MB RSS each under the JIT.

**Fixes landed:**
- `mksimdevice.py`: `com.apple.sleepd` in EAGER (slim-by-default was stripping RunAtLoad; under
  on-demand-only `launchd_sim` sleepd never started and cold boots hit BKS/XPC asserts). Optional
  `X86CODE_DELAY_SB=1` drops SpringBoard RunAtLoad.
- `boot-a64.sh`: `A64_JOBS=slim` (UI without sleepd/usernotificationsd); default `A64_SLIM=1`
  kills Spotlight / WidgetRenderer / pkd once after boot; `A64_KEEP` starts `slim-reaper.sh`
  (5 s interval — they grow to 400–600 MB RSS in ~15 s).
- `framecheck.py`: leading `.ring` path so monitors read `/dev/shm/x86code-a64-simfb.ring`
  (default fbring path is the x86 ring; that made a64 boots look stuck on a stale frame).
- `host.cpp`: SIGABRT breadcrumb → `/home/bobby/scratch/x86code/abrt-PID-TID.txt` (RIP + maps +
  guest TLS present/absent). Helps when per-pid logs truncate.
- `icstress.sim`: built and **pass** (`hits 48000 iallus 240`); binary kept in
  `scratch/x86code/arm64/bins/` (source already in tree).

**After slim (std jobs + reaper, live boot-abrt):** PSS ≈3.7 GB / cgroup ≈4.4 GB / sum VmRSS ≈7.4 GB.
Top: SB 1.39 / BB 0.38 / iconservices 0.29 / sleepd 0.19 / usernotificationsd 0.18. Screenshot:
`docs/screens/arm64/07-stage3-home.png` (bright≈0.21, dark≈0.74, home_exit=0). simview on
`SIMFB_RING_LINUX=/dev/shm/x86code-a64-simfb.ring` DISPLAY=:0.

`A64_JOBS=slim` would drop sleepd+usernotificationsd (~800 MB RSS more); not the default yet
(cold-boot BKS stability). Goal remains ~1 GB real for one app — SB alone is still ~1.4 GB PSS
(`anon_host` dominant; code-cache / guest heap work next).

### Slim park replaces reaper (2026-10-29 night)

**Who spawns Spotlight / WidgetRenderer:** SpringBoard via RunningBoard as
`UIKitApplication:…` ("launch job demand"), not LaunchDaemon `RunAtLoad`. Widgets in
`IconState` / `DesiredIconState` pull WidgetRenderer; Spotlight is the home-screen search host.
Killing them every 5 s (`slim-reaper.sh`) made RunningBoard relaunch within ~1 s and burned a
core in `xpcproxy_sim` while never keeping memory down.

**Clean stop (default slim profile):**
1. **IconState widget strip** in `boot-a64.sh` (unless `A64_KEEP_WIDGETS=1` or `A64_JOBS=full`) —
   drops widget / emptied custom stacks so SpringBoard demands fewer WR launches.
2. **slim-park** in `linux/arm64jit/host.cpp`: before dyld/JIT, match `X86CODE_A64_SLIM_PARK`
   (or file `/home/bobby/scratch/x86code/a64-slim-park`) path substrings and `pause()` forever.
   Process stays alive → RunningBoard assertions satisfied; RSS stays ~12 MB. launchd does **not**
   pass `sim.env` into job environ, so the file fallback is required.
3. Boot end reports parked helpers via `wchan=*pause*` + tiny RSS (no more reaper start).
   `A64_SLIM=0` / `A64_JOBS=full` clears the park file (full profile).

**Cold-boot note:** park from t=0 with widget strip can leave a status-bar-only frame
(`bright≈0.06`) on some boots (BKS). Reliable recipe used here: reach full home with park
file absent (`A64_SLIM=0`), then write the park file and `SIGKILL` fat Spotlight/WR so they
respawn parked. Subsequent boots with park+strip still need hardening.

| metric | before park (fat helpers) | after park (live enable) |
| --- | --- | --- |
| whole-sim PSS | 6096 MB | **3889 MB** |
| cgroup `memory.current` | 6148 MB | **3954 MB** |
| sum guest VmRSS | 11167 MB | **7964 MB** |
| Spotlight | 2024 MB PSS | absent |
| WidgetRenderer | 388 MB PSS | **~12 MB parked** (`ia32_sys_pause`) |
| SpringBoard PSS | 1248 → 1387 MB | (SB itself unchanged) |
| idle (3 s) | ~286% core (Spotlight+launchd+securityd) | **~190%** (launchd_sim ~103%, securityd ~83%) |
| frame | bright≈0.21 | bright≈0.21 retained |

Screenshots: `10-stage3-home-before-park.png`, `11-stage3-home-after-park.png`,
`12-stage3-slim-park-home.png`.

**SpringBoard smaps attribution (still open — park does not shrink SB):**

| bucket (PSS MB) | before (home, fat) | after park |
| --- | --- | --- |
| anon_host | ~673–685 | ~685 |
| host_heap | ~139–140 | ~140 |
| other (incl. dyld / JIT / guest) | ~433–558 | ~558 |

`anon_host` is dominated by large glibc `mmap` blobs + JIT metadata (largest single VMA ~448 MB).
A page-map `block_ranges` replacement was built then **reverted** (launchd_sim SIGSEGV under the
new map). Guest stacks stay **8 MB** (1 MB previously crashed SB). IR-free / compact maps /
`madvise` of freed guest heap remain next.

### Idle CPU fix (2026-10-29): cloudd + diagnosticd in MIN

**Root cause:** after slim-park, idle home still burned ~1.9 cores. strace + logs:

- `securityd` wrote ~250 log lines/s: `daemon error while acquiring session … com.apple.cloudd … error 3`
- `launchd_sim` served a tight failed-lookup storm for `com.apple.cloudd` and `com.apple.diagnosticd`
  (neither was in the arm64 MIN LaunchDaemons set)
- `SIGSTOP securityd` → launchd dropped to ~0.3% (fully coupled)

**Fix in `cli/libexec/arm64/boot-a64.sh`:** add `com.apple.cloudd` and `com.apple.diagnosticd` to
MIN (on-demand MachServices). Park list stays `Spotlight.app,WidgetRenderer` only — parking a
MachServices daemon accepts the lookup then hangs clients forever (observed: SB stuck at 2 threads,
no ring paint).

| metric | before (parked home) | after (reboot2, park from t=0) |
| --- | --- | --- |
| launchd_sim | ~103% | **~0%** (5 s jiffies) |
| securityd | ~83% | **absent / idle** (no lookup storm) |
| whole-sim idle (3–5 s) | ~190% | **~1.3%** (SpringBoard only) |
| whole-sim PSS | ~3.9 GB | **~3.7 GB** |
| cgroup `memory.current` | ~3.95 GB | **~4.25 GB** (diagnosticd live ~182 MB) |
| frame | bright≈0.21 | **bright≈0.21**, icons+dock (full home) |

Screenshot: `13-stage3-idle-cpu-fix-home.png`. Scratch: `/home/bobby/scratch/x86code/idle-cpu/`.

**Cold-boot note:** this same reboot2 (park+strip from t=0, plus the two new MIN jobs) landed full
home — previously park-from-t=0 was flake-prone toward status-bar-only. Still treat as improved,
not proven across N boots.

**SpringBoard smaps (after idle fix, before page-map):** anon_host ~686 MB (largest VMA **448 MB**
rw-p), host_heap ~138 MB, JIT code cache 256 MB VMA (~155 MB PSS rwxp).

### Page-map `block_ranges` re-land (2026-10-29)

Replaced `boost::icl::interval_map` + per-interval `std::set` with page-indexed
`tsl::robin_map` (`page_blocks` / `block_pages`). Unit test
`/home/bobby/scratch/x86code/stage3/test_block_range` OK (narrow IVAU, wide IALLU, dedupe).
dynarmic `fa4b9619`. Cold boot with park+page-map reached full home (bright≈0.21).

| SB bucket (PSS MB) | interval_map (idle-fix home) | page-map home |
| --- | --- | --- |
| anon_host | 686 | **763** (448 MB VMA still present) |
| host_heap | 137 | **79** |
| whole-sim PSS | 3713 | 3790 |
| glibc mallinfo2 mmapped | ~652 MB / 11 blocks | ~652 MB / 11 blocks (settled) |
| idle | ~1.3% | ~1.3% |

**Finding:** the 448 MB anon VMA is **not** cured by swapping `block_ranges` alone. SIGUSR1
memlog shows it inside glibc `hblkhd` (large mmap chunks). `host_heap` did drop (~58 MB).
Next attribution: log SharedStats (blocks/code MB) on SIGUSR1 (landed in arm64jit) and size
the other dynarmic maps (`block_descriptors`, `fastmem_patch_info`, page_blocks entry counts).

Screenshot: `14-stage3-pagemap-home.png`.

**Cold boot:** park+strip from t=0 reached full home on both reboot2 (idle-fix) and the
page-map reboot — improved vs the earlier status-bar-only flake; still want a few more reps.

### Compact patch + fastmem maps (2026-10-29, round 3g)

Fingerprinted the “448 MB” SpringBoard anon VMA as **two adjacent glibc `mmap` chunks**
merged by the kernel:

| chunk | owner | ~size before |
| --- | --- | --- |
| A | `tsl::robin_map` buckets for `fastmem_patch_info` (56 B/entry × ~1.5M) | ~224 MB |
| B | `tsl::robin_map` buckets for `patch_information` (value held 4×`std::vector` ≈96 B) | ~224 MB |

The “144 MB” VMA was `block_descriptors` (~64 MB) + `block_pages` (~80 MB); “56 MB” ≈
`unlinked_heads`. Confirmed by walking `SharedCode*` pointers into the large VMAs after
SIGUSR1 memlog + SharedStats (~885K blocks, ~152 MB code).

**Fix (dynarmic `11608843`):**

1. `PatchInformation`: four site vectors → one tagged `sites` vector (`Kind` + `CodePtr`,
   96 B → 24 B per value) → patch map bucket array ~224 → ~80 MB.
2. `FastmemPatchInfo`: keep only `resume_rip` + `callback`; move `marker` /
   `recompile` into `fastmem_recompile_markers` (empty under our default
   `recompile_on_fastmem_failure=false`) → ~224 → ~128 MB.
3. `ClearCache` clears the side map.

Cold boot with park+strip+compact maps reached full home (bright≈0.21, icons+dock).

| metric | before (page-map home) | after (compact maps) |
| --- | --- | --- |
| SB PSS | ~1.4 GB | **~1.18 GB** |
| SB anon_host PSS | ~763 MB | **~477 MB** |
| largest anon VMA | 448 MB | **288 MB** (merged slim fastmem ~128 + compact patch ~80 + block_pages ~80) |
| glibc mallinfo2 `hblkhd` | ~652 MB / 11 blocks | **~413 MB** / 12 blocks |
| host_heap (smaps) | ~79 MB | ~86 MB (unchanged class) |
| whole-sim PSS | ~3.8 GB | **~3.56 GB** |
| SharedStats | — | ~885K blocks, ~152 MB code |

Screenshot: `15-stage3-compact-maps-home.png`. Scratch: `/home/bobby/scratch/x86code/stage3g/`.

**Whole-sim PSS by category (after compact, home):**

| category | PSS MB | share |
| --- | --- | --- |
| anon_host_jit_meta (dynarmic maps) | 1642 | 46% |
| dyld_cache (clean+dirty) | 726 | 20% |
| jit_code (rwxp code cache) | 463 | 13% |
| host_heap | 352 | 10% |
| mapped_libs_files | 315 | 9% |
| other / arm64jit.so / stacks | ~68 | 2% |
| **TOTAL** | **3566** | |

Top processes: SpringBoard 1178 MB (anon_host 477 / dyld 378 / jit 156), backboardd 368,
iconservicesagent 248. Same JIT-meta pattern in every translated process — the remaining
~2.5 GB to the 1 GB goal is mostly more of these maps + dyld + code cache across the job set.

**PosterBoard:** left out this round. Prior notes: LaunchAngel wallpaper path blows RSS
(extensions / PRSService / same JIT-meta tax). Restoring it before another ~200–400 MB of
JIT meta is cut would push whole-sim further from 1 GB. Next pass: apply the same map
compaction wins process-wide, then trial-enable PosterBoard with a before/after PSS.

### Input fix (2026-10-29, this round)
Arm64 touch was broken after the socket path started delivering events: `BKDisplayConvertCAScreenLocationToContext`
got `displayUUID=(null)` for every digitizer event. `BKIOHIDService _setUpInitialProperties` copies the HID
property `displayUUID` into its ivar; set it to `x86Code-main-display` (same string as `SFBDisplayGetDisplayUID`).
Also: sync `_IOHIDServiceDispatchEvent` on the socket thread (async was a silent no-op under the JIT), mutex
instead of `dispatch_sync` for `handle_line`, and `simtouch`/`simview` read 1206×2622 from the frame ring.
Verified: Settings tap launches `com.apple.Preferences` with no new null-displayUUID convert errors.

### Still open (stage 3 remainder / next rounds)
1. **SPEED first (next):** Metal→Vulkan on the GPU; native x86 hot paths (`objc_msgSend`, retain/release,
   memcpy/string); ahead-of-time translation of the shared cache. Faster runs unblock later memory work.
2. Further shrink JIT meta (deferred this round — incomplete dynarmic header WIP was reverted; remaining
   288 MB SB VMA = fastmem+patch+block_pages; sparse `block_descriptors` / drop unused unlinked_heads;
   free IR after emit; `madvise` freed guest heap). Goal gap: ~3.56 GB → under 1 GB.
3. PosterBoard extensions RSS bomb; restore chronod/FileProvider when leaner.
4. A few more cold-boot reps with park+strip to call reliability done.
5. Persistent on-disk translation cache; trim the early shared boot spin.
6. Guest TLS destructors on every exit path.
7. `X86CODE_A64_PROFILE` / `MEMLOG` still correlate with early `launchd_sim` SIGABRT — leave off for boots.

## Speed 1: Metal on the GPU (2026-10-29)

Goal: CoreAnimation / apps via Metal→Vulkan on the RTX 5090 on the arm64 path.

### Per-component
| Piece | Decision |
|---|---|
| mtlvk (linux/mtlvk) | native x86_64 Linux, unchanged; per-boot socket `var/run-a64/mtlvk.sock` |
| iosurfaced | arm64 Mach-O under JIT (`host/iosurfaced/arm64/`) |
| metalhostd + vkmtl | arm64 Mach-O under JIT (`host/metalhostd/arm64/`); heavy work in mtlvk |
| mtlprobe | arm64 (`tests/metal/arm64/`) |
| simshim / simfb / simhid | already arm64 |

### iOS 27 Metal interface
vkmtl stubs: `supportsPVRTC`, `supportsRGBTextureBuffers`, `supportsTextureMultifetch`, `supportsMXU*`,
`useInternalResidencySet(s):`, `genericAddressSpaceDispatchInfo`, `compressionMetadata`, `forwardProgressUsage`,
`recommendedPersistentThreadgroupsPerGrid…`. Removed client selectors (`set*GlobalConstants*`, `supportsFP8`) fall
through. MTLCompiler moved out of the dyld cache onto disk as `Metal.framework/XPCServices/MTLCompilerService.xpc`.

### What works
- `MTLCreateSystemDefaultDevice` → MTLSimDriver → metalhostd → mtlvk → RTX 5090 (~2.7 s cold).
- Buffers, textures (replace/getBytes), private render targets, GPU clear + blit readback, fillBuffer, shared events,
  100 command buffers in order (~35–40 ms).
- Fixes that unblocked the hang: FEAT_FP16 `SCVTF/UCVTF Hd,Wn` interpret (`1ee20184`); metalhostd `CFRunLoopRun`
  instead of `dispatch_main` parking; simshim skips `enable_sim2host` for metal (+ null name when `X86CODE_METAL_IN_SIM`).
- **Speed 1b:** compute/render pipelines + `newLibraryWithSource` (43/0); real iosurfaced; CA Metal presents on GPU.

### Speed 1b (same day): MTLCompiler + iosurfaced + CA Metal
- **MTLCompilerService:** iOS 27's `Metal.framework` executable is cache-only; without an on-disk `Metal` binary,
  `xpc_bundle` cannot resolve `XPCServices/MTLCompilerService.xpc` and lookups fail with error 3. Fix:
  `cli/libexec/arm64/ensure-metal-framework.sh` extracts Metal from the dyld cache into the framework (`runtimes/`
  gitignored). Do **not** install a LaunchDaemon for the label (create-copy 137). Probe then: **43 PASS / 0 FAIL**
  (compute + render pipelines, `newLibraryWithSource`, AIR→SPIR-V via mtlvk). Cold first pipeline ~4 s under JIT.
- **iosurfaced hang:** `dispatch_main` parks the A64 JIT main thread so XPC handlers never run; backboardd stuck
  after `simfb: swapchain`. Same fix as metalhostd: `CFRunLoopRun`. Software + real IOSurface publishes frames.
  simshim now skips **all** null-name `enable_sim2host` (iOS 27 enable-before-set_name).
- **CA Metal:** `X86CODE_METAL=backboardd` + `X86CODE_IOSURFACE=1`. MTLCompilerService UUID instances spawn;
  metalhostd builds CA pipelines; simfb presents wait on GPU (`ready_at_submit=0`). Screenshot
  `docs/screens/arm64/31-speed1b-gpu-home.png` (black wallpaper — no PosterBoard; same as software leave-running).

### Metrics (Speed 1b — **content/PNG invalid**; timing numbers superseded by Speed 1c)
| | Software baseline | CA Metal (Speed 1b) |
|---|---|---|
| Scroll fps while moving | ~40 | ~24 (CA-paced ~42 ms) |
| Frame time p50 | 25 ms | 41.7 ms |
| Touch→frame latency p50 / max | ~247 / ~950 ms | **23.6 / 44.1 ms** |
| backboardd CPU while scrolling | ~102% | **~10%** |
| Total CPU while scrolling | ~123% | **~25%** |
| Idle: frames / 12 s | few | 293 (~24 fps continuous) |
| Idle: backboardd | low | ~6.7%; SpringBoard ~160% (gap) |

Probe GPU clear ~100 ms first CB; 100 CBs ~35–42 ms once the device is up. Scroll GPU wait age ~2–6 ms (CA late).

### Gaps after Speed 1b (historical; see Speed 1c)
1. Home PNG looked corrupt — **was screenshot stride**, fixed in Speed 1c.
2. Idle SpringBoard ~160% — **gone** in Speed 1c idle (few frames, SB ~2%).
3. Scroll fps 24 — superseded by Speed 1c page-swipe ~49 fps.
4. metal2vulkan: no AIR changes required for iOS 27 probe/CA shaders used so far.

### Commits
- `dff59a5` scaffolding (iOS 27 selectors, arm64 shims, mtlvk boot)
- `dddbffd` FP16 SCVTF/UCVTF + metalhostd RunAtLoad
- `3fce907` CFRunLoop + sim2host null-name / METAL_IN_SIM
- `dae5b4e` docs Speed 1 + progress.json
- `77cd92f` ensure-metal-framework + iosurfaced CFRunLoop + simshim null-name skip
- `11cc869` arm64 iosurfaced/metalhostd/mtlprobe Mach-Os
- `801df25` docs/progress Speed 1b + GPU home milestone entry
- `ad88b07` force-add SW + GPU home screenshots (`30` / `31`)


### Speed 1c (same day): stride truth + correct GPU home
- **Root cause of "corrupt" 31-speed1b-gpu-home.png:** screenshot/save bug, not GPU write. The ring and
  IOSurface use bytesPerRow **4864** (64-byte aligned) for width 1206 (tight row = 4824). Decoding with
  `width*4` shifts every row by 40 bytes → thin diagonal lines. Live ring pixels were always correct
  (pad bytes zero; correct-stride slope 0).
- **x86 1179-wide:** tight 4716 vs align64 4736 (pad 20). Shared stride code that always takes
  `bytesPerRow` / ring stride stays correct for both widths; do not assume `width*4`.
- **Fixes / probes:**
  - `cli/libexec/fb2png.py`: refuse buffers shorter than `stride*h`; always pass real stride into Pillow;
    print tight vs stride.
  - `linux/mtlvk` `selftest()`: 1206×900 BGRA IOSurface (bpr 4864), VRAM-proxy clear red, verify every
    texel + that the 40-byte pad stays 0xCD.
  - `tests/metal/mtlprobe.m` (+ arm64 rebuild): 1206×64 IOSurface create/render/pad-clobber check.
  - `tests/metal/test_fb2png_stride.py`: synthetic padded frame; wrong decode slope≈10; fb2png exit 2 on short buffer.
- **Content:** correctly-strided GPU home matches stage3 software (mean abs diff ~3.2; colorful icons,
  labels, dock, status bar). Side-by-side: scratch `speed1c-sw-vs-gpu.png`; docs
  `32-speed1c-gpu-home.png` + `32-speed1c-sw-home-ref.png` (stage3 slim-park). Missing-content fear was
  the bad PNG + dark wallpaper; icons were present.
- **Idle SpringBoard spin:** gone on this boot. Idle ~4–5 frames / 8–12 s; SpringBoard ~2% (Speed 1b had
  continuous ~24 fps present + SB ~160%). Continuous idle presents were an artifact of that boot's CA
  display-link behavior, not required for a correct home.
- **Fresh metrics (home page-swipe; Settings launch went black this round):**

| | Software baseline | CA Metal (Speed 1c, page-swipe) |
|---|---|---|
| Scroll/swipe fps while moving | ~40 | **~49** (p50 frame 16.7 ms) |
| Frame time p50 | 25 ms | **16.7 ms** |
| Touch→frame latency p50 / max | ~247 / ~950 ms | **~86 / ~435 ms** (19 samples) |
| backboardd CPU while moving | ~102% | **~23%** |
| SpringBoard CPU while moving | ~13% | ~74% (CA under JIT) |
| Idle: frames / 12 s | few | **few (~4)** |
| Idle: SpringBoard | ~1% | **~2%** (was 160% in Speed 1b — invalid) |

Vertical rubber-band on home: sparse frames, p50 16.7 ms when present. GPU wait age still a few ms.

### Gaps after Speed 1c
1. PosterBoard wallpaper still out (memory); home is black wallpaper with correct icons.
2. Settings/app launch went black this round — openurl job missing on a64 leave-running; use icon tap / fix openurl for list-scroll metrics.
3. Raise list-scroll confidence once a scrollable app opens; page-swipe already beats software fps.
4. cloudd+diagnosticd still eat idle CPU (~70%+20%); unrelated to Metal present path.

## Speed 2: app launch + idle + scroll (2026-10-29)

Retarget: the GPU-path "black app" was a **slow cold launch** (~30–60 s blank), not a broken paint path.
Wait up to 3 minutes; read the screen every ~10 s until real UI text appears.

### What landed
- **openurl on a64:** `host/openurl/arm64/openurl` (Linux inotify via `svc #0xfffe`); boot-a64 adds
  `com.x86code.openurl=host/openurl/arm64/openurl`. `simtouch.py app` / terminate work.
  FrontBoard open wait is 120 s when `X86SIM_ARCH=arm64` (was 30 s — too short for cold Settings).
- **nsurlsessiond in MIN:** stops cloudd's `getActiveSessionIdentifiers` → missing `nsurlsessiond` busy-retry
  (~70% core + hundreds of MB of log). This boot: no cloudd process, launchd.log idle (0 failed-lookup growth).
- **X86CODE_METAL=all** for the measurement boot (generic `com.apple.metal.simulator`); Settings UI renders on GPU.
- **memlog:** SharedStats now prints `translate Xs` on SIGUSR1. `X86CODE_A64_PERFMAP=1` → dynarmic
  `/tmp/perf-<pid>.map`.

### Settings launch (GPU, Metal=all, home already warm)

| | Cold (terminate→launch) | Warm-alive (home, process kept) | Warm-relaunch (terminate→launch) |
|---|---|---|---|
| Tap/open → first list content | **30.6 s** | **26.0 s** | **30.6 s** |
| Screen at content | Settings title; Apple Account; General; Accessibility; Action Button; Appearance; Camera; Home Screen & App Library; Search; Siri; StandBy | same | same |
| mtlvk log growth during launch | **0** | — | **0** |

Screenshot: `docs/screens/arm64/33-speed2-settings-gpu.png` (OCR-verified list rows).

### Cold-launch breakdown (SIGUSR1 SharedStats at content, ~30 s wall)

| Piece | Evidence | Share of ~30 s |
|---|---|---|
| JIT **translate** | `translate 5.9s`, ~565K blocks, ~108 MB x86 code, 21 threads | **~6 s (~20%)** |
| JIT **execute** cold UIKit/Swift/CA setup | Preferences ~100–140% CPU; lookups are GetBlock hits (dispatcher), not miss count | **~24 s (~80%)** |
| Metal / AIR→SPIR-V / MTLCompiler | mtlvk byte delta 0; compilers already up from home | **~0 s** (home warmed them) |
| openurl / FrontBoard | `openurl … Preferences: ok` after process exists | small vs JIT |
| IOSurface | iosurfaced already serving; no launch-specific stall seen | small |

Warm-relaunch matches cold: translation cache is **per-process**; kill discards it.
Warm-alive stays slow because Preferences keeps burning ~110% CPU in the background (see below).

### Settings list scroll (after content)

| | Software baseline | GPU Speed 2 |
|---|---|---|
| Motion fps | ~40 | **~50** |
| Frame p50 | 25 ms | **16.7 ms** |
| Touch→frame p50 / max | ~247 / ~950 ms | **~23 / ~247 ms** |
| CPU while scrolling | backboardd ~102% | Preferences ~53%, backboardd ~31% |

### Preferences idle spin (~110% on home after launch)
perf (~5 s, no guest map): **~42% samples in [JIT]**, with heavy `pthread_mutex_lock/unlock` (native libc via
pthread thunks). Blocks barely grow (565K→580K) while lookups double — run-loop / lock churn under the JIT,
not continued translation. Dominates whole-sim idle once Settings has been opened.

### Native hot paths (status)
- **memcpy/memset/str\***: already native via libsystem_platform stubs (Stage 0/2; memcpy bench 1.0x).
- **objc_msgSend / retain/release:** not landed this round. Launch wall is ~80% guest execution — these matter.
- **perf map:** `X86CODE_A64_PERFMAP=1` wired; use on next profiled launch.

### Commits
- `1b9b34f` — openurl arm64 + nsurlsessiond in boot-a64
- (follow-ups) memlog translate_ns, PERFMAP, openurl 120 s arm64 wait, Speed 2 docs/screens

### Gaps / next
1. Cut cold launch: persistent per-app or shared-cache JIT disk cache; native objc_msgSend fast path; revisit RSB
   under shared (off today — every return hits GetBlock).
2. Stop Preferences ~110% background spin (lock/run-loop).
3. objc_msgSend + retain/release native thunks; re-bench tests/arm64 + Settings scroll.
4. PosterBoard still out (memory).

