# Metal stand-in for the iOS simulator (Linux, Vulkan)

Status: PLAN.md stages 1-5 work (handshake/device, buffers + textures over shared memory, blit and clear
passes, command buffers with handlers, shared events; compute: metallibs and runtime-compiled source,
function constants, compute pipelines, compute encoders, samplers, threadgroup memory, real GPU addresses;
**render** (stage 5): render pipeline states, vertex descriptors, blending, depth-stencil states, MSAA + resolve,
render encoders with all bindings, fixed-function state and draws, textures over IOSurfaces shared across
processes, imported without copies). mtlprobe: 46/46.

**Metal is on by default** in the launcher (its boot setting passes `X86SIM_METAL=all` to
`cli/libexec/arm64/boot-a64.sh`; `X86SIM_METAL=0`, boot-a64.sh's own default when run by hand, gives the software
renderer). CoreAnimation's own Metal renderer composites the screen on the host GPU into
SimFramebuffer's swapchain IOSurfaces, and it draws the iOS 26 glass dock platter that the software renderer leaves
out. MercuryPosterExtension draws the default wallpaper, which matches a Mac's since the `uint4` alignment fix
(docs/screens/36-metal-wallpaper-astc-fixed.png, 37). Apps get
a device too: Settings, Calendar, Safari and Maps' Metal map view work (35). Presents wait for real GPU completion.
If mtlvk dies, the boot falls back to software rendering. See "CoreAnimation on Metal" and "Robustness" below.

Cross-process IOSurfaces (`com.apple.IOSurface.Remote`, host/iosurfaced/iosurfaced) are **on by default**.

## Architecture as built

```
 sim app (mtlprobe)                     metalhostd (Darling, sim binary)                mtlvk (native Linux)
 Metal.framework -> MTLSimDriver  --XPC-->  Apple's MTLSimImplementation      --VKMTL-->  metal2vulkan-runtime
  "com.apple.metal.simulator.<proc>"         + MetalSerializer deserializer    Unix sock   (Metal API on Vulkan)
  MTLBuffer = mmap MAP_SHARED|ANON           replays on VKMTLDevice (vkmtl.m)   + memfds    -> NVIDIA RTX 5090
  -> xpc_shmem (darlingserver memfd)  ====  same pages  ====  syscall 599 -> fd  ==SCM_RIGHTS==> mmap + VK_EXT_external_memory_host
```

- **metalhostd** (`metalhostd.m` + `vkmtl.m`, one iossimulator binary; arm64 build in `host/metalhostd/arm64`): dlopens the runtime's
  `MTLSimImplementation`, rebinds its (and MetalSerializer's) GOT entries for `MTLCreateSystemDefaultDevice` /
  `MTLCopyAllDevices` to ours, and calls `init_with_xpc_connection(listener, registryID, 0)` for every service name in
  `X86SIM_METAL_NAMES` (a launchd job with those `MachServices`). Everything Apple's server does (handshake, buffers
  from xpc_shmem, object commands, command-buffer segments, getBytes into the app's shmem, completion messages 100/101,
  event listeners) is Apple's code; only the `id<MTLDevice>` below it is ours.
- **vkmtl.m**: `VKMTLDevice`, `VKMTLBuffer`, `VKMTLTexture`, `VKMTLCommandQueue`, `VKMTLCommandBuffer`,
  `VKMTLBlitCommandEncoder`, `VKMTLRenderCommandEncoder` (load/store actions only), `VKMTLSharedEvent` (also MTLEvent),
  `VKMTLFence`, and for compute `VKMTLLibrary`, `VKMTLFunction`, `VKMTLComputePipelineState`, `VKMTLSamplerState`,
  `VKMTLComputeCommandEncoder`. Commands are recorded per command buffer and sent on commit; completions come back asynchronously and
  run the handlers. Unimplemented selectors are logged (`vkmtl: unimplemented -[...]`) and answer 0/nil
  (`forwardInvocation:` with signatures from the Metal protocols).
