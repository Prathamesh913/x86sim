# Metal Simulator transport ("com.apple.metal.simulator")

Sources (all x86_64 slices unless noted):
- Client: RuntimeRoot `System/Library/PrivateFrameworks/MTLSimDriver.framework/MTLSimDriver`
  (+ `MetalSerializer.framework`, loaded by sim `Metal.framework` with dlopen).
- Server: **the sim runtime ships the server code too.** `MTLSimImplementation.framework`
  (platform iossimulator, exports `init_with_xpc_connection`) and
  `MTLSimDriver.framework/XPCServices/MTLSimDriverHost.xpc/MTLSimDriverHost`.
  On the Mac the same code is in `/System/Library/PrivateFrameworks/MTLSimImplementation.framework` (macOS build, shared cache).
- Mac host plumbing: `/Library/Developer/PrivateFrameworks/CoreSimulator.framework/Versions/A/Resources/SimRenderingServices.simdeviceio/`
  with `XPCServices/SimRenderServer.xpc` (registers `com.apple.metal.simulator` for the device, Swift class `MetalIsolate`)
  and `XPCServices/SimMetalHost.xpc` (one process per device, "metal isolate"; it dlopens `MTLSimImplementationPath` and calls
  `init_with_xpc_connection`).
- Disassembly used: `/tmp/mtl/{impl,drv,metal}.s` (regenerate with `llvm-objdump --macho --arch=x86_64 -d --no-show-raw-insn`).

Confidence: **[C]** = read from disassembly. **[I]** = inferred. **[?]** = open.

---

## 0. Key finding

The protocol is **XPC dictionaries carrying opaque blobs made by Apple's `MetalSerializer`**: object descriptors
and per-command-buffer encoder streams. The server side (`MTLSimImplementation` + the `_MTLDeserializer` class in
`MetalSerializer`) decodes the blobs and **replays them against a normal `id<MTLDevice>`**. Both halves exist as
iossimulator x86_64 binaries in the runtime. So the cheapest stand-in is **not** to reimplement the wire format.
Run Apple's `MTLSimImplementation` in a Darling process, and give it an `MTLDevice` that we implement on Vulkan
(see PLAN.md, option B). The wire details below are needed for debugging, and for option A (a full clean-room server).

## 1. Client-side architecture

- `Metal.framework` (sim) → `MTLCreateSystemDefaultDevice` dlopens `/System/Library/PrivateFrameworks/MTLSimDriver.framework/MTLSimDriver`
  ("could not open MTLSimDriver!") and instantiates `MTLSimDevice` **[C]**.
- MTLSimDriver classes **[C]**: `MTLSimDevice` (160 methods), `MTLSimCommandQueue`, `MTLSimCommandBuffer`, `MTLSimBuffer`,
  `MTLSimTexture`, `MTLSimHeap`, `MTLSimResource`, `MTLSimFunction`, `MTLSimFunctionVariant`, `MTLSimRenderPipelineState`,
  `MTLSimComputePipelineState`, `MTLSimDepthStencilState`, `MTLSimSamplerState`, `MTLSimArgumentEncoder`,
  `MTLSimIndirectArgumentBufferLayout`, `MTLSimSharedEvent`, `MTLSimSharedEventHandle`, `MTLSimFence`,
  `MTLSimBlitCommandEncoder`, `MTLSimRenderCommandEncoder`, `MTLSimOperation`, `MTLSimCommandStoragePool`,
  `MTLSimBufferStoragePool`. The encoders are thin: they subclass or wrap `MTLSerializer*CommandEncoder` from MetalSerializer,
  which writes the binary command stream.
- `-[MTLSimDevice init]` (0x2cea) opens **three XPC connections** with `createNewConnection(type, &processRef, &deserializerVersion, &registryID, &featureProfile, handler)` **[C]**:
  | connectionType | ivar | notes |
  |---|---|---|
  | 0 | `_mainConnection` | processRef=0 on input; server allocates processRef; reply gives registryID + hostFeatureProfile; handler `handleMainConnectionReplies` |
  | 1 | `_resourceConnection` | same processRef; resource ops (buffers, getBytes) |
  | 2 | `_compilerConnection` | same processRef; functions/pipelines ("Compiler failed with XPC_ERROR_*") |
