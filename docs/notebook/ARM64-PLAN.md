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
- **objc_msgSend (svc stub):** landed in `linux/arm64jit/objc.cpp` (cache hit → `SetPC(IMP)`; miss →
  `__objc_msgSend_uncached`; nil/tagged handled). Correct on `objcswift.sim` (identical result hashes) and
  Settings cold launch. **Default OFF** (`X86CODE_A64_NATIVE_OBJC=1` to enable): the svc/CallSVC/
  ReturnFromRunCode round-trip is slower than the ~15 guest lookup instructions under the JIT
  (`objcswift` msgSend **~537 ms native vs ~291 ms guest**). Launch wall with native on stayed **~30.6 s**
  (same as guest). Next: **inline** the lookup into translated code (no svc), or a cheaper boundary.
- **retain/release:** not landed (isa CAS / layout; keep guest).
- **perf map:** `X86CODE_A64_PERFMAP=1` wired.

### Commits
- `1b9b34f` — openurl arm64 + nsurlsessiond in boot-a64
- `1c8cc3b` — memlog translate_ns, PERFMAP, openurl 120 s arm64 wait, Speed 2 docs/screens
- (this) native objc_msgSend path (opt-in)

### Gaps / next
1. Cut cold launch: persistent JIT disk cache; **inlined** objc_msgSend (not svc); revisit RSB under shared.
2. Stop Preferences ~110% background spin (lock/run-loop) — dominates warm-alive.
3. PosterBoard still out (memory).


## Speed 4 (2026-10-30): fuse split, Stage 4 step 1 (AOT image)

### fuse_svc_ret: diagnosis and flags
- **Why Speed 3's fuse broke boot:** the x64 emitter rewrote PC=X30 after *every* CallSupervisor whose handler left
  PC at svc+4, including svcs **not** followed by `ret` (it skipped their code). Seq race / present hang followed.
- Under shared translations RSB is off, so the classic svc terminal `PopRSBHint` is `ReturnFromRunCode`: every
  libsystem call leaves `Run()`. The useful part of the "hybrid" is the terminal, not skipping PushRSB (a no-op).
- `X86CODE_A64_FUSE_SVC=1` = **hybrid** (frontend: svc;ret block ends in `FastDispatchHint`, ret block still runs).
  `X86CODE_A64_SKIP_RET=1` = **skip-ret** (GuestThread::CallSVC sets PC=X30, read *after* the handler, outermost
  CallSVC only, only when the next insn is `ret`). Both **default OFF**.
- fusestress (4 threads, ms): off 57-58, hybrid 39-40, hybrid+skip-ret 20, all OK.
- Cold boots (cleared cache, Metal all): off: home ~245 s, Settings cold 32 s / warm 5 s. Hybrid: home 158 s,
  Settings cold 32 s, then SpringBoard SIGSEGV in libobjc lookup (garbage class cache, count=1867017839) on warm
  reopen. Skip-ret: home 86 s, no restarts, then SpringBoard aborted on Settings open (BKS assertion below).
  Hybrid is not faster on Settings and crashed, so it stays off.
- **Grok's skip-ret crash (~55 s, SIGSEGV in JIT code reading a host-range guest pointer)** has the same shape as
  the libobjc probe crash above; the skip-ret path itself (with post-handler X30) reached home cleanly. Hypothesis:
  staying inside `Run()` longer (no exit per svc) exposes an existing shared-code/inline-objc race; next test is
  hybrid + `X86CODE_A64_INLINE_OBJC=0`.