- **mtlvk** (`mtlvk/`, Rust): one thread per metalhostd connection; object table (buffers, textures, queues, events);
  executes commands with [steelbrain/metal2vulkan](../../external/metal2vulkan)'s runtime crate (cloned at f6a96b7,
  license GPL-2.0-or-later OR LGPL-3.0-or-later; mtlvk takes the same). Buffers are imported **without copies**:
  `new_buffer_with_bytes_no_copy` over our mmap of the memfd (VK_EXT_external_memory_host); if a driver refuses,
  mtlvk mirrors the buffer around each command buffer, and if Darling only has a snapshot fd, metalhostd copies
  (`BUF_WRITE`/`BUF_READ`). Textures live in mtlvk; replaceRegion/getBytes of Private textures go through a staging blit.
- **Protocol**: `vkmtl_proto.h` (mirrored by `mtlvk/src/proto.rs`): 16-byte header, requests with seq get replies,
  `CB_COMMIT` carries a list of commands (copies, fill, mipmaps, render pass, signal/wait, compute encoder
  BEGIN/state/dispatch/END), `EV_CB_DONE` comes back. Stage 4 added `LIB_NEW`, `FUNC_NEW`, `CPS_NEW`, `SAMPLER_NEW`;
  `BUF_NEW`'s reply carries the buffer's GPU address.
- **Darling** (patches/darling-iossim.patch, all marked `x86Sim:`):
  - darlingserver RPC `x86sim_memfd_for_region` (appended last in generate-rpc-wrappers.py so older clients keep their
    call numbers) + libsystem_kernel BSD syscall **599**: `fd = syscall(599, addr, len, &offset, &shared)` returns the
    memfd behind a range of the caller's memory (made shared first, as `mach_make_memory_entry` does).
  - darlingserver: anonymous `MAP_SHARED` memory (Linux "/dev/zero (deleted)") is moved into a memfd when it is
    shared, like private memory, instead of being handed out as a snapshot. MTLSimDriver allocates every MTLBuffer
    (and getBytes destination) with `mmap(MAP_SHARED|MAP_ANON)`; with snapshots the GPU's results never reached the app.
- **simshim**: `xpc_connection_enable_sim2host_4sim` is skipped for `com.apple.metal.simulator*` and
  `com.apple.IOSurface.Remote` connections (it packs messages in the host's macOS XPC format for the Mac-side
  service; our services are inside the simulator and its libxpc dropped them).
- **IOSurfaces** (stage 5): `iosurfaced` (host/iosurfaced) serves `com.apple.IOSurface.Remote`: surfaces are
  `mmap(MAP_SHARED|MAP_ANON)` memory that every process maps from the `MemoryObject` xpc_shmem (darlingserver moves
  it into a memfd). For a texture over a surface (message 13: MTLSimImplementation calls
  `IOSurfaceLookupFromXPCObject` and then `newTextureWithDescriptor:iosurface:plane:`), metalhostd's IOSurface
  client maps the same pages. vkmtl wraps them in a VKMTLBuffer (memfd through syscall 599, imported by mtlvk
  without a copy), and mtlvk makes the texture with the runtime's `Buffer::new_texture_with_iosurface_plane`.

## Gating

`cli/libexec/mksimdevice.py` adds the `com.x86sim.metalhostd` job only when `X86SIM_METAL` is set and not `0`.
`cli/libexec/arm64/boot-a64.sh` starts this boot's mtlvk on `mtlvk.sock` in its state dir (log `mtlvk.log`), or shares a
running one (`A64_MTLVK_SOCKET`), and waits up to 5 s for the socket. If mtlvk does not come up, it boots with
`X86SIM_METAL=0`:
- `X86SIM_METAL=1`: `com.apple.metal.simulator.mtlprobe` only (the test client).
- `X86SIM_METAL=proc1,proc2`: `com.apple.metal.simulator.<proc>` for those processes (MTLSimDriver looks up
  `com.apple.metal.simulator.<getprogname()>` first).
- `X86SIM_METAL=backboardd`: CoreAnimation's render server (backboardd) composites with Metal. Every other process
  keeps the software path.
- `X86SIM_METAL=all`: also the generic `com.apple.metal.simulator`, so every process gets a device (SpringBoard,
  the wallpaper poster, RenderBox, apps). **This is the launcher's default.**
