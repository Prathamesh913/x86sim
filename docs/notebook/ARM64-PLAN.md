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

### Still open (stage 3 remainder / stage 4 prep)
1. Fix shared-cache SpringBoard boot (linking/invalidation races or thunk-boundary crashes under contention).
2. Then re-measure shared boot-to-home + memory; restore chronod/FileProvider if it fits.
3. Persistent on-disk translation cache; profile the ~6 min boot.
4. Crash fixes: BKS assert, dyld_sim/thunk SIGSEGV, guest TLS destructors on every exit path.
5. Metal → Vulkan (arm64 shims / 27.0 selector changes).
