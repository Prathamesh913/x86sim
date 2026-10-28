# Plan: Linux Metal stand-in for the iOS 26.5 simulator (Vulkan backend)

See PROTOCOL.md for the transport. Short version: sim apps talk XPC to `com.apple.metal.simulator`. The server is Apple's
`MTLSimImplementation` + `MetalSerializer` deserializer, which replays everything on an `id<MTLDevice>`. Both are in the runtime
as x86_64 iossimulator binaries, and they run under Darling like any other sim dylib.

## Architecture options

**A. Clean-room server (Linux or Darling) that speaks the wire format.** We would reverse-engineer and reimplement
MetalSerializer's object-command and encoder-stream encodings (~120 deserializer entry points, ~150 command decoders, versioned).
Brittle and large. Rejected as the primary route; keep it as a fallback if B hits a wall.

**B. (recommended) Apple's server + our MTLDevice.** One Darling process, `metalhostd` (≈30 lines, the same as
`MTLSimDriverHost`'s main): `xpc_connection_create_listener("com.apple.metal.simulator")` → `init_with_xpc_connection(listener, registryID)`
from the runtime's MTLSimImplementation. In that process only, `MTLCreateSystemDefaultDevice`/`MTLCopyAllDevices` are interposed
(simshim-style `__interpose`, which works since the dyld AMFI fix) to return **`VKMTLDevice`**: our ObjC classes that implement the
`MTLDevice`/`MTLBuffer`/`MTLTexture`/`MTLCommandQueue`/`MTLCommandBuffer`/encoder/pipeline protocols, plus the private selectors
the deserializer uses (`featureProfile`, `registryID`, `newTextureWithDescriptor:iosurface:plane:`, ...). Apple's descriptor classes
(`MTLTextureDescriptor`, `MTLRenderPipelineDescriptor`, ...) stay the real ones from the sim Metal.framework: they are plain data.
Our objects do not call Vulkan in-process (a Darling process can't load Linux libvulkan). They encode a **simple, self-defined
"VKMTL" command protocol** and send it over a Unix socket to:

**`vkmtl` (native Linux process)**: Vulkan device, resource tables, AIR→SPIR-V pipeline cache, executes command buffers,
signals completions back.

Bridge Darling→Linux: Darling processes can make raw Linux syscalls through Darling's `lkm`/`linux_syscall` path
(the same mechanism libsystem_kernel uses). Options in order of preference:
1. Unix domain socket + `SCM_RIGHTS` from a small "linux-side" C helper compiled for Darling that calls Linux `socket/sendmsg` directly
   (check how Darling's `darling-host` / `xtrace` does raw Linux syscalls; vchroot paths map to the prefix).
2. If fd passing is awkward, a fixed shared-memory ring (memfd created by vkmtl, path in the prefix `/dev/shm`) + futex/eventfd.
Shared memory: MTLBuffer contents arrive as xpc_shmem = darlingserver mach memory entry, **memfd-backed**. Add a way to get that memfd
(a darlingserver query, or have VKMTLDevice allocate a memfd itself and `mmap` it as the buffer's backing, then copy-free alias the
client's shmem mapping into it). The second avoids darlingserver changes only if MTLSimImplementation lets us choose the backing.
It does not: it maps the client's shmem and calls `newBufferWithBytesNoCopy`. So **one small darlingserver addition is needed:
"give me the memfd + offset behind this VA range"**. The rule says don't modify Darling now. Record it as a required patch.
In vkmtl, import the memfd as `VkDeviceMemory` via `VK_EXT_external_memory_host` (needs page alignment) or `VK_KHR_external_memory_fd`
(only for memory vkmtl allocated). Fallback: copy on commit / on `didModifyRange`, using the CPU-visible staging mirror.
IOSurfaces: same idea. The IOSurface Remote server (ours, to be written) must back surfaces with memfds that vkmtl can map,
so that CA render targets and the SimFramebuffer surfaces are the same memory.

Option C (rejected): run the server natively on Linux with a Mach/XPC reimplementation. Linux has no Mach IPC, and the sim's
XPC packing + mach memory entries would all need emulation. B uses Darling for all of that.

## Stages

0. **Recon tooling (1-2 d).** On the Mac sim: an interposer dylib that logs every `messageType` + blob sizes for a CA-heavy app and for
   PosterBoard. This gives real traffic, and confirms that the deserializer-side object calls B must implement (log selectors on a
   proxy MTLDevice with `forwardInvocation:` in a Mac-side test of MTLSimImplementation against real Metal).
1. **Handshake (2-3 d).** `metalhostd` in Darling with Apple's MTLSimImplementation and a stub `VKMTLDevice` (`featureProfile` = lowest
   profile that CA accepts, `registryID` constant). Register `com.apple.metal.simulator` in launchd_sim's namespace (job plist in the device
   set, like other host-forwarded services). Gate: `MTLCreateSystemDefaultDevice()` non-nil in a test client. All three connections connect.
   Keep the service disabled by default until stage 5 passes (see risk "half-working device").
2. **Buffers/textures + shared memory (1 wk).** Darling side: VKMTLBuffer over the mapped shmem, VKMTLTexture (descriptor → handle).
   vkmtl: resource tables, memfd import, texture upload (`replaceRegion`) / readback (`getBytes`). Gate: buffer round trip, texture
   replace/getBytes round trip from a sim test app.
3. **Blit + clear (1 wk).** Command buffers, blit encoder (copies, fill, mipmaps), render pass with clear load action and store; completion
   messages (100/101) with times/status; MTLEvent/MTLSharedEvent (timeline semaphores), fences (no-ops/barriers). Gate: clear-to-colour
   into an IOSurface texture that SimFramebuffer shows.
4. **Compute (2 wk).** AIR→SPIR-V for kernels: parse the metallib (PROTOCOL.md §3) and load the AIR bitcode with LLVM (Apple bitcode is
   mostly readable by upstream LLVM ≈ same major; strip/upgrade unknown attributes). Lower `air.*` intrinsics to SPIR-V via a custom
   LLVM→SPIR-V path (the upstream SPIR-V backend or SPIRV-LLVM-Translator) with resource-binding rewriting from `air.*` metadata.
   Metal buffer indices → descriptor set bindings / BDA (buffer device address is the easiest model for `device T*`). Argument buffers →
   BDA tables. Gate: a sample kernel, then CA's blur kernels.
5. **Render pipelines (3-4 wk).** Vertex/fragment AIR→SPIR-V (stage IO via `air.location_index`, `air.position`, `air.render_target`),
   vertex descriptors, blend/depth/stencil state, samplers, `VK_EXT_extended_dynamic_state*`, dynamic rendering. Pipeline cache keyed by
   AIR hash + state. Gate: CA's MetalContext renders the home screen pixel-identical (within tolerance) to the SW path; then enable
   the service by default.
6. **CA compositing in backboardd (1-2 wk).** Needs cross-process IOSurface (Remote server, memfd-backed), IOSurface texture message 13,
   performance (one queue submit per CA frame, avoid stalls on completion). Report a featureProfile without tile shaders /
   imageblocks, or emulate tile pipelines.
7. **Presentation into SimFramebuffer (≤1 wk).** SimFramebuffer display surfaces are IOSurfaces. With memfd backing, vkmtl renders
   straight into the memory host/simfb already reads. Sync: completion before swap. Optional later: vkmtl presents to a Wayland/X window
   directly (zero-copy dmabuf) instead of fb2png dumps.
8. **Wallpaper / RenderBox / apps.** MercuryPoster, IconRendering (RBDevice), SwiftUI drawingGroup, CAMetalLayer apps. Add features as traces
   show them.

## Risks
- **AIR→SPIR-V is the critical path.** No mature open translator exists. AIR is versioned (v28 here) and uses Apple-only intrinsics
  (texture ops, simdgroup, fast-math variants, raytracing). CA's shader set is bounded. Start with it and grow.
- **Undocumented object API surface used by the deserializer.** Private selectors on MTLDevice/resources (e.g. `featureProfile`,
  `newBufferWithBytesNoCopy`, resource ID / GPU address queries, `newFunctionWithSerializedMetallib` callbacks into the device's library
  creation). Mitigation: stage 0 proxy logging on the Mac. `doesNotRecognizeSelector` logging in VKMTL classes.
- **Shared memory under Darling:** xpc_shmem works (icons), but exposing the memfd to Linux needs a darlingserver hook. Without it,
  copies on every commit cost bandwidth (CA frames ≈ 12 MB at 1179x2556 BGRA).
- **IOSurface cross-process** is still unimplemented (emulation mode only). It is a prerequisite for stage 6.
- **Half-working device is worse than none:** once MTLCreateSystemDefaultDevice succeeds, CA, PosterKit and RenderBox switch to Metal and
  will hit any gap. Feature-flag the service (don't register the name) until stage 5.
- **GPU timing/semantics:** Metal's implicit hazard tracking (untracked vs tracked resources) needs conservative Vulkan barriers. Correct first, fast later.
- **Version coupling:** MTLDeserializerVersion and the serializer format are tied to this runtime. That's fine: iOS 26.5 is frozen.
- **Formats:** compressed (ASTC/ETC/BC) via MTLSimDriver's own decompression kernels (`DecodeBGRA_XR10` etc. in its default.metallib)
  or Vulkan support; XR10 IOSurfaces are unsupported in the sim anyway.