- `X86SIM_METAL=backboardd,PosterBoard,MercuryPosterExtension`: CA plus the wallpaper only (apps stay software).
- `X86SIM_IOSURFACE` (default 1): the iosurfaced job, and no `IOSURFACE_REMOTE_EMULATED`. `X86SIM_IOSURFACE=0`
  goes back to per-process emulation (IOSurfaces cannot be shared; Metal IOSurface textures then fail).
  `X86SIM_IOSURFACE_LOG=1`: iosurfaced trace.
Diagnostics (mtlvk): `MTLVK_DUMP_PASSES=<dir>` (colour attachments after every render pass, resolve textures for
MSAA; every level of the textures a compute encoder bound; compute buffer bindings logged; `MTLVK_DUMP_MAX`
command buffers, default 60; view with a raw-to-PNG script), `MTLVK_DUMP_REPLACE=<dir>` (every replaceRegion's
bytes), and refused render pipelines are written to `run/failed/` (replay: `rpsrepro <profile> <id>-vert.metallib
<id>-frag.metallib I:FORMAT... stencil=N`). `astctest` (bin) + tools/debug/astccmp.py check MTLSimDriver's ASTC kernel
against a CPU decoder.
Optional: `X86SIM_VKMTL_LOG=1` (metalhostd trace), `X86SIM_VKMTL_DUMP=1` (metalhostd saves every metallib it
receives to `var/metal/captured/`), `X86SIM_METAL_PROFILE=<n>` (hostFeatureProfile, default 10001 =
Mac2 + Common3, no tile shaders), `MTLVK_LOG=1` (mtlvk trace).

## Build and run

```sh
# Linux side (needs Vulkan; uses external/metal2vulkan's target dir to share its build)
cd linux/mtlvk && CARGO_TARGET_DIR=../../../external/metal2vulkan/target cargo build --release
../../../external/metal2vulkan/target/release/mtlvk selftest      # memfd import, clear, blits, events on this GPU
../../../external/metal2vulkan/target/release/mtlvk selftest-compute   # stage 4 handlers: shaders/probe.metallib kernels
# translator survey: which functions of metallibs compile (function constants get zero values)
../../../external/metal2vulkan/target/release/survey <profile.json> <lib.metallib>...
# shaders (on a Mac, target air64-apple-ios26.5-simulator): shaders/build.sh -> probe.air, probe.metallib, probe.ll;
# (also writes probe_metallib.h / probe_source.h, which mtlprobe embeds)
# Darling side + probe (arm64 builds; on Linux with sdks/, or on a Mac)
build/mac-build-arm64-shims.sh metalhostd iosurfaced mtlprobe         # -> host/*/arm64/..., tests/metal/arm64/mtlprobe
# Darling changes (no sudo needed: only darlingserver and libsystem_kernel change; replace by rename, they may be busy)
cd darling/build && ninja src/external/darlingserver/darlingserver src/external/xnu/darling/src/libsystem_kernel/libsystem_kernel.dylib
# test: starts mtlvk, boots a minimal arm64 device (metalhostd + mtlprobe + iosurfaced) in its own prefix
SECS=60 tests/metal/run-probe-a64.sh
# the normal boot: Metal for every process (boot-a64.sh starts mtlvk; X86SIM_METAL=0 for software rendering)
X86SIM_METAL=all A64_KEEP=1 cli/libexec/arm64/boot-a64.sh
```

Probe output (2026-10-27, RTX 5090, driver 615.78.08), stage 5 (46/46). Every result is checked on the CPU:

```
...stage 1-4 checks as below...
PASS render pipeline tri_vs/color_fs (BGRA8)
PASS clear + coloured triangle: 0 of 4096 texels wrong (1953 inside), (8,56) BGRA=30,34,191,255 (18.3 ms)
PASS render pipeline quad_vs/tex_fs (vertex descriptor, RGBA8)
PASS indexed quad (uint16), vertex descriptor, texture + sampler, tint: 0 of 4096 wrong, (40,20)=176,36,210,255
PASS blending 50% red over blue (src alpha, 1 - src alpha): 0 wrong, BGRA=128,0,127,191
PASS scissor rect (32,8 16x24): 0 wrong
PASS depth-stencil state and pipeline with a Depth32Float attachment
PASS depth test (Less, write): 0 of 4096 wrong, (8,8) BGRA=255,0,0 (40,8)=0,0,255
PASS 4x MSAA triangle resolved: 0 wrong off the edge, edge (20,20) BGRA=86,168,0,255
PASS texture over IOSurface 1 (64x64 BGRA, bytesPerRow 256)
PASS render into the IOSurface, read through IOSurfaceGetBaseAddress: 0 wrong
INFO child process 53: IOSurface 1 64x64: 0 wrong, 1953 inside
PASS process 53 looked IOSurface 1 up and read the rendered pixels (exit 0, spawn 0)
SUMMARY 46 passed 0 failed
```

The triangle check compares every texel off the diagonal with the CPU's barycentric interpolation (+-2/255). The
quad checks every texel against the 8x8 source texture (nearest), with the green channel halved by the tint from
`setFragmentBytes`. The IOSurface test renders into a texture over a surface made by `IOSurfaceCreate`. The
pixels are read through `IOSurfaceGetBaseAddress`, then by a child process (`posix_spawn` of mtlprobe
`readsurface <id>`, which calls `IOSurfaceLookup`).

Stage 4 (34/34):

```
...stage 1-3 checks as below...
PASS newLibraryWithData (probe.metallib, 29331 bytes, iOS-simulator AIR): reduce_sum,fconst,sample_tex,fill_tex,saxpy_bytes,via_ptr,vadd (2 ms)
INFO pipeline vadd: threadExecutionWidth=32 maxTotalThreadsPerThreadgroup=512 staticThreadgroupMemoryLength=0 (2 ms)
PASS compute pipeline vadd (width 32, max 512)
PASS vadd float x4096: 0 wrong, c[3]=998.5 (status 4, 1.3 ms)
PASS setBytes params + dispatchThreadgroups (saxpy): 0 wrong
PASS kernel writes a 100x60 RGBA8 texture (getBytes): 0 texels wrong, (7,5) = 7,5,12,255
PASS threadgroup-memory reduction (16 groups x 256): 0 of 16 sums wrong, sum[0]=228736
PASS function constants (float 2.5, bool true by index; int 3 by name): 0 wrong, y[2]=-5.5
PASS setSamplerState + texture sample in a kernel: 0 of 6000 wrong
PASS kernel reads buffer pointers (gpuAddress 0x100020000, 0x100028000) from memory: 0 wrong
PASS newLibraryWithSource (probe.metal): reduce_sum,fconst,sample_tex,fill_tex,saxpy_bytes,via_ptr,vadd (0 ms)
PASS vadd from newLibraryWithSource: 0 wrong
SUMMARY 34 passed 0 failed
```

Stages 1-3 (unchanged):

```
PASS MTLCreateSystemDefaultDevice() = 0x7f8503708880 (523 ms)
INFO name="Apple iOS simulator GPU" class=MTLSimDevice registryID=0xf02326ddc9527b2d
INFO maxBufferLength=268435456 unified=0 lowPower=0 headless=0 recommendedMaxWorkingSet=0
INFO maxThreadsPerThreadgroup=512x512x64 argumentBuffersSupport=0
INFO families: Apple4=0 Apple7=0 Apple9=0 Mac2=0 Common1=1 Common2=0 Common3=0 Metal3=0
PASS texture replaceRegion/getBytes round trip (64x64)
PASS command buffer completed: status 4 error none (16.0 ms)
PASS GPU clear to (1, 0.5, 0.25, 1) read back via blit: first texel BGRA = 64,128,255,255, 0 of 12288 texels wrong
PASS blit buffer->buffer (4096 bytes)
PASS blit buffer->texture (8x8 at 40,40), read with getBytes
PASS shared event signaled by GPU: 42
PASS CPU setSignaledValue(50) released it; it signaled 60 (value 60)
PASS notifyListener:atValue:70 fired (70)
PASS 100 command buffers in order (96.6 ms total)
SUMMARY 23 passed 0 failed
```