- **Service lookup** (`createNewConnection`, 0x30a0) **[C]**:
  1. `name = "com.apple.metal.simulator." + getprogname()`; `bootstrap_look_up(bootstrap_port, name)`. If that succeeds, use it (port deallocated, name kept).
  2. Otherwise use `"com.apple.metal.simulator"`.
  3. `xpc_connection_create_mach_service(name, queue, 0)`, `xpc_connection_enable_sim2host_4sim(conn)` (a stub in Darling's libxpc, which is fine), set event handler, activate.
- **Handshake** (sync, `xpc_connection_send_message_with_reply_sync`) **[C]**:
  ```
  request: { messageType: u64 0, connectionType: u64 {0|1|2}, processRef: u64, version: u64 1 }
  reply:   { processRef: u64, version: u64 (= MTLDeserializerVersion of server), registryID: u64,
             hostFeatureProfile: u64, [error: u64] }
  ```
  On an XPC error reply ("XPC_ERROR_CONNECTION_INVALID", etc.) `createNewConnection` returns NULL. Then
  `-[MTLSimDevice init]` returns nil. So **MTLCreateSystemDefaultDevice() == nil today**. That is why
  CA falls back to `CA::OGL::SWContext`, IconRendering says "No Metal renderer available", and MercuryPoster
  fails with `WallpaperCoordinatorError.unavailable`.
- **Capabilities**: there is no big caps struct. The client gets one enum, `hostFeatureProfile` (`MTLDeviceFeatureProfile`).
  Server side, it comes from `[device featureProfile]` (private selector). Sim `Metal.framework` maps it with
  `getSupportedFamilies(MTLDeviceFeatureProfile)` (0xc828f): valid values 0..12 and >10000. The highest case pushes
  Apple9/Apple8/.../Mac2/Common3 families. Limits are then answered locally from tables keyed by the profile.
  Anything dynamic goes through `operationWithReply` (section 2, type 12) **[C/I]**. The serializer is created with
  `deserializerVersion` from the reply. The client must get the version it was built for. Use the value of
  `_MTLDeserializerVersion` from the runtime's own MetalSerializer (`__TEXT,__const` 0x21ca0 in the x86_64 slice) **[I]**.

## 2. Wire protocol (server dispatch: `MTLSimulatorHandleEvent` block, MTLSimImplementation 0x2f70)

Every message is an XPC dictionary with `messageType` (u64). The server keeps per-connection context →
`MTLSimApplicationContext` (shared by processRef across the 3 connections, registry `MTLSimGlobalContext`).
Exceptions thrown while handling a message become reply `{ fatalError: string }` **[C]**.

| messageType | handler | request keys | reply |
|---|---|---|---|
| 0 | connect | connectionType, processRef, version | processRef, version, registryID, hostFeatureProfile |
| 1 | `newBufferWithLength` | length, bufferIndex, options, sharedMem (**xpc_shmem**, `xpc_shmem_map` on server) | bufferRef or error "failed to allocate buffer" |
| 6 | `submitCommandBuffer` (async) | commandQueue, commandBufferRef, segments (**xpc_array of xpc_data**, one per serialized encoder segment) | none; later async messages (below) |
| 7 | `newCommandQueueWithDescriptor` | maxCommandBufferCount | commandQueueRef / index, or error "failed to create command queue" |
| 8, 9 | `newObjectCommand_DEPRECATED` | command (data) | |
| 10 | `getBytes` (texture readback) | command (data), sharedMem (xpc_shmem destination) | empty reply when done |
| 12 | `operationWithReply` (sync query) | command (data) | replyData (data) / fatalError |
| 13 | `newIOSurfaceTexture` | iosurface (`IOSurfaceCreateXPCObject`), command data | |
| 14 | `deleteObject` (async) | command (data) | none |
| 15 | `newFunction` | functionIR (data: serialized metallib + command data) | |
| 16 | `notificationWithListener` | (event ref + value in command) | later async `{messageType:16, notificationRef}` |
| 17 | `newSharedEventHandle` | eventRef | labelTraceID, eventPort (mach send) / error |
| 18 | `newSharedEventWithHandle` | eventPort (mach send), label | |
| 19, 20 | no-op | | |
| 21 | `newSharedEventWithMachPort` | eventPort | |
| 50 (0x32) | fire-and-forget object command | command (data) → `[deserializer <sel>]` (probably `newObjectCommandWithSerializedData:serializedDataSize:`) **[I]** | none |
| 51 (0x33) | `newOperationAsyncWithReply` | command | reply handed off from worker queue "com.apple.Metal.AsyncWithReplyWorker" (replyData) |
| 200 | `reportLeaks` | | runs /usr/bin/leaks |
| 12345 | ping | | `{reply: 12}` |
| 12346 | `wait` | waitTime | reply after sleep (test hook) |
| other | `abort()` | | |

Server → client async messages (on the main connection) **[C]**:
- `{messageType: 100, ...}` from the submitCommandBuffer path, sent first. It is the "submission completed / scheduled" callback **[I]**.
- `{messageType: 101, commandBufferRef, startTime: double, endTime: double, status: u64, error: u64}`, the completion
  (GPU times, `MTLCommandBufferStatus`, `MTLCommandBufferError*` code). GPU faults → server aborts with
  "The Metal Simulator workload (version=%u) from simulator process %s (PID=%d) encountered GPU execution error".
- `{messageType: 16, notificationRef}`, the shared-event listener notification.

### Objects and references
All objects are named by client-allocated 64-bit refs (`MTLSerializer` objectRefAllocator, `allocateObjectRef`/`releaseObjectRef:`).
Server maps ref→object (`register*ForReference:` / `get*ForReference:` / `delete*ForReference:` in `_MTLDeserializer`).
Most object creation does **not** have its own messageType. It is a serialized "object command" that
`_MTLDeserializer newObjectCommandWithSerializedData:serializedDataSize:` / `operationWithSerializedData:...` dispatch
internally. The full set of server operations (from `_MTLDeserializer`) **[C]**:
newBufferWithHeap, newTextureWithDescriptor(:allocator), newTextureWithHeap, newTextureWithIOSurface:serializedData:,
newTextureView{WithPixelFormat,WithTextureType,WithSwizzle,WithBuffer}, newTiledTextureViewWithBuffer,
newSamplerStateWithDescriptor, newDepthStencilStateWithDescriptor, newHeapWithDescriptor, newFence, newSharedEvent(WithOptions),
newSharedEventWithEventPort:label:, newFunctionWithSerializedMetallib:commandData:commandDataSize:overrideTargetTriple:,
newRenderPipelineStateWithDescriptor:serializedDataSize:, newComputePipelineStateWithDescriptor:serializedDataSize:,
newArgumentEncoderWithBufferLayout, newArgumentBufferLayoutWithStructType, replaceRegion:, didModifyRange:, getBytes:dataSize:command:,
sharedEvent{SignaledValue,SetSignaledValue,WaitSignaledValue}, notifyListenerForEvent..., heap size queries,
minimumLinearTextureAlignment, minimumTextureBufferAlignment, bufferGPUAddress, uniqueIdentifier, doResourcesAlias,
makeAliasiableResource, computeInfo, argumentEncoderSetValues, deleteObjectWithSerializedData, and
`submitCommandSegments:lengths:count:submissionCompleted:commandBufferCompleted:commandQueueRef:`.

### Memory
- **MTLBuffer**: the client allocates, then `xpc_shmem_create` → `sharedMem` in message 1. Server `xpc_shmem_map`s it and wraps
  it in `newBufferWithBytesNoCopy`. Shared/managed storage is therefore *the same pages* in both processes, and CPU writes
  need no transfer (`didModifyRange:` exists for managed). Under Darling, xpc_shmem = mach memory entry, memfd-backed
  in darlingserver. A Linux process can map the same memfd.
- **Textures**: private storage is server-only. `replaceRegion:` carries bytes in the command data. `getBytes` copies into a
  client shmem. IOSurface-backed textures (CoreAnimation render targets, drawables) send the IOSurface as XPC object
  (`IOSurfaceCreateXPCObject` → server `IOSurfaceLookupFromXPCObject`) plus the serialized descriptor/plane.
  Unsupported: "IOSurface backed XR10 textures are not supported in the simulator". **This needs cross-process IOSurface**
  (`com.apple.IOSurface.Remote`, docs/protocols/IOSURFACE.md). `IOSURFACE_REMOTE_EMULATED=1` makes IOSurfaceCreateXPCObject fail.

### Command stream (MetalSerializer)
- A command buffer is a list of **segments**, one blob per encoder: `MTLSerializerRenderCommandEncoder` / `Compute` / `Blit` /
  `ParallelRender`, plus event signal/wait records. The server walks them with `-[_MTLDeserializer decodeSegments:lengths:count:into:]` →
  `decodeSegmentWithHeader:withIterator:withDecoder:into:`. A per-type decoder (`MTLDeserializerRenderDecoder`, `...ComputeDecoder`,
  `...BlitDecoder`, `...EventDecoder`) reads the segment header (`decodeWithHeader:`, render pass descriptor via
  `newDescriptorFromVersion1:`) and then a sequence of fixed-size commands (`readCommand:withIterator:expectedSize:into:`,
  variable ones via `readVariableCommand:...maxExpectedSize:`). Iterator: `MTLByteIterator` (`advance:`, `readBytes:into:`, `bytesLeft`).
- Command kinds (decoder selectors) **[C]**:
  - Render: setRenderPipelineState, set{Vertex,Fragment}{Buffers,BufferOffset,Textures,SamplerState(s)(LODClamp)}, setDepthStencilState,
    setBlendColor, setCullMode, setFrontFacingWinding, setDepthBias, setDepthClipMode, setScissorRect(s), viewport, setStencilRef,
    setTriangleFillMode, setLineWidth/PointSize/AlphaTestReferenceValue/ClipPlane, draw{Primitives,IndexedPrimitives,Instanced*,BasePrimitives,Patches}{16,64},
    *Indirect, executeCommandsInBuffer(Ranged), set{Color,Depth,Stencil}StoreAction(Options), set*ResolveTexture, fences, barriers, useResource(s)/useHeap(s).
    The 16/64 suffix variants are compact encodings for small vs. large counts/offsets.
  - Compute: setComputePipelineState, setBuffers/BufferOffset, setTextures, setSamplers(LODClamp), setThreadgroupMemoryLength,
    setStageInRegion(Indirect), dispatchThreads, dispatchThreadgroups(Indirect), barriers, fences, useResources/useHeaps.
  - Blit: copy buffer↔buffer/texture, texture↔texture (incl. NumSliceLevel/Options), fillBuffer, generateMipmaps, optimize*,
    synchronizeResource/TextureImage, fences.
  - Event: signalEvent, waitForEvent, waitForEventTimeout.
- The byte layout of each command is **not yet extracted**. It is only needed for option A. Get it by disassembling the
  `MTLSerializer*Encoder` methods (writer side) next to the matching `decode*` (reader side) in MetalSerializer, or by
  recording blobs from a client on the Mac sim (an interposer on `xpc_connection_send_message` inside the sim app).

### Drawables / presentation
There is no presentation message. `CAMetalLayer` drawables are IOSurface-backed `MTLSimTexture`s (message 13). CA/backboardd
composites into IOSurfaces that are the SimFramebuffer display surfaces. "Present" = the GPU finishes writing the IOSurface
(completion 101), then the normal SimFramebuffer swap.

## 3. Shader format

- The sim `Metal.framework` + in-runtime `MTLCompilerService.xpc` / `GPUCompiler.framework` compile source or load metallibs
  **client-side to AIR only**. The "compiler output" passed to `-[MTLSimDevice newFunctionVariantWithCompilerOutput:functionType:]`
  is sent as `functionIR` (message 15, compiler connection) **[C]**. The server calls
  `newFunctionWithSerializedMetallib:commandData:commandDataSize:overrideTargetTriple:@"air64-apple-macosx10.14.0"` **[C]**:
  the AIR bitcode is retargeted from `air64_v28-apple-ios26.5.0-simulator` to macOS and compiled by the host Metal compiler.
  Function constants / specialization are resolved client-side (CA specializes with function constants: "Metal failed to specialize
  fragment function").
- Container: **metallib**, e.g. `MTLSimDriver.framework/default.metallib` header:
  `"MTLB"`, u16 0x0001 (target/platform), u16 2, u16 9 (file format version **2.9**?), u8 0 (type = executable), u8 0x87
  (OS = iOS | 0x80 simulator), u16 26, u16 5 (OS 26.5), u64 file size, then function-list / public-metadata /
  private-metadata / bitcode offset+size pairs. Each function = tagged entry (NAME, TYPE, HASH, OFFT, VERS, ...) and an
  LLVM bitcode module (wrapper magic 0x0B17C0DE). Producer "Apple metal version 32023.884 (metalfe-32023.884)". **[C header bytes / I field names]**
- Reflection: AIR metadata in the bitcode (`air.buffer`, `air.location_index`, `air.arg_type_size`, `air.texture`, `air.read_write`,
  `air.struct_type_info`, `air.thread_position_in_grid`, `air.max_*`...). Pipeline creation goes through
  `newRenderPipelineStateWithDescriptor:serializedDataSize:` (descriptor + function refs). Reflection is produced server-side when
  the client asks (MTLSerializer `reflectionSerializationVersion`). Argument encoders and buffer layouts are server objects
  (`newArgumentEncoderWithBufferLayout`, `argumentEncoderSetValues`).

## 4. CoreAnimation

- QuartzCore (sim) has `CA::OGL::MetalContext` (220 symbols), `SWContext`/`SW*` (software rasterizer), `GLESContext`, `NullContext`.
  With a device, CA renders layer trees with Metal (its own `metallib` from the QuartzCore bundle, "com.apple.coreanimation.metal-library",
  render + compute + tile pipelines, `MetalBufferPool`) into IOSurface-backed textures. Without one it uses `SWContext`, which works today
  (boot logo, home screen).
- **Minimum subset** for a working device:
  - backboardd/CA: handshake; buffers (shmem); IOSurface textures + private 2D textures (BGRA8/RGBA8/R8/RG8/RGBA16F, MSAA unlikely);
    samplers; render pipelines with blending; load/store actions (clear); draw (indexed) primitives; blit copies; a few compute kernels
    (blur/filters, `Metal failed to build compute pipeline`); fences/events; completion callbacks. Tile shaders appear
    ("render pipeline with tile shader"). Gate them by reporting a lower `featureProfile`, or CA will expect Apple-GPU tile features.
  - MercuryPoster/PosterKit wallpaper: only needs `MTLCreateSystemDefaultDevice() != nil` to get past
    `WallpaperCoordinatorError.unavailable`. Then real rendering (RenderBox/RB, shaders, compute).
  - IconRendering / RenderBox (`RBDevice`): same as CA (render + compute).
  - Typical UIKit apps: nothing directly; they get Metal through CA if backboardd uses Metal. Apps with their own `CAMetalLayer`/SwiftUI
    `.drawingGroup` need the full path.
- Important: CA in **each** app process (not only backboardd) may pick Metal for offscreen work once a device exists. A device that
  appears and then fails is worse than none. Stage gates should keep `MTLCreateSystemDefaultDevice` nil (service absent) until the
  CA subset passes.