### Cold-boot blockers found (not caused by fuse)
1. **backboardd misses the BoardServices bootstrap deadline** for `BKHIDUISensorService` ("no listener at automatic
   bootstrap deadline", ~11 s after start on a cold JIT). Every SpringBoard that later uses
   `_BKSHIDUISensorOverseer` aborts (BackBoardServices brk #0 after `_connectionLock_connectToRemoteServiceIfNeeded`,
   "we must have a remote target", pc 0x1c8e3e420). Cold boots restart SpringBoard 2-6 times, sometimes forever.
   Speed 3's 71 s home was a warm cache (saves only happen on SIGUSR1/exit with STATS).
2. **SpringBoard heap corruption**: write fault at Darling `libsystem_malloc` `_small_malloc_should_clear+0xc9`; the
   fault handler then deadlocks (malloc lock), so the process hangs at 0% instead of restarting.
3. **backboardd faults loading any persisted cache** (its own warm .bin *or* the AOT image): instruction fetch at a
   non-exec host address right after the call at ColorSync 0x1dde82a40 (`bl` to a stub at the end of ColorSync
   __TEXT); skipping ColorSync's range does not help, so the bad block is on the callee side (another library). This
   blocks the obvious fix for (1): a fast backboardd from cache. Bisect with JITCACHE_SKIP_RANGE by library next.

## Speed 5 (2026-10-30): crash elimination, autosaved JIT cache, linked AOT image

Metric: a cold boot (cleared JIT cache dir) + Settings cold/warm + 3 reopens with zero process crashes/restarts.
Reached on 5 consecutive runs (sweep8, warm8, c9, c10, aot3) and the final default-config boot.

Root causes and fixes (details in round-done/speed5.md):
1. SpringBoard abort in `_BKSHIDUISensorOverseer` ("we must have a remote target"): the sim backboardd has no UI sensor
   server (BKHIDUISensorCoordinator compiled out), BoardServices invalidates the declared service at its "automatic
   bootstrap deadline" (a dispatch_async to the main queue, not a timer); on a Mac the invalidation arrives after
   `-remoteTarget` returns, under a cold JIT before. `objc_direct`, so patched in the guest:
   `_connectionLock_connectToRemoteServiceIfNeeded` returns nil (`X86CODE_A64_UISENSOR=1` keeps Apple's code).
2. objc method-cache UAF: `_collecting_in_critical` cannot see JIT threads (their PCs are x86) -> returns true now
   (old buckets leak; `X86CODE_A64_OBJC_CACHE_GC=1` restores). objc bucket race: LDP/STP pairs are 16-byte atomic on
   arm64 (LSE2), two 8-byte accesses here; LDP now loads the high half (sel) first and the inline msgSend probe reads
   sel before imp (no stale-imp hit -> jump to `0 ^ class`).
3. 16K guest pages vs 4K Darling mappings: guest `vm_deallocate`/`munmap` of 4K-granular host-range memory (Mach OOL
   data) unmapped the next mapping: libsystem stub images (GOT calls into unmapped pages), glibc chunks (robin_map UAF).
   Received OOL sizes are recorded (`_mach_msg`/`_mach_msg_overwrite`), unmaps are clipped to them, to the containing
   VMA for 4K-aligned starts, and never spill into a file mapping.
4. Fault handler: native-RIP faults went to Darling's sigexc, which parked the thread in a darlingserver RPC (process
   hung at 0%). Now report (SA_NODEFER, TLS-free nesting guard, mappings, native frame walk) and SIGKILL so launchd
   restarts it. The `[x20+0xc]` diagnostic read and a SIGALRM watchdog were killing the report itself.
5. backboardd image/cache fault (ColorSync): FP/vector fallbacks call host functions via `CallFunction` without
   `RegAlloc::HostCall`, baking libarm64jit's old address into persisted blocks. `CallFunction` now marks the block
   non-persistable.
6. AOT image ran unlinked (every block exit through the locked dispatcher: home 154 s, Settings >74 s): link sites are
   persisted (A64JC06) and relinked at load. backboardd loads the image by default now.
7. Autosave thread (new) aborted metalhostd: Darling calls (logf's file write) from a non-Darling thread; and signals
   delivered to it made mldr abort. Saver blocks signals and logs to stderr only.

JIT cache autosave: idle (<500 new blocks/10 s for 20 s) or every 120 s while growing, >=5000 new blocks, plus at
exit; snapshot under the lock without unlinking live code, file written outside it (7-14 ms lock for daemons).
`X86CODE_A64_JITCACHE_AUTOSAVE=0` off. Verified: a reboot on the saved dir loads SpringBoard 754K, backboardd 175K,
Preferences 561K blocks.

Numbers (cold = cleared cache): home 68-84 s with 0 restarts (Speed 4: ~245 s, 3 restarts); Settings cold 27-32 s
(no AOT), 26 s (AOT, linked); warm 5 s. A warm per-process cache or the AOT image barely moves home or Settings:
SpringBoard spends ~24 s and Preferences ~12 s in dyld_sim prepare (all initializers), which is the next target.

## Stage 4: AOT translation (design)

Prior art (all MIT, none drop-in): Box64 DynaCache (github.com/ptitSeb/box64, docs/USAGE.md, discussion #2510:
one cache per binary/lib keyed by file identity + dynarec option hash + host CPU features, stale blocks skipped,
size cap with eviction, clean command); FEX-Emu code cache (FEX issue #5125, FEX-2609 `FEX_DISKCACHE`: offline
generation, load-time validation, mapped image; shared mmap still open there); Ryujinx PPTC (RelocEntry/Symbol
relocation of host pointers, but guest addresses stored absolute so a rebased image breaks it: we store guest
addresses image-relative); github.com/ChrisPachulski/arm64-x86-recomp (static Mach-O lifter with rebase/bind, for
install-time app AOT later). Box64 and FEX both drop cached blocks whose guest page gets written.

Long-term shape:
1. **System AOT**: the shared-cache hot set is translated once per runtime (setup or background job) and stored with
   the runtime. Hot set = block PCs recorded across boots and common apps, later widened statically
   (LC_FUNCTION_STARTS, export tries, objc method lists).
2. **Shared mapping**: the image is mmap'd read-only `MAP_SHARED` into every iOS process instead of a private copy
   per process (the main memory lever toward 1 GB/app). Code must be position-stable: ASLR off per process
   (personality ADDR_NO_RANDOMIZE on the boot stack), reloc table for what still moves, and no per-process mutable
   state in code pages: objc counters, link/patch sites, FastDispatch go through a per-process writable section
   (indirect-branch table) addressed via a fixed register or RIP-relative slot table.
3. **Per-app AOT at install** (`x86sim install`): app binary + its frameworks into a per-app cache keyed by the
   binary UUID, refined from recorded blocks.
4. **JIT fallback** for misses and runtime code (JSC); misses are recorded and feed the next build. Blocks on
   written pages are dropped (IC IVAU / write fault), as today.

File format (target, "A64AOT1"):
- One file per **image**: each shared-cache subcache (`dyld_sim_shared_cache_arm64`, `.01`) and each app image.
- **Key** = image UUID + codegen hash (libarm64jit ELF build-id, kBuildId, code-shaping flags) + host CPU feature
  mask. Mismatch refuses the whole file; a `clean` command drops files whose key no longer matches.
- **Guest addresses image-relative** (offset from the image base; the shared cache happens to be fixed at
  0x1C0000000 but per-app images move).
- Sections: header | block table (guest offset, flags/FPCR bits, code offset, size, guest-bytes hash) |
  reloc table (kind, offset) | fastmem sites | constant pool | code (page-aligned, mapped R-X shared) |
  writable-template section (copied per process).
- Validation at load: key, then per block the guest-bytes hash on first use (cheap, lazy) so a patched page
  falls back to the JIT; refuse any block whose relocs reference a kind the process does not have.
- Hot-set files (`<uuid>.pcs`-style, today the descriptor tables of the `.bin` files) are **build-independent**: a
  JIT rebuild re-generates the image from them without retraining.
- Size cap per image with eviction (invalid first, then least recently recorded).

### Step 1 (this round): what landed
- `linux/arm64jit/jitcache.cpp`: key `<uuid>-<build>` where build = kBuildId | fuse/inline flags | FNV of
  libarm64jit's ELF build-id (and its load base when ADDR_NO_RANDOMIZE). Per-process training files
  `<key>.<exe>.bin` (no more clobbering one file). `<key>.aot` image loaded at start by `.app/` processes
  (`X86CODE_A64_AOT_PROCS=name,..|all`, `X86CODE_A64_AOT=0` off).
- Builder: `X86CODE_A64_AOT_BUILD=1` on any small guest binary (hello.sim); at exit it reads the block descriptors
  of every training file for this image UUID (any build), pre-translates them (`Jit::Pretranslate`, new dynarmic
  API) and saves the `.aot`. Preferences: 546K blocks in 4.2-4.8 s -> 143.9 MB; union of backboardd +
  SpringBoard + Preferences: 654K blocks, 170 MB, 6.9 s. Load: ~300-360 ms per process.
- cfprobe (Foundation): cold 1075 ms -> 935 ms with the image, identical output.
- **Settings cold on the sim with the image: not measured.** Every boot after the image existed was gated by
  cold-boot blocker 1 (SpringBoard never stayed up; with the image in SpringBoard it died every ~55 s, with it in
  backboardd blocker 3). Before = 32 s (three cold boots).
- Not done: ASLR-off per process (relocs suffice for step 1), shared mmap, image-relative guest offsets, per-block
  guest hash.

## Speed 6 (2026-10-30): GetBlock, dyld_sim matchesPath, metadata compaction, shared AOT image (Stage 4 step 2)

Details in round-done/speed6.md. All numbers below are single runs on the shared host (about ±10 s spread).
- Profile (perf + PERFMAP; mksimdevice now passes every X86CODE_A64_* flag to jobs): GetBlock + an uncontended
  recursive mutex took 33% of SpringBoard. Fix: lock-free hit path, a growable insert-only map that mirrors
  block_descriptors. 54% of SpringBoard's dyld_sim prepare was `JustInTimeLoader::matchesPath`: it builds a full
  mach_o::Image per loaded image to read the install name (~14M calls). Fix: an inline arm64 routine in dyld_sim's
  __TEXT padding (`X86CODE_A64_DYLD_NATIVE=0` turns it off).
- Prepare time, cold: SpringBoard 25.0 -> 3.4-3.8 s (it varies, 5.8 s while also mapping the AOT image); Preferences
  12 -> 1.7 s.
- Metadata: SpringBoard anon 909 -> 433 MB. Link sites in a pool, page->blocks vectors, an 8-byte fastmem table,
  unlinked heads computed instead of stored, and a persistable flag in place of a separate vector.
- Stage 4 step 2: launchd_sim sets ADDR_NO_RANDOMIZE for its children. The code cache sits at 0x7fffa0000000; the
  FastDispatch table follows it and a fixed data page (callbacks object, objc counters) sits below it. The first
  process with a given layout writes `<key>.aot.<layout>.map` (relocated and linked); every other one mmaps it
  MAP_PRIVATE, with no writes if the bytes are already right. Result: ~180 MB of shared clean pages across
  backboardd, SpringBoard and Preferences. `X86CODE_A64_AOT_SHARE=0` and `X86CODE_A64_NOASLR=0` turn these off.
- Guest libc: arm64 memmove/memcpy/bzero/strcmp/memcmp/strlen run as guest code, sizes of 256 B and up go to the
  native function. An svc costs 42-60 ns vs 13 ns for guest strlen. `X86CODE_A64_GUEST_LIBC=0` turns it off. Wall time
  was neutral in an A/B.
- Final default config (AOT on): home 35 s, Settings cold 12 s / warm 6 s, zero crashes. PSS total 3.92 GB
  (was 5.17 GB cold without AOT). Cold without AOT: home 34-35 s, Settings 17-18 s.

## Speed 7 (2026-10-30): memory diet (lazy AOT metadata, image in every process), Swift conformance step

Details in round-done/speed7.md. Single runs on a shared host.
- PSS 3.92 -> 2.70 GB (38 processes). SpringBoard 748 -> ~490, Preferences 596 -> ~300, backboardd 466 -> ~135 MB,
  small daemons ~65 -> ~28 MB, mtlvk unchanged (~450-480 MB, mostly the NVIDIA driver: 163 MB of nvidiactl mappings,
  live heap; no leak). Of the remaining 2.7 GB, ~950 MB is clean file pages (dyld cache 704 MB, AOT .map/.idx 245 MB)
  that a second sim instance would share.
- Lazy AOT metadata: the image's blocks / link sites / fastmem sites go to `<map>.idx` (sorted, mapped shared);
  blocks are registered in lf_map on first lookup (a live bitset, no block_descriptors entry); invalidation
  sets a dead bit. `X86CODE_A64_AOT_LAZY=0` restores full registration. A flock serialises the first .map/.idx build.
- Every ASLR-off process loads the image (was apps + backboardd); `X86CODE_A64_AOT_PROCS` still overrides.
- FastDispatch: keys stored inverted (clear = MADV_DONTNEED); 64K entries for daemons, 256K for apps/backboardd
  (`X86CODE_A64_FD_ENTRIES`).
- Swift conformance: the record step of swift_conformsToProtocolMaybeInstantiateSuperclasses is read by the JIT as
  `svc #0xfffd`; native walk with a global protocol index and an exact skip of records naming a different unique type
  (value types only). Preferences: 51M -> 10M guest round trips, conformance ~14.5% -> ~3.5% of samples, -13% CPU;
  Settings cold wall time unchanged within noise. `X86CODE_A64_SWIFT_CONF_NATIVE=0` off.
- Tried and reverted: FastDispatch probe in GetBlock's entry path (GetBlock 15% -> 4%) brought iconservicesagent and
  Preferences crashes (likely a stale-entry store race after invalidation). sleepd cannot be dropped: SpringBoard
  spins on com.apple.sleepd.sleepserver.

## Speed 8 (2026-10-30): memory per instance, two instances side by side

Details in round-done/speed8.md. Single runs on a shared host.
- PSS one sim (38 procs, idle after Settings): 2.74 -> 2.32 GB. Guest heap scavenger (MADV_DONTNEED of whole free
  pages, thread caches ~32 KB/class): guest anon ~430 -> ~150 MB. darlingserver frozen memfd copies: large OOL /
  copy=TRUE data shared copy-on-write (lsd's 8 MB LaunchServices store was copied into 8 processes): 84 -> 1.5 MB.
  dynarmic constant pool no longer memset (2 MB touched per process): code-base region 108 -> 54 MB. AOT image also
  trained on MTLCompilerService + iconservicesagent (code cache 384 MB): 146 -> ~60 and 138 -> ~85 MB.
- dyld cache file pages (~450-700 MB PSS) fluctuate with host reclaim; clean, shared by instances; no change.
- Two instances: A64_DEVICE/A64_STATE/A64_DPREFIX/SIMFB_RING per instance, A64_MTLVK_SOCKET shares one mtlvk.
  Found: every darlingserver start stacked a tmpfs over the host's /dev/shm (617 layers); X86CODE_PRIVATE_SHM=1
  (set by boot-a64 / tests/arm64/run.sh) keeps it in the container; arm64 rings live in /run/user/<uid>, linked
  from /dev/shm. Two instances: 3.27 GB total, B first boot to home ~3.5 min, no crashes.
- GetBlock FastDispatch probe (X86CODE_A64_GETBLOCK_FD=1) still crashes cold boots: off. metalhostd off the AOT
  image (callee-saved register zeroed after libxpc calls with it). Settings "Time" placeholder: not addressed.

## Speed 9 (2026-10-30): Swift sleeps, GetBlock probe, metalhostd, shared prelude, fresh devices

Details in round-done/speed9.md. Single runs on a shared host.
- **Swift Task.sleep returned at once.** libswift_Concurrency's dispatch executor converts deadlines from ns to mach
  ticks with Apple silicon's timebase inlined (ns * 3 / 125, the only such site in the cache); our guest timebase is
  1/1, so every deadline was ~40x in the past. Settings' search-field task spun Preferences' main thread (~10k
  wakeups/s, ~90% of a core while idle). The JIT reads the two words that pick the conversion branch as
  "mov x21, x8; b <join>" (MemoryReadCode, only if the original words match). Preferences idle 90% -> 0%, 10k -> 2
  wakeups/s. `X86CODE_A64_SWIFT_TIMEBASE=0` off. Probes: tests/arm64/sleepprobe.swift, clockprobe.c, timerprobe.c
  (dispatch timers, clocks and continuous deadlines were already right; DISPATCH_MONOTONICTIME is bit 63).
- **GetBlock FastDispatch probe**: the Speed 7/8 crashes were a torn read: clang split the probe's 16-byte load into
  two 8-byte loads (key, then code), so a concurrent refill of the same slot returned another block's code.
  `X86CODE_A64_GETBLOCK_FD_VERIFY=1` (debug) found swapped pairs (6+ per boot, 0 after). Probe and fill are one movdqa
  each (asm); on by default (`X86CODE_A64_GETBLOCK_FD=0` off). The image must be built with the same setting
  (prelude size).
- **metalhostd NULL read** was not the image (two of the three crash logs had none): a command on a connection
  whose init found no application context (MTLSimApplicationContext* NULL). metalhostd wraps MTLSimImplementation's
  peer handlers and cancels such a connection; metalhostd is on the image again.
- **Memory**: the prelude (~1.8 MB written per process) is shared via a content-addressed `<aot>.prelude.<hash>`
  (`X86CODE_A64_PRELUDE_SHARE=0` off): anon JIT code 160 -> 109 MB. Image processes no longer autosave a .bin
  (~240 MB each, ~6.5 GB per boot, never read back; `X86CODE_A64_AOT_AUTOSAVE=1` for training). launchd_sim is off
  the image (its prelude matched only by chance; then +60 MB and 3 s). Anon PSS without mtlvk 827 -> 755-760 MB.
- **Fresh devices**: boot-a64 seeds a widget-free IconState.plist (SpringBoard's first-boot default had Maps and
  Calendar widgets: blank tiles without chronod). First boot of a fresh device waits ~80 s on LaunchServices; once
  (1 of 4) it did not recover in 4.5 min (SpringBoard main thread on the LS database lock).
- Settings search "Time" overlap: not a bug. iOS 27's Liquid Glass search pill shows the row under it ("Screen Time"
  and its purple icon); Xcode's iOS 27 simulator on the Mac shows the same with "StandBy".
- Lookups (accountsd, corefollowup, profiled): fail fast on background queues, not in Preferences' main-thread
  profile; most of their volume was the Task.sleep spin (1945 -> 118 lines). Not stubbed.
- `X86CODE_A64_GSAMPLE=<exe>` (debug): samples one guest thread's stack every 5 ms (top stacks + inclusive frames).
- Final: home 42 s, Settings cold 9-12 s / warm 3 s, 3 consecutive clean cold boots + Settings (fin1-3).

## Speed 10 (2026-10-30): image fast load, boot critical path, guest timebase, fresh devices, memory

Details in round-done/speed10.md. Single runs on a shared host (30 GB RAM, ~33 GB in swap).
- **Boot critical path.** A 1 Hz per-process CPU timeline plus SpringBoard's main-thread sampler showed that the host is
  ~80% idle during boot: it's latency-bound. Where the time goes:
  - dyld_sim prepare in SpringBoard: 3.5-12 s. The spread follows host page-cache misses: the slow runs coincide with
    seconds of btrfs-endio/kcryptd reads, as image/dyld-cache pages come back from the encrypted disk.
  - Every process on the image parsed ~1.2M index entries, sorted them and compared them with the shared .idx before
    dropping them. That took ~300 ms each, under the layout flock, so ~25 daemons starting together were serialized.
    Fixed: a published .map + complete .idx (v2) is mapped directly, 1-2 ms (dynarmic; `X86CODE_A64_AOT_FASTLOAD=0`
    off).
  - SpringBoard waits ~5 s on a FrontBoard scene lock. Home appears 8-11 s after MTLCompilerService starts, in every
    run. MTLCompilerService spends a deterministic 11.9 CPU-s per boot in libLLVM -O2 passes (InstCombine, GVN,
    inliner): shader source compiles redone every boot. The front-end caches (com.apple.metalfe) stay empty; the
    client is not identified yet.
  - boot-inner.sh's cgroup join forked tr + grep for every host process every 3 s (~35k forks per boot); now two
    grep -l passes.
  - Home: 36-44 s (image, before) -> 29/29/32 s (fin1-3).
- **Guest timebase.** `X86CODE_A64_TIMEBASE=apple` (off by default) gives the guest Apple silicon's 125/3 timebase:
  - Mach time is converted at the thunks: mach_*_time, timebase_info, mach_get_times, mk_timer_arm/_leeway/_cancel,
    mach_wait_until, and EVFILT_TIMER with NOTE_MACHTIME in kevent/kevent64/kevent_qos/kevent_id.
  - CNTPCT/CNTFRQ run at 24 MHz, mach_absolute_time is guest code (mrs), the Swift patch is off, and the cache key
    has bit 18.
  - Probes: all Swift sleeps and dispatch timers are right without the patch.
  - A full boot leaves 8 processes spinning one native thread at 100%: Darling's workqueue kevent manager receives
    events nonstop. The converted timers look right (TB_DEBUG log), so the source is not found yet; it needs Darling's
    WQDBG.
  - The scan route instead: libdispatch's inlined 125/3 helpers are guarded (it installs generic conversions when
    mach_timebase_info isn't 125/3). The 0.024/41.67 doubles in UIKitCore/SpringBoardFoundation are layout constants.
    Audio frameworks (AudioToolbox, CoreAudio, AVFAudio, PHASE, ...) carry 24e6 host-time constants; not in the boot
    path. Under 1/1, the Swift deadline patch remains the only needed fix.
- **Fresh device first boot.** lsd builds the LaunchServices store, and SpringBoard's icon threads
  (SpringBoardHome -> IconServices) wait on it in sync XPC to iconservicesagent. SpringBoard was killed at ~90 s and
  relaunched; home came after ~5 min. `A64_CLONE_FROM=<device>` (boot-a64) starts a new device from a booted device's
  data/ (like simctl clone): a second instance reached home in 25 s, and Settings opened with its icons.
  `A64_STOP_ONLY=1` stops a device's container.
- **Memory** (anon PSS without mtlvk, one sim): 769 MB -> 698 MB, from two changes:
  - lf_map grows at 75% load: SpringBoard's 32 -> 16 MB.
  - Adaptive gheap refill: 1 block, doubling to ~16 KB.

  Then 698 -> 669-680 MB from an image trained on every daemon (25 profiles: s9's five plus each daemon's cold-boot
  .bin): -3 to -6 MB per daemon. Total PSS 2404 -> 2283-2291 MB.
- Sampler: `X86CODE_A64_GSAMPLE` now starts at dyld_sim on the main thread and names the thunk a thread is in.
  `_OUT=<linux path>` writes a file (logf dropped the samples on that thread). `_ALL=1|2` samples all threads (running
  only | also blocked).

## Speed 11 (2026-10-31): icon store kept, Apple timebase by default, boot prefetch, memory

Details in round-done/speed11.md. Single runs on a shared host.
- **MTLCompilerService** (11.9 CPU-s every boot):
  - The client was iconservicesagent: IconRendering -> RenderBox -> Metal pipeline compiles, in sync XPC.
  - Its "launch" log line explained it: "one or more preconditions have been met to delete previous cache". The
    agent keeps its store (IconsCache/<uuid>/) only if `__cache_info__` has the current ProductBuildVersion, the
    directory has no ACL, and the directory's owner account name is `_iconservices`.
  - Here every process runs as the host user, so the store was wiped at every launch, together with
    `__METAL_CACHE__`: IconServices points MTLSetShaderCachePath there, which is why the Metal caches "stayed
    empty". Every boot re-rendered every icon and recompiled every pipeline.
  - Fix: the arm64 simshim reports `_iconservices` as the owner of files owned by our own uid, in iconservicesagent
    only.
  - Result: MTLCompilerService doesn't start on a warm boot, iconservicesagent 6.9 -> 1.2 CPU-s, home 32 -> 26 s,
    -113 MB PSS (also mtlvk's anon 187 -> 147 MB). A fresh device still compiles once.
- **SpringBoard's ~5 s FrontBoard lock wait**: gone with the fix above. With the old shim, the main thread waits
  24.3-28.4 s in os_unfair_recursive_lock under FrontBoard scene code on the main queue; with the fix, lock waits
  total <0.5 s.
- **Apple timebase**: the default now (`X86CODE_A64_TIMEBASE=1to1` for Darling's 1/1 plus the Swift deadline patch).
  - Darling's kevent manager got an event log (`/private/var/tmp/x86code-wqdbg` in the prefix enables it). It showed
    libdispatch's event-manager EVFILT_TIMER knotes (NOTE_ABSOLUTE|NOTE_LEEWAY|NOTE_MACHTIME) firing ~400k times a
    second, with the leeway still in guest ticks.
  - Cause: a workqueue thread hands its changes back by returning from the kevent/workloop callback. Darling's native
    libpthread then calls `__workq_kernreturn` itself, so no thunk saw those deadlines; they were 41x early, i.e.
    already past.
  - Fix: pthread_workqueue_setup wraps the callbacks and converts the returned timer changes in place (Darling
    finds the batch by its address).
  - Result: no spin, every sim process <=1% idle, 3 consecutive clean boots (fin1-3), then fin4-6 with everything.
- **Prefetch** (`cli/libexec/arm64/prefetch.py`):
  - What it reads: boot-a64 records the ranges of the dyld cache and the image .map/.idx that the sim's processes
    have mapped in (/proc/PID/pagemap; 12 s in for the early set, 60 s in for the rest; ~930 MB). The next boot
    fadvises them WILLNEED from inside its IO-capped scope while the container starts. kyber ignores ioprio, so the
    early set goes first.
  - Measured with these files evicted from the page cache (per-file fadvise DONTNEED): home 57/59/60 s without,
    49/30/33/52/34 s with.
  - No effect when the files are already cached. `A64_PREFETCH=0` turns it off.
- **Memory**:
  - dynarmic block_descriptors and patch heads at 0.9 load: SpringBoard 16 -> 8 MB and 9 -> 6 MB.
  - Anon PSS without mtlvk 677 -> 597-604 MB; total 2302 -> 2145-2161 MB (37 procs).
  - mtlvk 425-430 MB. An idle mtlvk with one empty connection is already 262 MB: nvidiactl 126, driver libraries 50,
    anon 76. Ours on top of that is ~140 MB: host-visible buffers ~44 MB (38 MB reported), anon ~30 MB, more driver
    code paged in ~30 MB, shared surfaces 19 MB, the binary.
  - SpringBoard still JIT-translates ~210k blocks (774k guest instructions, 34 MB code) that are not in the image.
    That is the next memory and launch item.
- Home 29-32 -> **20/20/24 s** (fin4-6), Settings 10 s cold / 3 s warm.

## Speed 12 (2026-10-31): keyboard, Darling RPC fd fix, stock apps, retrained image
- **Keyboard**: com.apple.InputUI added to the UI jobs (SpringBoard failed its osservice bootstrap ~60 times per boot).
  The first text-field tap then made the app's main thread spin at 100%: AFDictationConnection "availability changed"
  -> UIDictationController notification -> UISystemKeyboardDockController re-checks dictation -> again, because
  com.apple.assistant.dictation (assistantd) wasn't in the job set; InputUI's arbiter timed the app out. assistantd is
  now an on-demand job. simhid (arm64) attaches its keyboard HID service at the first `key` command, so UIKit isn't
  in hardware-keyboard mode. Settings search: the software keyboard appears and typing "wifi" on it gives "Wifi"
  with suggestions. (Dictation Enabled=false in com.apple.assistant.support does not stop the loop.)
- **Darling RPC**: `dserver_rpc_hooks_receive_message` re-ran recvmsg after an S2C call with the msg_controllen the
  previous recvmsg left (0 after munmap/msync), so a following S2C mmap's FD was truncated and the client aborted
  ("Bad S2C call: no FD, but expected one"): backboardd on app launches (once at boot), then darlingserver threw and
  the whole sim died. Fixed in the hook and the generated wrappers; libsystem_kernel installed in the arm64 prefix.
- **Photos**: needs com.apple.assetsd (else NSAssert in PXCuratedLibraryUIViewController viewDidLoad). assetsd crashed
  in LEOTokenizerCreate (ARC store into an uninitialised sqlite3_malloc(8)); it gets MallocZeroOnAlloc=1 (device
  libmalloc zeroes on free; ours evidently doesn't: next step). Calendar and Maps work as is.
- **Image**: retrained (s12/train.sh: Settings scroll, General, search typing; Calendar/Photos/Maps cold+warm) with
  X86CODE_A64_AOT_AUTOSAVE=1 in an isolated jitcache dir; s10/train2 + 38 new profiles, 1.98M blocks, 530 MB. It
  filled the 384 MB shared code cache ("shared code cache full", SpringBoard abort loop): default now 768 MB
  (reserved). Uncovered JIT code after the apps 306 -> 139 MB. SpringBoard keeps ~35 MB: blocks outside the dyld
  cache (SpringBoard.framework, the executable) are never persisted (a64_emit_x64 cache_lo/cache_hi filter).
- Fresh device (no clone) reaches home in 91 s. Seeding the icon store doesn't carry over: the store dir is named
  per device (v3 UUID tied to the LaunchServices DB; a fresh device made a random v4 one).
- Results: home 28/33/24 s, Settings 11/4 s, PSS 2.41-2.42 GB (43 procs; InputUI 85 MB, assistantd 136 MB and the
  cloudd/nsurlsessiond/securityd/tccd it pulls in), 3 consecutive clean boots + the three apps.

## Speed 13 (2026-10-31): assistantd off the boot path, zero-on-free heap, site as services

Details in round-done/speed13.md.
- **assistantd**: its LaunchEvents and ~30 MachServices (not dictation) started it at boot, and it pulled in cloudd and
  securityd. mksimdevice (arm64) keeps only com.apple.assistant.dictation (`X86SIM_ASSISTANTD=full`: Apple's plist;
  `X86SIM_ASSISTANTD_MACH=a,b`: the services kept). Typing in Settings search still works, Preferences doesn't spin,
  and assistantd isn't even launched. PSS 2.41 -> 2.27-2.31 GB (40 procs). InputUI (90 MB) still starts at boot
  (SpringBoard's osservice request, as on a device).
- **Zero on free**: the arm64 guest malloc is gheap (libsystem_malloc is replaced), so libmalloc's zero-on-free never
  ran and `MallocZeroOnAlloc` did nothing. A fresh small block kept its free-list link in word 0: exactly what
  assetsd's LEOTokenizerCreate released. gheap now zeroes small blocks (<=32 KB) on free and clears the 16-byte header
  on alloc; calloc skips its memset for small sizes. `X86CODE_A64_GHEAP_ZERO=0` turns it off. assetsd's env removed.
- **Home**: 21-35 s. Fast runs (22-26 s) have ~0% iowait; slow ones 26-39% iowait from t=7-19 s (page-cache misses
  under host memory pressure; the boot scope's IO cap is 40 MB/s and the prefetch early set is ~885 MB). SpringBoard's
  main thread is in guest code then, at 20-60% CPU. Next: a smaller, ordered early set.
- **Image beyond the dyld cache**: not done. SpringBoard.framework loaded at the same address in two boots, but 78 of
  115 non-cache r-xp mappings (SB, Preferences, backboardd, InputUI) moved. Needs deterministic placement (a shared
  path->address table applied in the mmap path, MAP_FIXED_NOREPLACE) plus a per-range guard before non-cache blocks go
  live, a .bin section listing the files, and the AOT builder mapping them; or guest-PC relocations in emitted code.
- **mtlvk** (heaptrack): ~150 MB live heap: NVIDIA libnvidia-glcore 91 MB (679 live command buffers = 9.4 MB, kept by
  metal2vulkan's spares/replays), libcuda 19 MB, Rust 21.5 MB. malloc_trim frees nothing (tried, reverted).
- **Maps in the background** (pre-existing): ~80% CPU and launchd_sim ~120%:
  -[MNNavigationServiceRemoteProxy _openConnection] reconnects to the missing navd. A navd job loops on locationd/
  routined itself (63%); a parked navd stops the loop but Maps' sheet stays empty. `A64_PARK_EXTRA`, `A64_DROP` knobs.
- **Progress site** died with the shell's cgroup scope (agent session teardown: SIGTERM to all four parts at once). Each
  part is now a transient systemd user service (`x86sim-site-<name>`, Restart=on-failure); it survived 8 sim restarts
  with the same URL.

## Speed 14 (2026-10-31): Maps idles in the background, ordered prefetch, non-cache images in the AOT image

Details in round-done/speed14.md.
- **Maps background loop**: two client retry loops without backoff against daemons not in the job set:
  -[MNNavigationServiceRemoteProxy _openConnection] (navd) and, once navd existed, -[GEOOfflineService
  _fetchCurrentState:] (geod). Backgrounded Maps held ~85% and launchd_sim ~120% answering failed lookups. A real geod
  costs ~78 MB PSS and blocked Maps' cold launch; a parked one blocked it too. Both objc_direct entries are now no-ops
  (objc.cpp maps_service_patches; `X86CODE_A64_MAPS_RETRY=1` off). Backgrounded Maps and launchd_sim <=1%, sheet intact.
  `X86SIM_NAVD_MACH`: navd (when added) serves only those services.
- **Boot I/O**: the early prefetch set is recorded as first-touch order (pagemap snapshots every 3 s up to 24 s, every
  mapped runtime/jitcache/darling-prefix file) and read in that order *outside* the 40 MB/s scope (~1 GB in <1 s on
  this NVMe); prefetch.hot stays a capped WILLNEED. Home 23/23/23 then 25/20/25 s (was 21-35), iowait ~0-3%.
  `A64_PREFETCH_EARLY_CAPPED=1`, `A64_PREFETCH_EARLY_SECS`. Note: evict.sh + boot is not a fair test (a separately
  stopped container cold-starts darling: launchd_sim at 14 s instead of 4 s).
- **Non-cache images**: they moved because dyld_sim's anywhere reservation (vm_allocate/mmap syscall helpers) was Linux
  top-down placement, i.e. the order of every earlier host mapping. mkplacement.py writes jitcache/placement.txt (169
  files, 197 MB; committed copy placement-24A434.txt) packed into the free top of the cache window [0x2ac000000,
  0x2c0000000). The reservation after dyld_sim opens a listed file, and listed main executables (map_macho), go to its
  slot (size/mtime checked, once per process; vm_allocate placement must be RW: dyld leaves bss tails in it). The table
  hash is in the cache key; the AOT builder maps the files at their slots. Those PCs already pass cache_lo/hi, so the
  existing persistence covers them. Retrained (s14/train4 = s12/train3 + 36 profiles): 2.36M blocks, 616 MB image.
  JIT anon code after Settings 106-111 -> 55 MB (SpringBoard 35 -> 3.4 MB), anon PSS 809-819 -> 737-739 MB.
- **Memory**: one sim 2.23-2.25 GB, with the three apps 2.84-2.90 GB. Largest anon: mtlvk 152 MB; per app ~40 MB guest
  heap + ~30 MB host anon (SpringBoard/Maps: a 16 MB block, probably dynarmic's lf_map, and the 4 MB FastDispatch).

## Speed 15 (2026-10-31): features restored, not disabled; geod deadlock fixed; compact image block map

Details in round-done/speed15.md. New rule: turning features off is not a speed or memory win.
- **Maps retry no-op removed** (objc.cpp maps_service_patches, `X86CODE_A64_MAPS_RETRY` gone). geod and navd are on-demand
  UI jobs with Apple's MachServices (the `X86SIM_NAVD_MACH` restriction is gone). The black first open with a real geod
  was our bug: Darling's arm64 workqueue cap (24 threads, wq_spawn) applied to every thread request and blocked the
  *requester* in sem_down. geod's GEOConfig server sat with ~20 workers in synchronous XPC waits (NanoRegistry's
  sharedInstance dispatch_sync among them) whose wakeups needed one more workloop thread: _dispatch_event_loop_poke ->
  kevent_id -> wq_spawn blocked forever, and Maps' main thread (viewDidLoad -> GEOConfigGetDictionary -> sendSync)
  waited ~60 s. The cap now covers constrained requests only; overcommit / kevent / workloop / manager requests keep
  the 64 limit (XNU's constrained pool). Maps cold 9-11 s with the full UI; backgrounded Maps, geod, navd <=1%.
  geod ~75 MB PSS, navd ~51 MB.
- **Audit** of disabled features: Spotlight was parked (home Search pill showed nothing) -> unparked by default
  (`A64_PARK` overrides; ~150 MB PSS settled). Siri is broken with or without full assistantd: its UI is
  osservice<com.apple.campo> (Campo.app LaunchAngel, not in the job set); with the job added, Campo is started by a
  Mach lookup before RunningBoard launches it, RBS has no identity for it, and SpringBoard asserts in
  SBApplicationSceneUpdateTransaction ("process was nil"). Not default; open. PosterBoard (wallpaper: home is black),
  chronod/WidgetRenderer (widgets) remain off for memory (+several GB, Speed 8): reported, not decided silently.
  UI-sensor patch: the sim backboardd has no such server (Mac end state). objc cache GC guard: correctness.
- **Image block map**: registered AOT image blocks live in an 8-byte-slot map (fingerprint + image index, key checked in
  the shared image table) instead of lf_map's 16-byte entries: SpringBoard 16 -> 8 MB, anon -4..-8 MB per app process.
- **Retrained**: s15/train5 = train4 + 5 live profiles (keyboard/search code, 124K Preferences blocks were missing),
  image c6aecb79, 648 MB.
- **More apps**: Reminders, Contacts, Files cold 4 s / warm 2 s, clean. Notes is a stub bundle in this runtime.
- **Profiles**: SpringBoard boot ~25 CPU-s; ~12% of image samples are cross-library import stubs (adrp/ldr/br x16,
  indirect dispatch): a guarded direct link at translation is the next CPU win. Settings cold: main thread ~50% idle;
  Swift type demangling (swift_getTypeByMangledName) the largest active item (~9%).
- **gheap**: per-class 1 MB chunks are only partly resident (225-600 KB avg); SpringBoard ~37 MB small heap resident, of
  which ~7 MB free lists (3.5 released), ~2 MB thread caches, rest live 32-64 B objects. ~40 MB more in the guest range
  is guest mmaps (backing stores, IOSurfaces), not the allocator.

## Speed 16 (2026-10-31): Siri works, Files Browse, wallpaper/widget cost measured

Details in round-done/speed16.md.
- **Siri**: SpringBoard asserted in SBApplicationSceneUpdateTransaction ("process was nil") because Campo's scene client
  was a plain FBProcess: FrontBoard asks RunningBoard isIdentityAnAngel(osservice<com.apple.campo>) and got NO.
  launchd_sim gives a job the angel type only if its plist path, read back with F_GETPATH (canonical), starts with
  <SimulatorRuntimeRoot>/System/Library/LaunchAngels/. Ours were in the device dir and SimulatorRuntimeRoot was the
  symlinked runtime path. arm64 mksimdevice now writes the selected angels to <real runtime>/System/Library/
  LaunchAngels/.x86code-<device>/ and passes the realpath as SimulatorRuntimeRoot. Campo (com.apple.campo) is in the std
  job set; RunningBoard launches it on the Siri tap. InputUI is now an angel too (as on a Mac). host/simshim/diag/siridiag.m.
- **FileProvider**: com.apple.FileProvider + an arm64 build of host/fpsim2host (com.apple.FileProvider.sim2host) on
  demand. Files Browse lists iCloud Drive / On My iPhone / Recently Deleted / Tags. Open: On My iPhone stays "Loading".
  Photos now also starts 3 PhotosFileProvider extensions (~37 MB PSS each).
- **Wallpaper/widgets measured** (A64_JOBS=full, widgets kept): 4.83 GB PSS over 103 procs vs ~2.55 GB. 15 poster
  extensions 828 MB PSS (537 anon), 15 widget extensions 849 MB (553 anon); JIT code 483 MB (vs ~105): the extensions
  are not on the AOT image. One MercuryPoster extension (what SpringBoard runs once a poster is chosen) costs 167 MB.

## Speed 17 (2026-10-31): wallpaper by default, poster/widget code in the AOT image, stub links

Details in round-done/speed17.md.
- **Wallpaper**: com.apple.PosterBoard is in the std job set (on-demand angel). A device without a poster store gets
  the store PosterBoard builds at first boot, with MercuryPoster in the lock/home role, which is what the Mac's iOS 27
  devices have (checked by deleting the store here: the same ribbon wallpaper came back). SpringBoard asks for
  PosterBoard every boot (+~138 MB PSS); the poster itself is one MercuryPosterExtension (~125 MB, was 167).
- **AOT training** for PosterBoard, MercuryPoster, chronod and ~35 widget extensions (full-config boot with
  `X86CODE_A64_AOT_AUTOSAVE=1` in the boot env: every `X86CODE_A64_*` variable goes into sim.env; A64_ENV only reaches
  launchd_sim). SIGUSR1 saves only when it lands on a guest thread, and guest threads block SIGUSR1, so a
  process-directed signal mostly hits a host thread: use autosave. Image 4ecf7640: s17/train6 = s15/train5 + 45
  profiles, 2.75M blocks, 712 MB. JIT code one sim 105 -> 74 MB; full config with widgets 4.83 GB / 103 procs ->
  3.30 GB / 79 procs, JIT 483 -> 136 MB, home 54 -> 36 s.
- **Import-stub links** (dynarmic A64StubLinkPass): a block ending in `br` to a value loaded from a constant address
  links straight to the slot's current value (UserCallbacks::MemoryReadConstant64: only slots and targets in the
  persisted window, read with process_vm_readv), guarded by check bit = (loaded == expected), else FastDispatch.
  `X86CODE_A64_STUBLINK=0` off. No measurable boot change this round (home 26-28 s, PosterBoard now also boots).
- **Workqueue (darling xnu 5c178c6)**: arm64 constrained requests made while the 24-thread constrained pool is full
  are queued and started when a worker's run ends, instead of blocking the requester in sem_down (Maps black for
  good once in run fb: geod had 33 threads in futex waits).
- **Files**: com.apple.DesktopServicesHelper (on demand) added: on a Mac, Files lists On My iPhone (LocalStorage
  domain, no extension in iOS 27) through DesktopServices (FIDSNode_AllFPProviders, DesktopServicesHelper.FileService).
  Our Files no longer fails that lookup, but still shows "Loading": its DesktopServices node setup (OpenSync /
  startObserving, logged at default level on the Mac at launch) never appears in our log. Open.
- **Not done**: GetBlock/FastDispatch cost, Swift type-name fast path; home 26-28 s, Settings cold 12-13 s.

## Speed 18 (2026-10-31): chunked, incremental AOT image with static discovery; launcher, CLI, MCP; feature toggles

Details in round-done/speed18.md.
- **Chunked image**: one chunk per library / executable (1826 dyld-cache dylibs, 169 placed Mach-Os; app executables
  placed from Containers/ form the app tier). Each chunk is translated by libarm64jit's builder
  (`X86CODE_A64_AOT_JOB`) into its own *slot* of the code slab (code is slab-position dependent: rel32 to the prelude,
  RIP-relative constants), saved as an A64CK01 chunk file and joined by tools/launcher into the usual A64JC06
  `<uuid>-<build>.aot`, so fast load (.map/.idx) and lazy registration are unchanged. Chunk key = library UUID (or
  size:mtime), load address, CODEGEN_VERSION, code-shaping flags. The libarm64jit build-id hash is gone from every key
  (kCodegenVersion 0x14 in jitcache.cpp, mirrored in core/src/key.rs and checked against the builder): a translator
  rebuild without a codegen change kept all 728 chunks (tested). Parallel builders own disjoint constant-pool regions;
  the assembler merges pools slot by slot and rebases reloc embeds.
- **Static discovery**: LC_FUNCTION_STARTS, symbol tables and __unwind_info of every dylib / placed file (11.5M
  entries, 1.75 GB of arm64 code), plus every direct successor the translator reports (`PretranslateCollect`: link
  targets, BL return addresses), limited to the library's code sections. A block runs only when the guest branches to
  its exact PC, so a guess in data is translated but never executed; page writes invalidate as before.
- **Measured** (4 builders, nice 19, idle IO): everything = 1995 chunks, 115.6M blocks, 29.1 GB of x86 code, 4m17s
  (15 CPU-min): 22x the largest slab (1344 MB, rel32 reach of libarm64jit/glibc). Libraries any process loads with
  static fill to a 960 MB budget: 5.19M blocks, 29 s full build; 3-chunk incremental 24 s (builder start dominates);
  no-op update 0.16 s. A/B against the trained set only (same pipeline, 2.75M blocks, = Speed 17's image): Settings
  13/7 s both, Calendar/Photos/Maps identical, untrained Reminders/Contacts cold 4 s both, dynamic JIT code after the
  full test 152.9 vs 153.5 MB, but PSS +130..165 MB one sim (bigger .idx and per-process block bitsets, fault-around
  on denser .map pages). **Default scope: the trained set ("hot")**, chunked and incremental; "loaded"/"all" are
  opt-in (x86code-aot config scope, launcher). Static code does not pay here: what still runs through the JIT is not
  in the discoverable code.
- **Code cache**: sized from the image (768..1344 MB, end fixed at the old cache end, grows down).
- **tools/launcher** (cargo workspace): x86code-core (lib), `x86code-aot status|update|build|measure|config|features|sim`,
  `x86code-mcp` (MCP over stdio: features_list/get/set/preset, sim_status/start/stop/restart, aot_status) and
  `x86code-launcher` (eframe/egui: sim controls and status, Pre-translate with progress / cancel / resume / cores
  slider, per-chunk image status with reasons and "N of M chunks need rebuilding", Features panel with presets).
  boot-a64.sh checks the image 90 s after boot (aot-status.txt; X86CODE_AOT_AUTOUPDATE=1 rebuilds).
- **Feature toggles**: `<device>/features.json`, definitions in cli/libexec/arm64/features-def.json (jobs, parks,
  costs, presets full / lean / minimal), applied by boot-a64.sh at the next boot. A shared job (assistantd: keyboard
  and Siri) is dropped only when every feature using it is off. Boot-critical jobs are never offered.

## Speed 19 (2026-10-31): native baseline, frame-accurate opens, bitmap fills, sparse image

Details in round-done/speed19.md.
- **Timing method** (`cli/libexec/simrecord.py`): `capture` copies every frame ring slot (slot t_ns, CLOCK_REALTIME) to a
  file; host/simhid (arm64) now prefixes each log line with CLOCK_REALTIME ns, so a tap's touch-down lines up with the
  frames. `open` gives response (first changed frame) and content (first frame that resembles the final screen and stays
  for 0.5 s). `mp4open` does the same for `simctl io recordVideo` files; `render` makes a real-timing mp4 with an Xcode
  "Show Touches"-style overlay; `sbs` puts two recordings side by side with a shared timer.
- **Mac baseline** (own iPhone 17 / iOS 27.0 device, booted, measured, deleted): cold opens via simctl launch 4-8 s
  (response->content 2.9-7.0 s), warm (app switch) 1.6-2.7 s (0.62-0.76 s; simctl adds ~0.9 s before anything moves).
- **Ours** is close on cold (Settings 5.4-5.8 s from the tap, Photos 7.2, Maps 8.1, Reminders 5.0, Contacts 4.0).
  Speed 18's 14 s / 7 s Settings figures were measurement (2 s OCR polls, openurl). Warm from home was 1.1-1.2 s with
  the same pid (no relaunches, no jetsam).
- **Where warm latency went**: the touch-down highlight shows at +55 ms, then nothing until +0.83 s. SpringBoard's main
  thread was 100% busy (per-thread CPU every 50 ms): 0.92 CPU-s, 12.6k page faults (~50 MB of new bitmaps), 2.5k
  context switches per tap. perf: 25% in Darling's x86 libsystem_platform: `bzero`/`memset` call `memset_pattern4`,
  which calls memmove once per 4 bytes (3.1M calls per 12.6 MB full-screen clear).
  **Fix:** guest calls to bzero/memset/memset_pattern4/8/16 bind to glibc memset / doubling memcpy fills (host.cpp dsym,
  `X86CODE_A64_FASTFILL=0` restores Darling's; Darling's own native callers and the x86_64 runtime are unchanged).
  Warm tap->content 1.09-1.22 -> 0.88-1.04 s, response 0.83 -> 0.5-0.67 s.
- **Still on tap**: libate ASTC encode/decode, vImage conversions, objc retain/release in SpringBoard (perfguest2 on
  the image): an image compressed on the CPU at JIT speed. Next target.
- **Not ours**: backboardd's 8 "IOSurfaceQueryTransactionList got non-success" lines per frame; the simulator's
  IOSurfaceClientQueryTransactionList is `mov w0, #0xe00002c7; ret`.
- **Sampler**: `X86CODE_A64_GSAMPLE_TRACE=1` writes every sample with wall-clock ns. With SpringBoard it killed the
  process silently after 10-60 s (depth 24 sooner, depth 6 later); unresolved, debug only.
  `X86CODE_A64_BIGCOPY=<bytes>` (+ `_DUMP=<file>`) sums fill/copy bytes by guest caller.
- **Sparse image files**: the launcher assembler seeks over slot holes, dynarmic writes the published .map without
  all-zero 64 KB pieces. .aot 1.2 GB -> 713 MB, .map 925 -> 514 MB on disk, byte-identical; jitcache 3.0 -> 2.2 GB.
- Videos: ~/scratch/x86code/videos/speed19 (ours, mac, sbs) and the progress site's Videos section (not in git).

## Speed 20 (2026-10-31): lock-free exclusive monitor, Wallet fixed, where tap latency and Health stand

Details in round-done/speed20.md. Host under memory pressure (33 GB swap, ~35% IO wait at the start): wall-clock opens
swung 2x between runs, so CPU-s per tap and sample shares are the primary measures this round.
- **Exclusive monitor** (dynarmic `Unsafe_IgnoreGlobalMonitor`, now default; `X86CODE_A64_STRICT_MONITOR=1` restores the
  old codegen, key bit 19; `kCodegenVersion` 0x15). With shared translations every STXR took the process-wide spinlock
  and ran an unrolled 64-slot scan (~400 host instructions). objc retain/release are LDXR/STXR loops on the isa word, so
  they were ~1/3 of SpringBoard's image samples on a tap. STXR is now a `lock cmpxchg` against the thread's own LDXR
  value (QEMU user-mode semantics; only ABA differs from LL/SC), and the reservation lives in the thread's A64JitState
  (monitor slots were shared by threads: processor_id is a counter mod 64). libobjc share on a tap: 32% -> 12%;
  SpringBoard CPU per warm tap 0.92 (Speed 19 perf stat) -> 0.69-0.82 CPU-s. Tap -> first response still ~0.52 s.
- **No ASTC encode on tap.** Guest stacks (GSAMPLE trace on SpringBoard's main thread): every libate sample is
  `at_encoder_decompress_texels` under ImageIO's ATX reader, i.e. the app's SplashBoard snapshot (.ktx, LZFSE+ASTC)
  being decoded for the zoom, then colour-converted by CoreGraphics/vImage. perfguest2's "EncodeASTC" was
  nearest-symbol attribution in stripped libate code. Snapshots are encoded (written) on the home press, not on tap.
  In [touch-down, +0.5 s]: ~20% waiting in mach_msg, ~7% snapshot decode, ~6% drawing it, ~4% colour conversion, ~5%
  os_log formatting; the rest is spread (layout, CA commit).
- **Wallet** (Swift trap at libswiftCore `_assertionFailure`): FinanceKit/FinanceDataStore.swift:54 "Could not create
  CoreDataStore". FinanceKit's client opens an NSXPCStore on `com.apple.financed.service.coredatastore`; financed was not
  in the device's job list. Added financed + passd (feature "wallet", default on; they start at boot via launch events,
  +194 MB PSS) and Health's healthd/healthappd/finhealthd (feature "health", on demand). Wallet now shows its home
  (What's New in Wallet, Passes and Tickets).
- **Health blank**: not fixed. Its main thread blocks in `-[NSFileCoordinator coordinateWritingItemAtURL:...]`
  (`_blockOnAccessClaim:withAccessArbiter:` semaphore) inside HealthPlatform's
  `destroyStoreIfNeededAndMakePersistentContainer` dispatch_once during scene creation; SpringBoard's scene-create
  watchdog fires at 10 s. filecoordinationd is running (started on demand) but never grants the write claim and is idle;
  it logs "unable to mark itself as an nspace resolver". Files' On My iPhone LOADING is the prime suspect for the same
  arbiter problem (not yet confirmed).
- **Memory** (g3 idle, 2760 MB / 46 procs): anon 899, dyld shared cache 484, other files 314, nvidia driver 310, AOT .map
  275, memfd surfaces 121, JIT code 95, .idx 46. Top processes: mtlvk 475, SpringBoard 431, Preferences 224, Spotlight
  190, backboardd 158, PosterBoard 131, MercuryPoster 122, passd 113, financed 81.
- Gates g1-g3 (cold boot + Settings + Calendar/Photos/Maps + keyboard + Siri + Files): 3 consecutive clean.

## Speed 21 (2026-10-31): dispatch_main's main queue drained again (Health, Files), repo hygiene

Details in round-done/speed21.md.
- **Repos:** the xnu fork's uncommitted SYS_thread_selfcounts (186) and the `darling/` -> `emulation/include`
  symlink were part of the installed build: the a64prefix libsystem_kernel.dylib is byte-identical to the build
  output and has `_sys_thread_selfcounts`, and 301 emulation sources include `<darling/emulation/...>` through the
  symlink. Committed to x86code/darling-xnu `x86code` (a859aa2), pointers bumped.
- **filecoordinationd never answered** (Health blank, Files "On My iPhone" stuck loading). filecoordinationd puts its
  NSXPCListener and NSFileAccessArbiter on the main queue and calls `dispatch_main()`. libarm64jit's pthread_exit
  handler parked the main thread without running the thread's TSD destructors, and libdispatch's queue-key destructor
  (`_dispatch_queue_cleanup2`) is what hands `_dispatch_main_q` to the root queue after dispatch_main. So the main
  queue was never drained again in any daemon using dispatch_main (filecoordinationd, cfprefsd, notifyd, syslogd,
  diagnosticd, fontservicesd, iconservicesagent, appprotectiond, launchd_sim itself). The handler now runs cleanup
  records + guest TSD destructors before parking (`X86CODE_A64_MAIN_EXIT_NOTSD=1` restores the old park).
  - The destructor ends in `_dispatch_sig_thread` -> `_dispatch_clear_stack`, which bzeroes down to the stack bottom
    from `pthread_get_stackaddr_np/stacksize_np`. Those were thunked to Darling's native pthread (the native stack, not
    the guest one): launchd_sim SIGSEGV. They now answer from the GuestThread stack for the calling thread.
  - Result: filecoordinationd logs "Received claim ... granted in server"; Health opens to Summary (Pinned, "Show All
    Health Data", Summary/Sharing/Browse tabs); Files browses On My iPhone ("On My iPhone is Empty") and New Folder
    creates a folder ("1 item").
- **os_log DT mode off by default** (boot-a64: `X86CODE_NODT=${X86CODE_NODT-all}`). DT mode formatted every os_log call
  to text on the job's stderr log and streamed it to diagnosticd; real iOS never formats on the send path. Warm
  Settings tap, 6 taps each, frame-timed (simrecord open from the simhid touch-down): SpringBoard CPU 0.83-0.89 ->
  0.59-0.62 CPU-s, response 0.56-0.60 -> 0.43-0.48 s, content 0.88-0.95 -> 0.78-0.79 s; ~50 MB less PSS at boot.
  `X86CODE_NODT=` (empty) brings the per-job text logs back.
- Gates: g4-g6 (TSD fix) and g7-g9 (+ DT off) 3 consecutive clean each: cold boot, Settings, Calendar/Photos/Maps
  cold+warm, keyboard, Siri, Files, Health, Wallet. g3 had one Photos NSAssert in PXCuratedLibraryUIViewController
  viewDidLoad (known "no photo library yet" path, Photos opened in the same second assetsd cold-started); not seen in
  the 6 runs after it.
- mtlvk (g1 idle 449 MB PSS): 158 anon (ours: heap, buffers), 246 file (NVIDIA libraries + 125 MB /dev/nvidiactl
  mappings: the driver's), 45 shm (surfaces); +90 MB anon/shm after the three apps. No cut this round.

## Speed 22 (2026-10-31): phone models, iOS 27.1/27.2 straight from Apple, Settings About fixed, where tap time goes

Details in round-done/speed22.md.
- **Phone models.** `x86sim device models|list|create --model "iPhone 17 Pro Max" [name]` (cli/libexec/arm64/device-a64.py)
  makes `var/device-a64-<name>` with a `model` (+ `name`) file and a reflink copy of device-a64's data/. Models in
  cli/lib/devicetypes-a64.json + devicetype-<id>.json, from Xcode 27.2's device types on the Mac (profile.plist +
  capabilities.plist): iPhone 17 (iPhone18,3, 1206x2622), 17 Pro (18,1, 1206x2622), 17 Pro Max (18,2, 1320x2868),
  Air (18,4, 1260x2736), 17e (18,5, 1170x2532), iPad Air 11-inch M4 (iPad16,9, 1640x2360 @2x). mksimdevice.py
  writes the model's identifier, product class, screen, scale, pitch, asset suffix and capabilities; the runtime
  version comes from the runtime's SystemVersion.plist. SimFramebuffer reads SIMULATOR_MAINSCREEN_WIDTH/HEIGHT at run
  time (was compiled in). The Pro Max boots to home at 1320x2868 (full-height grid, Dynamic Island) and Settings >
  General > About says "iPhone 17 Pro Max", model number A3257.
- **Launcher / simview.** boot-a64 writes the booted device to var/run-a64/device; simview takes input and logs from
  it and resizes to a new screen size. Simulator page: phones listed by model ("iPhone 17 Pro Max · Pro Max"), an
  "Add phone" picker (model + optional name), "On: <phone>" under the status. "Running" stays on once home was seen
  for that SpringBoard (an open dark app failed the frame test: the "Starting while home was up" report). MCP:
  sim_devices, sim_add_device.
- **Settings > General > About aborted** every time: MediaPlayer's library UID query threw ML3DatabaseException "no
  such table: _MLDatabaseProperties". medialibraryd (com.apple.medialibraryd.xpc), which creates the music library
  schema, was not in the job list, so MediaLibrary.sqlitedb stayed an empty file. Added (on demand); About shows
  Songs/Videos/Photos 0.
- **iOS 27.x runtimes, no Mac.** Apple's catalogue (index2.dvtdownloadableindex) has 27.0 24A434, 27.1 24A94425,
  27.2 24B8117 and 24B91016s (plus betas/RCs), all "mobileAsset", no authentication. `x86sim runtime list|url|fetch`
  (cli/libexec/arm64/runtime-fetch.py): POST gdmf.apple.com/v2/assets {AssetType com.apple.MobileAsset.iOSSimulatorRuntime,
  AssetAudience 02d8e57e-dd1c-4090-aa50-b4ed2aef0062 (Xcode simulator), RequestedBuild} (TLS: Apple's root CA) returns
  the updates.cdn-apple.com URL of an encrypted Apple Archive and its ArchiveDecryptionKey; `ipsw fw aea -b` decrypts;
  cli/libexec/arm64/aaextract.py streams the Apple Archive (metadata entries around a pbzx stream holding another
  archive with AssetData/Restore/<n>.dmg); 7z unpacks the APFS image. 27.1 was fetched this way end to end; 27.2
  24B8117 came via the Mac (xcodebuild -downloadPlatform -exportPath). Both are in runtimes/ and in the launcher's
  iOS version list. Not booted: they have no AOT image (keyed by cache UUID; a JIT-only first boot plus training and
  an image build per runtime), and placement-24A434.txt is 27.0's. libarm64jit's address shortcuts check their code
  words (swiftconf, Swift deadline, objc patch_return) or use exports (msgSend), so they switch off rather than
  misfire on another cache.
- **Tap path.** Warm Settings tap (6 taps): SB 0.54-0.70 CPU-s, response 0.43-0.58 s, content 0.73-0.79 s from
  touch-down (as Speed 21). perf split by time after touch-down + a main-thread guest stack trace (GSAMPLE_TRACE):
  - 0-100 ms after touch-down the main thread is idle in the run loop: the finger is still down (simtouch lifts after
    80 ms; icons launch on lift). Not Mach latency.
  - From lift to first response (~350 ms) the main thread is busy throughout. ~16% of it: CA commit ->
    `CA::Layer::prepare_contents` -> `CA::Render::copy_image` -> CGContextDrawImage of the app's SplashBoard snapshot
    (.ktx): ImageIO ATX -> LZFSE -> libate ASTC decode (`at_encoder_decompress_texels`, ~25-30 ms per tap), plus the
    colour conversion of that draw (vImage DoTRCs / LookupTable / PiecewiseGamma_BuildTable8to16). The rest is
    ordinary UIKit/CA/Swift work.
  - ASTC *encode* + lzfse encode run after the home press (the new snapshot), off the tap path.
  - Not done: a native at_encoder_decompress_texels (a C++ encoder object with vtable dispatch and flip flags: needs
    an ASTC decoder and the object's layout), and checking whether simfb's colourspace (1) makes CA convert the
    snapshot on every draw (needs a real simulator's SFBDisplay values).
- Gates g1-g3 3 consecutive clean (cold boot, Settings 20/10 s, Calendar/Photos/Maps cold+warm, keyboard, Siri,
  Files, Health 10-11 s, Wallet 5 s); home 57 (cold page cache after unpacking runtimes) / 36 / 37 s; PSS idle
  2870-3053 MB, with apps 3532-3674 MB (~200 MB above Speed 21, not explained yet: medialibraryd exits when idle).
- GSAMPLE_TRACE printed `g_thunks[i].name` while threads still add thunks (vector reallocation): SIGSEGV in the
  sampler thread. It prints the thunk number now.

## Speed 23 (2026-10-31): memory regression found (no jetsam on arm64), iOS 27.1/27.2 boot on their own images

Details in round-done/speed23.md.
- **Memory regression.** The launcher's 4.5 GB / 80 procs was a long-running sim: the arm64 boot never ran the jetsam
  stand-in (cli/libexec/jetsam.py; only the x86 rundev.sh did), so everything RunningBoard suspended stayed resident:
  the 12 poster extensions PosterBoard updates at every boot (Emoji, Pride, Unity x3, Weather, Gradient, Clock,
  Infograph, Aegir, Photos, ImagePlayground: ~470 MB; Speed 21's boots did not start them, cause not found: same
  device data, env and job list) plus every app opened. boot-a64.sh now clears the last boot's suspension marks and
  runs jetsam.py, which only looks at its own container's pid namespace (Darling pids overlap between containers).
  Fresh idle 4.37 GB/80 -> 2.56 GB/47 (Speed 21 2.68-2.72 GB). Gates: idle (Settings open) 2763-2799 MB, with apps
  3377-3416 MB right after the apps (before reclaim). Not reclaimed: Maps, Health, Wallet, Photos are never suspended by
  RunningBoard (no pid_suspend), so after a full app pass the launcher still shows ~4.0 GB / 65 procs: next lead.
  Where idle memory sits: dyld cache pages 715 MB, anon 637 + heap 176 MB, AOT .map 430 MB, NVIDIA 228 MB.
- **27.1 / 27.2.** x86code-aot's builders ran tests/arm64/run.sh without A64_RUNTIME, so they always mapped 27.0:
  another runtime's image was 27.0 code under its cache UUID (every xpcproxy_sim jumped to 0x200 and was killed).
  Fixed (A64_RUNTIME = config runtime_root). mkplacement.py PLACEMENT_RUNTIME=<root> makes a runtime's placement table
  from 27.0's keys (committed placement-24B8117/24A94425.txt); without one apps on the image hung black. Per-runtime
  cache dirs ($SCRATCH/jc-<build>, aot/config.json runtime_root + training from one JIT-only boot): images build in
  21-35 s. 27.2: home with all icons in ~55 s on its image, Settings opens; Maps stays black and wedges SpringBoard
  (also JIT-only: not the image). 27.1 (24A94425): home + Settings; Apple's 27.1 simulator image itself has no
  Calendar/Maps/Photos/Health/Wallet/News/Files (7.85 vs 8.32 GB in the catalogue). A fresh device's first boot stalls
  on LaunchServices (known); the second boot works.
- **Tap / ASTC.** Not done. libate decodes rows in parallel (ASTCEncoder::IsDecompressionMonolithic = false;
  dispatch per row, `DecodeRow(void*, size_t)` at 0x25cc13c90) through three function pointers in its context
  (+208 block decode into a 2640-byte intermediate, +224 transform, +216 texel writer). A native path must replace
  DecodeRow with that context layout; the entry points are virtual (ASTCEncoder vtable at enc+80). Warm tap unchanged:
  response 0.45 s, content 0.77-0.95 s.
- Gates g1-g3 3 consecutive clean (home 36-37 s, Settings 20-21/10-11 s, About "iPhone 17 / A3258 / 27.0",
  Calendar/Photos/Maps 4/2, 4/2, 6-9/2, keyboard, Siri, Files, Health 11 s, Wallet 5 s), 0 CRASH/STALL.

## Speed 24 (2026-11-01): background apps suspend, iOS 27.2 Maps past black, iOS version per phone

- **"App open regression" was the measurement.** The gates time Settings via openurl + a 1 s fb2png/tesseract poll
  (Speed 18 13/7 s, 20 16/8, 21 17-18/9, 22 20/10, 23 20-21/10-11). The frame-timed warm tap (tapm.sh, as Speed 21/22)
  is unchanged: response 0.44-0.48 s, content 0.64-0.80 s (5 taps). No regression.
- **Maps / Health never suspended: the simulator build of RunningBoard.** -[RBProcess _lock_suspend] (objc_direct)
  logs "Ignoring suspend because this process is simulated and has background audio capabilities" and returns for any
  app with UIBackgroundModes audio (Maps: audio/location, Health: audio). Confirmed on the Mac's 27.0 simulator
  (RB info log). A device suspends them. simshim (runningboardd only): -supportsBackgroundAudio answers NO when called
  from that one site, found per runtime by its log string (the "bl objc_msgSend; cbz w0" before it; +0x5ca6c in 27.0);
  the audio domain checks see the real value. Maps now gets pid_suspend after home and jetsam.py reclaims it.
  Gate g1: PSS idle 2280 MB (Speed 23 2763-2799), with apps 2611 MB (Speed 23 3377-3416).
- **Diagnostics added:** X86CODE_STATEDUMP=<guest dir> (X86SIM_JOBENV) keeps a job's os_state handlers and dumps
  them while <dir>/req exists (RunningBoard: every assertion and process state); use a dir in the device data, not the
  prefix overlay (the guest caches a missing file). os_log info/debug can't be enabled in DT mode (env, plist and an
  os_log_type_enabled interposer all failed). The Mac simulator's `log show --info` is the reference.
- **iOS 27.2 (and 27.1) fixes:**
  - libarm64jit's dyld_sim matchesPath shortcut found by its code (27.2's is at 0x1f378, not 0x1f2f4): dyld_sim
    prepare was 20-45 s per process on 27.2 without it. The BKS UI sensor patch is BackBoardServices __TEXT + 0xb22c
    in all three (the image moves). libobjc is byte-identical in 27.0/27.1/27.2.
  - Maps black on 27.2: geod's NSURLSession invalidation calls IOServiceClose on the system-power connection, which
    simshim fakes with a port whose receive right the process holds: the MIG call never got a reply, geod's session
    queue blocked, and Maps' main thread waited for geod (-[GEOUtilityService _getPathSynchronous:]). Fake ports are
    now dead names (and IOServiceClose/IOObjectRelease on them succeed). Maps now draws its map background.
  - Still open on 27.2: Maps then blocks in GEOConfigGetDictionary (sync XPCSession to geod); geod logs "com.apple.Maps
    has made more than 50 over 2.0 seconds" (its request throttle) for config writes. Next: why the throttle trips
    (a clock that doesn't advance in its window?) and whether a throttled sync request gets no reply.
- **iOS version per phone:** `device-a64.py create --model M --ios 27.2 [name]` writes <device>/runtime;
  `device-a64.py runtimes` lists them; boot-a64.sh boots a device on its runtime with $SCRATCH/jc-<build> (27.0:
  jitcache) and placement-<build>.txt. Launcher: the phone list shows "iPhone 17 · name · iOS 27.2", the version row
  is the phone's, Add phone has an iOS picker; start/restart pass only the device. MCP sim_add_device takes ios.
- **Scratch loss (00:41-00:49, 2026-11-01):** during gate g2 every directory in ~/scratch/x86code except site/ was
  deleted (jitcache and jc-* AOT caches, the prefixes, tessdata, s22-s24, round-done reports); loose files stayed.
  Not found in our code (boot-a64, run scripts, launcher, libarm64jit, fpsim2host's log shows no unlink calls). No
  /home snapshots. Gate g1 was clean (home 44 s, Settings 31/16 s on the OCR poll, About, Calendar 6/2, Photos 6/2,
  Maps 13/2, keyboard, Siri, Files, Health 10 s, Wallet 6 s, 0 CRASH); g2/g3 invalid.

## Gestures + launch gap (2026-11-01): edge touches, prefix self-heal, what the black launch gap is

- **Home-indicator / edge gestures work** (commit 302facf). Apple's Simulator marks an edge touch in the digitizer
  event mask (SimulatorKit IndigoHIDMessageForMouseNSEvent, Xcode 27.1: IndigoHIDEdge 0-4 -> 0x40000 |
  {0, 0x02000000 top, 0x08000000 left, 0x01000000 bottom, 0x04000000 right}); simhid does the same, simview marks
  touches beginning within ~16 pt of an edge, simtouch `swipe` detects it (`edgeswipe`, `[hold_ms]`). 27.2 iPhone 17:
  swipe up -> home, swipe up + 250 ms hold -> switcher (3/3, card tracks the finger from ~140 ms at 60 fps), tap card,
  swipe card away, swipe along the bottom -> previous app, Notification and Control Center.
  - A hold only opens the switcher when the drag slows into it: UIKit's pan velocity changes only when the touch moves,
    so a constant-speed drag that stops dead keeps its velocity (SBHomeGestureSettings pauseVelocityThresholdForAppSwitcher).
    simtouch's hold eases out. Earlier misses were a load artifact: `X86CODE_NODT=all,-com.apple.SpringBoard` (os_log
    text) put SpringBoard at ~92% CPU and it drew nothing until ~1 s after the lift.
  - Control Center's first page is empty by default: the Mac's fresh 27.2 device writes the identical
    ControlsIconState.plist (Now Playing and Connectivity on pages 2-3); + -> "Add a Control" works.
- **a64prefix self-heal** (648cbb0): the prefix recreated by the scratch loss lost Speed 12's libsystem_kernel
  (dserver RPC fix), and backboardd aborted again on Photos/Maps launches ("Bad S2C call"). boot-a64 runs
  cli/libexec/arm64/prefix-heal.sh: manifest pieces whose build differs go into darling-prefix (the lower layer).
- **The black gap on a cold app open** (ring timestamps, tap at 0): Wallet black 655-5365 ms, Health 605-6207 ms,
  Messages 666-3871 ms. It is the app's own start up to its first frame: there is no launch image for a never-run
  stock app (only suspend snapshots of apps that ran; the Mac's devices look the same). splashboardd in the job set
  (284 MB) made no launch image and did not shorten it: not kept.
- **Non-cache code ("lead 1")** was already persisted for placed images (Speed 14). On 27.2, images outside the
  placement table: Messages 3 small ones (~0.3 MB guest text), Health's executable 2.2 MB. Messages after a cold open:
  152 MB of code from the AOT image, 22 MB compiled live, so most live compiling is dyld-cache code the image does not
  cover (untrained paths), not unplaced images. Any placement-table change is in the cache key (image rebuild).
- News returns to the home screen ~11.6 s after a cold open (not investigated).

## Launcher QA (2026-11-01): the hub tested end to end with a second phone

Tested on a private Xvfb with a new 27.2 phone ("QA") next to the running ios272: Phones page (cards, live screens,
states, the missing-iOS group), Add phone, start / stop / restart, two phones at once (own prefix, state dir, ring;
screens and memory independent; stopping one leaves the other's processes and GPU helper alone), tap, wheel scroll,
edge swipe to the switcher, Home, keyboard into Settings search, features + restart to apply, logs, performance,
advanced, separate viewer window, iOS download (progress, cancel, resume, discard), Pre-translate, MCP, idle cost.
Fixed in tools/launcher (commits 5083f7a..1aa2bea):
- **Codegen version from the translator.** key::codegen() reads kCodegenVersion from the installed libarm64jit.so
  (g_build's initial value in .data), so GUI, x86code-aot and MCP key the image alike whatever value they were built
  with (a build with 0x10 compiled in still reads 0x17; the 0x18 bump during the test was picked up without a rebuild).
- **Wrong screen.** A new phone's sim.env names the default ring until its first boot; its viewer kept that path and
  showed ios272's screen. Viewers follow the phone's current ring.
- **Fresh device first boot.** QA stalled 4 min at 1% CPU (the known LaunchServices stall); the hub restarts a phone
  that never reached home (no <device>/.home-reached) once after 150 s idle in Starting; the second boot reached home in
  ~90 s.
- **Stuck Shift.** The key-up of Shift+key came without the modifier, so Shift stayed held on the phone ("Wi-Fi" ->
  "Wi_Fi"); Shift now mirrors the host key. Wheel scrolling added; Ctrl+V types the clipboard.
- **Stop leaked mtlvk** (~370 MB per stop: A64_STOP_ONLY stops the container only); Stop now ends the phone's own
  helper unless a running container's environment names its socket; Restart keeps it.
- **Idle cost** ~10% -> ~2-3% of a core (Xvfb/llvmpipe): smaps_rollup of ~100 processes every 2 s was ~130 ms; PSS is
  cached 12 s, repaints only on visible change.
- Rename / remove phones, download progress/cancel/resume/discard, runs started outside the hub shown (and
  cancellable) on Pre-translate, "nothing to do" runs say so, logs in five sources, "Restart to apply" from file times,
  shortcuts, MCP phones_list.
Not launcher (seen during the test): Spotlight runs at ~100% CPU and grows to 2.4-3.7 GB on both 27.2 phones; Settings
search pegs Preferences (~93% CPU) and stops taking input for a while; limited.sh reported the guests outside the
phone's memory scope (IN=0 OUT=107).

## Speed 25 (2026-11-01): app-open speed on iOS 27.2 (details: rounds/speed25.md)

- **Measurement**: tests/arm64/gate/openm.sh (tap the icon, frame ring timestamps from the simhid touch-down; cold =
  app process killed first). Baseline cold content: Settings 5.53, Messages 4.30, Health 9.93, Wallet 6.61, Maps 5.16 s.
- **Where it went**: the Swift runtime's _searchConformancesByMangledTypeName (type-by-name fallback) walked all ~346K
  conformance records per lookup under the JIT: 2/3 of a cold Settings open's guest samples. Every lookup seen was a
  full miss. Now native + indexed by type name (swiftscan.cpp). Speed 7's conformance step had 27.0's constants and
  never ran on 27.1/27.2; both steps are found in libswiftCore by code pattern, read from the cache files.
- **dyld_sim placed** at its table slot (it was always mapped anywhere: ~1.6 s of prepare translated live per process).
- **Training**: tests/arm64/train.sh needs X86CODE_A64_AOT_AUTOSAVE=1 (image-loading processes save no profile
  otherwise). 27.2 image 1.88M -> 3.06M blocks (1029 MB).
- **RSB on** (generation-checked pops; dynarmic A64JitState::rsb_gen / invalidate_gen_ptr); PushRSB sites are now
  persisted unlinked and as links (a stale pointer into int3 filler killed launchd_sim with SIGTRAP). codegen 0x19.
- After: Settings 4.2-4.6, Messages 2.6, Health 5.0-6.0, Wallet 3.4-3.6, Maps 2.9 s; warm response 0.44 -> 0.37 s.
- Fixed: News/Safari closing (JSC execute-only pages = PKU pages on Linux; the fault handler makes them readable),
  Messages conversation spin (StatusKitAgent job), launchd #IfFeatureFlagEnabled resolved with the runtime's flags.
- Open on 27.2: keyboard focus (SpringBoard "[KeyboardFocus] invalid pid for scene"), Siri, Spotlight runaway CPU/RSS,
  Settings search-typing spin (QA report).

## Speed 26 (2026-11-01): keyboard, network, Siri, Files on iOS 27.2 (details: rounds/speed26.md)

- **Keyboard**: not a focus problem ("invalid pid for scene" is logged before the app's spawn completes; SpringBoard then
  sets the real deferring target). The dictation-availability loop of Speed 12/13 again: 27.2 also asks
  com.apple.assistant.settings, which Speed 13's dictation-only assistantd didn't serve; the keyboard dock re-checked
  without end (Preferences ~75% CPU from the first keyboard) and host keys never typed. assistantd is Apple's plist again.
  Settings search and Spotlight take "wifi" from the host keyboard; Preferences idle.
- **Network on arm64** (had none): own mDNSResponder server per boot, arm64 netconfig (host network state), Darling
  `connectx`/`disconnectx`/`socket_delegate` (iOS 27's Network.framework), `/.nofollow/` paths in vchroot (Foundation's
  no-symlink prefix: nsurlsessiond's downloads never reached their owners). Safari loads pages. Open: intermittent
  ENETDOWN inside Network.framework on first flows.
- **Canonical runtime root** in sim.env: dyld_sim strips only the realpath root, so with the symlinked runtimes/<rt>/
  RuntimeRoot `_dyld_shared_cache_contains_path` failed and CFBundle found no executable for cache-only frameworks:
  WebKit's WebContent/Networking/GPU exited at once (Safari never rendered a page). First boot after: LS rebuild.
- **JIT**: CNTVCT_EL0 / CNTPCTSS / CNTVCTSS (dynarmic + fallback; codegen unchanged, image kept), FJCVTZS in the fallback.
- **Siri**: side button (iPhone 17); mobileassetd/assetsubscriptiond (Siri's UAF assets, as a Mac's device downloads
  them; ASR 39 MB installed), corespeechd and the Siri services on demand. The Siri process and presentation start; no
  visible UI yet (audio provider, accounts, server connection).
- **Files** fixed: searchd + corespotlightservice jobs; Darling renameatx_np accepts RENAME_SECLUDE/NOFOLLOW_ANY/
  RESOLVE_BENEATH (searchd's index state got ENOTSUP). Recents: "No Recents".
- **Spotlight**: typing crashes it (Apple's _CSSuggestionToken inserts a nil TOKEN when the query-parser UAF asset is
  missing). Root cause found, waits on that asset's download.
- **Gate** (tests/arm64/gate): Photos' What's New sheet dismissed; Siri by side button counting Siri's UI processes;
  stale ring frame rejected; stall = no screen and no guest CPU change. z1-z3: all checks pass except the Spotlight
  crash (z1, z2 also hit gate flaws since fixed). Zero-crash gate NOT met.

## Speed 27 (2026-11-01): Spotlight, boot time, network, Maps on iOS 27.2 (details: rounds/speed27.md)

- **Spotlight typing fixed**: the nil TOKEN was CoreSpotlight's localized strings, read from the host path
  /System/Library/Frameworks/CoreSpotlight.framework, a dangling link into the lost iOS 26.5 runtime; boot-a64 links
  the booted runtime's copy into the prefix each boot. The query-parser asset now downloads (106 MB, as on a Mac), Core
  ML compiles it (Darling F_PREALLOCATE + other fcntls), assistant_cdmd job (embedding service).
- **Boot to home 73-82 s -> ~16-42 s**: lsd rebuilt LaunchServices every boot (44.7 s) because the rebuilt store never
  saved: NSFileManager replaceItemAtURL resolves the parent by ID (fsgetpath); Darling's getattrlist now remembers the
  parent's path for the parent ID it hands out.
- **Network first-connection ENETDOWN fixed**: a TCP_INFO read racing connect() looked "disconnected" (re-read once),
  and close() left deferred-delete knotes alive (libkqueue: close drops knotes; EV_ADD replaces a knote of another file).
- **Maps**: tiles, search, place card, Directions card (nanoregistryd job: NanoRegistry sync-query deadlock froze Maps'
  main thread; host GeoServicesData via cli/libexec/arm64/fetch-macos-host.sh). Open: the route itself aborts
  (AttributeGraph "attribute failed to set an initial value"; not deadline, not AOT, not multi-core).
- **arm64 CPU identity**: hw.cputype/cpusubtype/machine/cpufamily/optional.* as an M1 for arm64 processes.
- **JIT**: FEAT_FRINTTS (FRINT32Z/32X/64Z/64X) in the fallback; Swift type index keeps indirect records; training
  saves profiles on abort (AOT_AUTOSAVE only). Image retrained 3.06M -> 3.39M blocks (Messages, News, Safari, Maps
  route, switcher).
- **Gate**: logs first (process + JIT-log crash before any screen read); Messages, News, Safari page, Maps tiles,
  Spotlight typing, swipe-up home, app switcher, Health first-run sheet. Zero-crash gate NOT met: JSC JIT-code faults in
  WebContent on en.wikipedia.org (new, open); Siri UI still absent (no audio input device: simaudio arm64 built, HAL
  not reached yet).

## Speed 28 (2026-11-01): dyld_sim loader scans, JSC stale code, huge pages blocked (details: rounds/speed28.md)

- **Baseline explained**: Speed 27's "first open after boot" numbers include the app's on-demand daemons starting
  (Health: healthd, healthappd and 4 more; Maps: navd, AppIntentsLiveEntityService), each paying dyld_sim prepare;
  openm.sh's default kills only the app. Not a Speed 26/27 code regression (host memory pressure accounts for the rest).
- **dyld_sim's loaded-loader scans native** (dyldsim.cpp, svc at the loop heads of loadDependents/getLoader): ~45% of a
  cold Preferences' first 10 s was matchesPath over ~900 loaders per lookup. Cold (app killed): Settings 5.05 -> 3.5 s,
  Messages 3.25 -> 2.2, Health 6.9 -> 5.3, Wallet 3.95 -> 2.86, Maps 6.15 -> 5.1; first open after boot Settings
  5.35 -> 4.15, Health 9.3 -> 7.4, Maps 7.3 -> 5.9. `X86CODE_A64_DYLD_SCAN=0` off.
- **JSC stale JIT code fixed**: sys_icache_invalidate / sys_cache_control went to Darling's x86_64 libplatform
  (no-ops); they now invalidate the JIT range. Remaining WebContent crash: DOMWrapperWorld::clearWrappers (Safari
  Reader), also with JSC's JIT off: not JIT code. Open.
- **Huge-page image**: tmpfs copy filled through MADV_HUGEPAGE, kept only with >= 50% 2 MB folios; this host has no
  free order-9 blocks, so it falls back (no gain measurable here).
- Gate: 1 failure per boot (the WebContent crash); zero-crash gate NOT met.


## Speed 29 (2026-11-01): WebContent root cause (MADV_ZERO), class conformance filter, clean gate (details: rounds/speed29.md)

- **Safari WebContent clearWrappers crash fixed**: Darling's sys_madvise returned success for every advice >= 5 without
  acting; XNU's MADV_ZERO (11), used by libpas to zero recycled pages, left stale data in "zeroed" tables. It now
  fails with ENOTSUP (callers zero themselves). darling-xnu 91f5392.
- **Swift conformance filter for class queries** ([sp+0xb0] = queried metadata): chain descriptors, only with a
  non-Swift root; verify mode found 0 dropped matches. Cold Settings round trips 10.0M -> 7.2M; Health cold 5.5 -> 4.5 s.
- **Gate: 3 consecutive clean boots** (s29g1-3, Safari on Wikipedia included).
- Open: Maps route ("attribute failed to set an initial value: DynamicLayoutComputer<_VStackLayout>", independent of
  every native shortcut and of the IR opts; GATE_ROUTE=1 runs it); SpringBoard ~810 mach_msg/s idle (MSG_STATS).

## Apps (2026-11-01): installing third-party apps, test-app suite

- **`x86sim install|uninstall|launch [<phone>] …`** (simctl-style leading device: name / folder / dir, cli/lib/pickdevice.sh)
  on arm64 phones; launcher core `apps.rs` (install / uninstall / launch / list) and MCP tools `app_install`,
  `app_uninstall`, `app_launch`, `app_list` for the hub's "Install app…".
- **installcoordinationd** was not a job on arm64: every install failed ("Failed to create IXPlaceholder …
  com.apple.installcoordinationd … lookup error 3"). On demand in boot-a64.sh's UI set.
- **mobiletimerd** (AlarmKit, com.apple.alarmkitservices) added on demand; it then trapped at every start in
  `+[BSServiceQuality main]`'s "invalid QoS class" assertion: Darling's `bsdthread_register` never wrote `main_qos`
  back (XNU does: the main thread's QoS, DEFAULT when none), so `qos_class_main()` was UNSPECIFIED in every process.
  Fixed in darling-xnu (libsystem_kernel). Cadence's AlarmKit prompt, scheduling and calendar work.
- **gettid (286)** wrote through its pointers unconditionally; React Native's glog calls `syscall(SYS_gettid)` the
  Linux way (junk pointer) and Waymark (Expo / Hermes) died with SIGSEGV at start. Now as XNU: ESRCH unless the thread
  assumed an identity with settid.
- User apps (built on the Mac, in $X86SIM_SCRATCH/apps, not committed): Cadence (SwiftUI + AlarmKit + widgets ext) and
  Waymark (TCRClone's Expo app, Release build with the JS bundle) install, launch and run on an iOS 27.2 phone.
- tests/apps (x86ui, x86net, x86data + build.sh / run.sh): all checks PASS on 27.2 (README there).
- Open: first keys typed right after the first tap into a text field can be lost while the keyboard session comes up
  (later bursts arrive intact); the in-app WKWebView paints a few seconds after didFinish.

## Speed 30 (2026-11-01): Maps route abort narrowed, SpringBoard idle traffic identified (details: rounds/speed30.md)

- **Maps route** (AttributeGraph "attribute failed to set an initial value", 10 of 13 route runs): the rule runs and
  leaves no value. Ruled out with evidence: AG's TLS (abort dump of the current UpdateStack: right stack, right node),
  deadline cancellation (frame flags not cancelled; JIT probes at set_deadline / passed_deadline / cancel never fired;
  Speed 27's interposition could not see cache-internal calls), the guest heap scavenger. Found and fixed behind
  `X86CODE_A64_REMAP_FREEZE=1`: Darling's shared vm_remap (copy into a memfd, then map it over the source) loses
  concurrent stores; not the Maps cause. GATE_ROUTE stays off by default.
- **SpringBoard idle**: real mach_msg ~256/s (Speed 29's 810 counted voucher_mach_msg_* thunks), all main-thread
  run-loop wakeups driven by two CADisplayLinks: SBUIPreciseClockTimer (15 fps, also on a Mac) and
  _UIMotionEffectEngine (30 fps, simulator motion provider without events; not on a Mac). Not reduced yet.
- Tooling: a64-penv variables reach the guest env; ABORT_TSD, AGDL, THUNK_HIST, MSG_STATS histogram; diag shims
  agnodediag.c, dldiag.m (CADisplayLink / motion engine).
- Not done: app-code persistence, hot/cold layout, SplashBoard ASTC/LZFSE decode, Siri audio input, msg fast path.