The device name, families and limits the app sees are the client's own (MTLSimDriver's `MTLSimDevice` hard-codes
`featureProfile` 1 -> Common1, maxBufferLength 256 MB, "Apple iOS simulator GPU"), as on a Mac; the server's
`registryID` (the Vulkan device's) and `hostFeatureProfile` come from us.

## Files

- `metalhostd.m`, `vkmtl.m`, `vkmtl_proto.h`, `mtlprobe.m`; binaries `host/metalhostd/arm64/metalhostd`,
  `tests/metal/arm64/mtlprobe` (from build/mac-build-arm64-shims.sh)
- `../simfb/simfb.c` (GPU-synced presents), `../simhid/simhid.m` (touch input; cli/libexec/simview.py, cli/libexec/simtouch.py)
- `mtlvk/` (Cargo crate; `src/main.rs`, `src/conn.rs`, `src/proto.rs`; `src/bin/survey.rs`, the translator survey)
- `shaders/`: `probe.metal` (the probe's kernels), `build.sh` (Mac: compiles with `-target air64-apple-ios26.5-simulator`),
  `probe_metallib.h`/`probe_source.h`
  (embedded in mtlprobe), `captured/*.wire.metallib` (what MTLSimImplementation actually hands the device: one
  metallib per specialized function, see "Shader path")
- `the metal2vulkan x86sim branch`: our fixes in external/metal2vulkan (`git apply` there)
- `../iosurface/iosprobe.m` (two-process IOSurface test); `host/iosurfaced/iosurfaced.m` (com.apple.IOSurface.Remote)
- `run-probe-a64.sh` (the arm64 probe boot)
- `PLAN.md`, `PROTOCOL.md`: design and the transport's wire format

## Shader path (stage 4)

- The client compiles everything to AIR itself: `newLibraryWithData` keeps the app's metallib on the client, and
  `newLibraryWithSource` runs the runtime's own MTLCompilerService (it works under Darling, ~0.5 s cold). Each
  `newFunction*` / pipeline creation sends the server (message 15) a **metallib of one function, already
  specialized** (function constants resolved by the client's compiler): file format 2.4, no OS fields, triple
  `air64-apple-ios13.0.0-simulator`, producer metalfe-32023.884. The server calls `-[device newLibraryWithData:error:]`,
  `-[library setOverrideTriple:@"air64-apple-macosx10.14.0"]` (for the Mac's compiler; we only remember it),
  `-[library newFunctionWithName:]`, and for the pipeline `-[device newComputePipelineDescriptorWithSerializedData:
  deserializationContext:]` (borrowed from Metal.framework's `_MTLDevice`, it doesn't use the device) then
  `newComputePipelineStateWithDescriptor:options:reflection:error:`. So `newFunctionWithName:constantValues:` never
  reaches the server here; it is implemented anyway (the three MTLFunctionConstantValues setters are swizzled in
  metalhostd to record values).
- metal2vulkan translates this AIR (Xcode 27.1's `metal -target air64-apple-ios26.5-simulator` = air64_v28, and the
  runtime compiler's) without changes. `mtlvk survey` over the runtime's metallibs (RTX 50 profile, constants zeroed):
  MTLSimDriver 12/12 (after our fix), CoreImage 60/60 kernels, QuartzCore 141/168 (13 tile kernels need tile
  attachments, 14 fragments need framebuffer-fetch formats: both are pipeline inputs, not refusals), RenderBox 54/60,
  MercuryPoster 36/44 (7 refusals: MSL 3.2 program-scope bindings `air.global_bindings`, not implemented by the
  translator; **43/44 since stage 5**, see below).
- Translator fix (the metal2vulkan x86sim branch): a `select` between two constant globals' addresses (blob pointers)
  was refused (`Lowering(Access) NotImplemented`); MTLSimDriver's `pvrtc4` decoder does it.
- Translator fixes (Spike 3.7, same patch): `[[front_facing]]` taken as an `i8` (RenderBox's specialized
  `path_interior_fragment`), and no `uvec4` units for 16-byte leaves (a `uint4` load from a buffer bound at a
  4-aligned offset read the wrong bytes: the wallpaper's ASTC texture).
- Translator addition (stage 5, same patch): MSL 3.2 program-scope bindings. MercuryPoster declares e.g.
  `texture2d<half, sample> t [[texture(10)]]` at program scope. That becomes an `externally_initialized` addrspace(2)
  global, plus an `!air.global_bindings` entry `!{"air.global_binding", @G, <node>}`. The node is an ordinary
  entry-parameter node with index -1, and the code reads the global with a whole-value load. The ABI decoder now
  keeps these (`AirAbi::global_bindings`). A canon pass (`canon/global_binding.rs`, before SCCP) replaces each load
  with a new entry parameter, one per binding (two globals on one binding share it). From there they bind like
  ordinary arguments at the node's index. Tests: an ABI decode test and
  `tests::engine_arena::a_program_scope_buffer_binding_reads_its_bound_buffer`. MercuryPoster survey: 43/44. The
  last one, `fluidity::fragmentShader`, needs framebuffer-fetch formats, which a pipeline supplies. Only translation
  has been checked: no wallpaper has been rendered yet.

## Render path (stage 5)

- Pipelines: the server borrows `_MTLDevice`'s `newRenderPipelineDescriptorWithSerializedData:` and calls
  `newRenderPipelineStateWithDescriptor:options:reflection:error:`. VKMTLDevice sends `RPS_NEW`: the vertex and
  fragment function ids, 8 colour attachments (format, blending, factors, operations, write mask), depth/stencil
  formats, raster sample count, alpha-to-coverage/one, rasterization enabled, input topology, and the vertex
  descriptor (only the layouts its attributes use). mtlvk builds metal2vulkan's `RenderPipelineDescriptor`. The
  attachment formats are what the translator gets for framebuffer fetch. Depth-stencil states: `DSS_NEW`.
- Encoders: a render encoder records into its own list. `endEncoding` sends `C_RENDER_PASS` (with the store actions
  known by then: an Unknown action becomes Store, or MultisampleResolve if there is a resolve texture), the list
  (`C_RE_*`: pipeline, vertex/fragment buffers/offsets/bytes/textures/samplers (with LOD clamps), viewport, scissor,
  cull, winding, fill, depth clip, depth bias, stencil refs, blend colour, depth-stencil state, useResource, draws
  direct/indexed/instanced/base vertex/base instance), then `C_RE_END`. mtlvk replays this on the runtime's
  `RenderCommandEncoder`. Colour/depth/stencil resolve textures are passed through.
- Textures over memory: `TEX_NEW_BUF` (buffer id + descriptor + offset + bytesPerRow + IOSurface flag) serves
  `newTextureWithDescriptor:iosurface:plane:` and `-[MTLBuffer newTextureWithDescriptor:offset:bytesPerRow:]`.
  MetalSerializer's `validateIOSurfaceDescriptor` checks `bytesPerRow % [device iosurfaceTextureAlignmentBytes]`
  (and a read-only twin). Answering 0 failed every surface ("IOSurface validation failed"), so we answer 16.

## CoreAnimation on Metal (backboardd)

`X86SIM_METAL=backboardd`: backboardd's QuartzCore finds a device and switches its render server to Metal. In a
200 s boot it created 27 render pipelines and 0 failed. They include CA's image/colour-matrix/blend fragment
shaders and the iOS 26 glass shaders (`glass_background_no_bleed_lpf`, `glass_background_sdf_no_bleed_lpf`,
`variable_blur_*`, `downsample_blur_*`). The swapchain's three full-screen BGRA IOSurfaces (from simfb, created in
backboardd) become render targets over their own pages. No command buffer failed and no selector was
unimplemented.

- Screenshots: `23-metal-backboardd-unsynced-present.png` (before the simfb fix: simfb copied the surface at
  `SFBSwapchainSwapSubmit`, while the GPU was still drawing it, so layers were missing) and
  `24-metal-backboardd-home.png` (after). Compared with the software renderer (22/25): the same, except the dock,
  where Metal draws the glass platter (grey over the black wallpaper) and the software renderer draws nothing, and
  the clock's time. Text/icon anti-aliasing differs by a few levels.
- CA passes no fence to `SFBSwapchainSwapAddSurface` (both extra arguments are 0 in `SimDisplay::finish_update`), and
  it never uses `SFBSwapchainAcquireSurfaceFence`. On a Mac the driver orders the host's reads after the GPU's
  writes (implicit IOSurface sync). **simfb now waits for real completion.** It overrides
  `-[MTLSimCommandBuffer commit]` in its own process (backboardd) to add a completed handler to every command
  buffer. A present copies the surface once every command buffer committed before its `SwapSubmit` has completed:
  the handler runs after mtlvk's `EV_CB_DONE`, and the surface's pages then hold the result. There is no fixed
  delay. A newer present supersedes a waiting one, and a 1 s watchdog copies anyway if completions stop.
- CA renders only damaged regions into each of the 3 swapchain buffers (per-buffer damage shapes at
  `SimDisplay+0x6490`). That only works because our textures alias the surfaces' pages: what the GPU wrote last time
  is still there.
- Checked with interaction through host/simhid: app launches, scrolling, page swipes, home button, typing,
  repeated launch/scroll/swipe cycles.
- Settings' row icons are gray placeholders with both renderers (not a Metal problem).

## Robustness (mtlvk missing or dying)

- At boot: boot-a64.sh boots in software if mtlvk doesn't start. If metalhostd starts without mtlvk, it stays in
  *refusing* mode: it holds the service names but cancels every client connection, so MTLSimDriver reports no
  device and processes render in software. It probes for mtlvk every 10 s and exits when it is back, so that
  launchd's next instance serves again.
- mtlvk dies during the boot: metalhostd exits, from the main queue (an `_exit` from the reader thread crashed
  darlingserver). Every Metal client is aborted by MTLSimDriver ("Connection to SimMetalHost ... was lost": Apple's
  behaviour on XPC interruption) and restarted by launchd in software. Tested: the home screen came back about 20 s
  later, without the wallpaper and the glass dock, and touch still worked. Processes stay in software until they
  restart, even after mtlvk is back.

## Wallpaper (MercuryPosterExtension)

- It decodes its ASTC textures with MTLSimDriver's `astc` compute kernel into RGBA8 textures with 3 mips, makes
  texture views of them, and renders the `space::*` cylinder/arc/background pipelines plus `VariableBlurrer`.
- Texture views (`VKMTL_TEX_VIEW` = 22, all three `newTextureViewWithPixelFormat:` variants) were missing and left
  the planet black (31).
- It crash-looped making its snapshot until Darling's commpage got its cache-line bits (vImage computed rowBytes 0).
- Its speckle and magenta rim came from one ASTC texture: MTLSimDriver's `astc` kernel reads `uint4` blocks
  from a KTX mip bound at a 4-aligned offset, and the translator's `uvec4` view dropped the bind residual's low
  bits. 16-byte leaves now use four aligned words (patch, `memory/leaf.rs`). Now it matches a Mac (36, 37).

## Next and known gaps

- Heaps, `newSharedEventHandle` (message 17; not used by the paths tested so far), visibility results, indirect draws, tessellation, tile
  and mesh pipelines (featureProfile 10001 has no tile shaders), `setVertexBuffer:...attributeStride:` (stride
  ignored), several viewports/scissors (only the first), render pipeline reflection (nil) are not implemented.
- Private-storage buffers are host memory (shared) for now; Private textures are real device-local images.
- One mtlvk thread per connection executes requests in order; replies to sync calls wait behind earlier async work.
- Compute gaps: heaps (`useHeap` logged), argument encoders / `newArgumentEncoder*` (shaders reading GPU addresses
  work; Apple's argument-encoder objects are not implemented), indirect dispatch, `setStageInRegion`, pipeline
  reflection (answered nil), binary archives, function pointers / visible function tables, texture `gpuResourceID`
  (ours is the object id, not the runtime's resource ID), private buffers live in host memory.
- Stage 5 needs: `newRenderPipelineDescriptorWithSerializedData:` (borrowed like the compute one) +
  `newRenderPipelineStateWithDescriptor:...` -> mtlvk `new_render_pipeline_state` (vertex descriptors, blend, formats);
  render encoder state/draw commands in the protocol (the runtime has all of them); depth-stencil states;
  IOSurface textures (message 13, needs the cross-process IOSurface service) for CA's render targets;
  tile pipelines stay off with featureProfile 10001.
