# IOSurface "Remote" protocol (iOS simulator)

Implemented by `iosurfaced.m` here (x86Code stage 5; on by default, `X86CODE_IOSURFACE=0` for emulation). Test:
the probe (the x86_64 one, since removed) (two processes, 14 checks). Answers to the open questions below: a sim-side listener decodes the
client's messages once simshim skips `xpc_connection_enable_sim2host_4sim` for this name, and `xpc_shmem_map` of the
server's `MAP_SHARED|MAP_ANON` memory gives every process the same pages (darlingserver memfd).

Sources:
- Client: sim runtime `IOSurface.framework/IOSurface` (x86_64 slice), disassembled (`/tmp/ios.s` was used; regenerate with llvm-objdump).
- Server: macOS 26 host `IOSurface.framework` (in the dyld shared cache) contains `IOSurfaceRemoteServer` / `IOSurfaceRemoteRemoteClient` and `__IOSurfaceRemoteMethod*` handlers. Disassembly saved at
  `hostfw/macIOSurface/IOSurface.arm64e.dis` (made with `dyld_info -disassemble` on the Mac).
  The host process that hosts it is CoreSimulator's
  `SimRenderingServices.simdeviceio/.../SimRenderServer.xpc` (copy in `hostfw/SimRenderServer/`). It just creates an `IOSurfaceRemoteServer`. The protocol logic lives in IOSurface.framework.

Confidence: **[C]** = read directly from disassembly on both sides. **[I]** = inferred. **[?]** = not sure.

---

## 0. Shortcut: built-in in-process emulation (no server needed)

`__check_needs_emulation` (dispatch_once, runs on the first connect) sets `_ioSurfaceEmulationMode = 1` if **either** env var is present (any value) **[C]**:

- `XPC_NULL_BOOTSTRAP`
- `IOSURFACE_REMOTE_EMULATED`

In emulation mode, `IOSurfaceClientCreate_Emulated` does everything locally. It parses the properties, computes the layout itself (see section 4), allocates memory with `mach_vm_allocate` (or uses `kIOSurfaceClientAddress`), and keeps values in a local CFDictionary. Lock and unlock always succeed. Limits are filled locally (cacheline-1 alignment masks, 0x7fffffff maxima).
**Limitations:** `IOSurfaceClientLookup_Emulated`, `LookupFromMachPort_Emulated` and `CreateMachPort_Emulated` all return 0/NULL. So there is no sharing across processes (`IOSurfaceCreateXPCObject` / `IOSurfaceLookupFromXPCObject` / mach ports all fail). This is enough for one process where QuartzCore software-renders into its own surfaces. Try `IOSURFACE_REMOTE_EMULATED=1` first. Write the server below only when backboardd/app sharing is needed.

---

## 1. Finding and connecting to the service

```
conn = xpc_connection_create_mach_service("com.apple.IOSurface.Remote", NULL, 0);   // flags = 0 [C]
xpc_connection_enable_sim2host_4sim(conn);                                         // [C]
xpc_connection_set_event_handler(conn, ^(xpc_object_t e){...});
xpc_connection_activate(conn);
reply = xpc_connection_send_message_with_reply_sync(conn, { "Method": uint64 0 });  // Initialize
```
- The name is `kIOSurfaceRemoteServiceName = "com.apple.IOSurface.Remote"`. The lookup goes through the normal bootstrap: flags are 0 and the service is not privileged. On a real sim, launchd_sim forwards names it doesn't know to the host. **For Darling: register this name in the bootstrap namespace that the sim processes see.**
- `xpc_connection_enable_sim2host_4sim` only sets `conn->platform (offset 0xc0) = 2` (macOS). It does not pick a different bootstrap. The platform value goes to `_xpc_serializer_pack`, and `send_message_with_reply_sync` checks it **[C]**. What it changes on the wire is **[?]**. It probably tags or translates the message for a host-side peer. If the server is also a sim-runtime process linked with the same sim libxpc, it should decode these messages. Verify early by sending one message.
- Connection setup is one sync message, `Method=0`. If the reply is an XPC error, the connection is not stored and every later create fails. The client reads only `Limits` (data, it copies `min(len,0x28)` bytes into `_ioSurfaceLimits`). It also stores `xpc_connection_get_pid(conn)`.
- Event handler **[C]**:
  - `XPC_ERROR_CONNECTION_INVALID` → prints "IOSurface not available" to stderr.
  - `XPC_ERROR_CONNECTION_INTERRUPTED` → logs "Lost connection to IOSurface Remote Server (%d); unable to recover, exiting process." and calls **`exit()`**. The server must never die or restart.
- One global connection is shared per process.

## 2. Common message rules

- `ErrorCode` must be a **uint64** (`xpc_dictionary_set_uint64`): the client reads it with `xpc_dictionary_get_uint64`, which yields 0 for an int64 **[C]**.
- Every request is a dictionary with `"Method"` (uint64). All requests use `xpc_connection_send_message_with_reply_sync`, except Release, which is usually async (see below).
- Surface-scoped requests (method ≥ 3) also carry `"SurfaceID"` (uint64). `_ioSurfaceSendMessageWithReplySync` adds it from client state +0x04.
- Server dispatch **[C]** (`-[IOSurfaceRemoteRemoteClient _handleMessage:]`):
  - `reply = xpc_dictionary_create_reply(msg)`.
  - Method ≥ 0x1b → `ErrorCode = 0xE00002C7` (kIOReturnUnsupported).
  - Methods 0..2 → handler is called directly.
  - Method 3 → remove this client's reference to SurfaceID. Error is 0 if the surface was found, else `0xE00002F0` (kIOReturnNotFound).
  - Other methods → look up the per-client surface by SurfaceID (`0xE00002F0` if not found). Call the handler, then **always add `"SurfaceState"`** (the 0x48-byte blob, section 3) to the reply. If the request had `"ExtraData"`, it is merged and an `"ExtraData"` value is echoed back (rarely used).
  - Finally `reply["ErrorCode"] = (int64)kr` is always set, 0 on success. Then `xpc_connection_send_message(remote, reply)`.
- Client handling: if `ErrorCode != 0`, return that error and ignore the rest of the reply. Otherwise copy `SurfaceState` (`min(len,0x48)`) into its cached state and run the method's reply block. If there is no connection, the error is `0xE00002CD`.
- Property dictionaries travel as `xpc_data` containing **IOKit binary serialization**: `IOCFSerialize(dict, kIOCFSerializeToBinary=1)`, the OSSerializeBinary format starting `0x000000D3`. They are decoded with `IOCFUnserializeBinary`. The server can call `IOCFSerialize`/`IOCFUnserializeBinary` from the sim's IOKit.framework, since the client uses them too.

## 3. Shared binary structs (little-endian, packed)

### SurfaceState (key `"SurfaceState"`, xpc_data, 0x48 bytes; client copies it to `client+0x30`)
| off | type | field (host getter that fills it) |
|---|---|---|
| 0x00 | u32 | ? (written by a single `stp` with the ID; probably 0 or a version) **[?]** |
| 0x04 | u32 | surface ID (`IOSurfaceClientGetID`) |
| 0x08 | u32 | alloc size |
| 0x0c | u32 | width |
| 0x10 | u32 | height |
| 0x14 | u32 | bytesPerRow |
| 0x18 | u32 | buffer offset. **Base address = mapped shmem + this** |
| 0x1c | u32 | pixel format (OSType) |
| 0x20 | u32 | cache mode |
| 0x24 | u32 | plane count |
| 0x28 | u32 | bytesPerElement |
| 0x2c | u32 | elementWidth |
| 0x30 | u32 | elementHeight |
| 0x34 | u32 | seed (returned by Lock) |
| 0x38 | u32 | subsampling |
| 0x3c | u8  | flags: bit0 allowsPixelSizeCasting, bit1 sysMemOnly, bit2 displayable, bit3 prohibitUseCount, bit4 memoryRegionUsed |
| 0x3d | u64 | traceID (unaligned) |
| 0x45 | 3B  | padding |

### PlaneState (key `"PlaneState"`, xpc_data, 0x48 bytes; client stores it at `client+0x78+i*0x48`, up to 20 planes)
| off | type | field |
|---|---|---|
| 0x00 | u16 | 0 |
| 0x02 | u16 | plane index. **0xFFFF here means "not loaded"**: the client initialises all planes to 0xFFFF and fetches them lazily with Method 6 |
| 0x04 | u32 | width of plane |
| 0x08 | u32 | height of plane |
| 0x0c | u32 | offset of plane (from the mapped base) |
| 0x10 | u32 | bytesPerRow of plane |
| 0x14 | u32 | size of plane |
| 0x18..0x27 | 4×u32 | extended pixels (left/right/top/bottom; exact order **[?]**, use 0) |
| 0x28 | u16 | bytesPerElement |
| 0x2a | u8  | elementWidth |
| 0x2b | u8  | elementHeight |
| 0x2c | u8  | number of components (≤4) |
| 0x2d | u8[4] | component bit depth |
| 0x31 | u8[4] | component bit offset |
| 0x35 | u8[4] | component name |
| 0x39 | u8[4] | component type |
| 0x3d | u8[4] | component range |
| 0x41 | 7B | pad |

Plane getters (`GetWidthOfPlane` and so on) first call `_ioSurfaceEnsurePlaneInfoLoaded`, which sends Method 6. If the plane count is 0 and index 0 is requested, they fall back to the surface-level fields.

### Limits (key `"Limits"`, xpc_data, 0x28 bytes, sent in the Initialize reply)
| off | type | meaning (client use) |
|---|---|---|
| 0x00 | u32 | must be nonzero. The server writes 1. If 0, client-provided memory is treated as unsupported |
| 0x04 | u32 | offset/plane-base alignment **mask** (`GetPropertyAlignment(Offset/PlaneOffset/PlaneBase)` = value+1) |
| 0x08 | u32 | bytesPerRow alignment mask (`GetPropertyAlignment(BytesPerRow)` = value+1) |
| 0x0c | u32 | max bytesPerRow |
| 0x10 | u32 | max width |
| 0x14 | u32 | max height |
| 0x18 | u64 | ? **[?]** |
| 0x20 | u64 | bit0 = server supports `ClientProvidedShmem`. The server writes 1 |

Suggested values: `{1, 63, 63, 0x7fffffff, 0x7fffffff, 0x7fffffff, 0, 1}`. Emulation mode uses cacheline-1 for both masks.

## 4. Methods

Keys are the strings in `_IOSurfaceRemoteKey*`. Types: u64 = `xpc_dictionary_set_uint64`, data = xpc_data, port = mach_send.
Every reply also carries `ErrorCode` (u64), and for methods ≥ 4 also `SurfaceState` (data).

| # | name | request keys (besides Method) | reply keys (besides ErrorCode/SurfaceState) |
|---|---|---|---|
| 0 | Initialize | — | `Limits` data[0x28], `ProtocolVersion` u64 = 1 |
| 1 | GetDebugInfo | — | `DebugSurfaceIDList` array<u64>, `DebugSurfaceRefCount` array<u64> |
| 2 | Create / Lookup | one of: `PropertiesDictionary` data (create), `LookupID` u64 (IOSurfaceLookup), `MachPort` port (LookupFromMachPort). Optional: `ClientProvidedShmem` (xpc_shmem), `ExtraData` | **`MemoryObject` xpc_shmem** (required), `SurfaceState` data (required) |
| 3 | Release | `SurfaceID` | — |
| 4 | Lock | `SurfaceID`, `Options` u64 (kIOSurfaceLockReadOnly=1, AvoidSync=2) | SurfaceState. The client returns state.seed as the seed |
| 5 | Unlock | `SurfaceID`, `Options` u64 | SurfaceState (seed) |
| 6 | GetPlaneState | `SurfaceID`, `PlaneIndex` u64 | `PlaneState` data[0x48]. Error 0xE00002C2 if index ≥ planeCount (index 0 with planeCount 0 is allowed by the host check `planeCount >= idx`) |
| 7 | BindAccel | `AcceleratorID` u64, `AcceleratorField` u64 | — |
| 8 | NeedsBindAccel | `AcceleratorID`, `AcceleratorField` | `NeedsBindAccelerator` bool |
| 9 | SetValue | `PropertiesDictionary` data = serialized `{"key": key, "value": value}` (IOSurfaceSetValue), or `{"isSetValues": true, "value": dict}` (IOSurfaceSetValues with key NULL: merge the dict) **[C]** (checked against `_IOSurfaceClientSetValue`'s CFSTRs, x86Code stage 5) | — (server: `IOSurfaceClientSetValue(surface, dict)`; error 0xE00002C2 if it can't be deserialized) |
| 10 | CopyAllValues | — | `PropertiesDictionary` data (serialized dict of all values) |
| 11 | RemoveValue | `PropertyKey` string | — |
| 12 | RemoveAllValues | — | — |
| 13 | SetPurgeable | `NewState` u64 | `OldState` u64 |
| 14 | CreateMachPort | — | `MachPort` mach_send |
| 15 | IncrementUseCount | — | — |
| 16 | DecrementUseCount | — | — |
| 17 | IsInUse | — | `IsInUse` bool |
| 18 | CopyDataValueBytes | `PropertyKey` string, `BufferSize` u64 | `Buffer` data, `BufferSize` u64 |
| 19 | SignalEvent | `EventValue` u64, `EventOperation` u64, `EventPort` port | — |
| 20 | SetBulkAttachments | bulk keys (below) | — |
| 21 | GetBulkAttachments | — | bulk keys |
| 22 | IncrementUseCountForCategory | `Category` u64 | — (client falls back to Method 15 on failure) |
| 23 | DecrementUseCountForCategory | `Category` u64 | — (fallback to 16) |
| 24 | IsInUseForCategory | `Category` | `IsInUse` bool |
| 25 | IsInUseForAnyOtherCategory | `Category` | `IsInUse` bool |
| 26 | GetYCbCrMatrix | — | `YCbCrMatrixIndex` u64 |

Bulk attachment keys: `CleanAperture`, `PixelAspectRatio`, `MasteringDisplayColorVolume`, `GammaLevel`, `EDRFactor`, `ContentLightLevelInfo` (data structs), and `FieldCount`, `FieldDetail`, `YCbCrMatrixIndex`, `ColorPrimariesIndex`, `TransferFunctionIndex`, `ChromaLocationTopField`, `ChromaLocationBottomField`, `ChromaSubsampling`, `PremultipliedAlpha` (u64). The struct is `IOSurfaceColorAndSpatialKeys`, 0x84 bytes.
Other keys defined but not seen in any client request: `GetExtraData`, `SetExtraData`, `GetYCbCrMatrix` (string key), `ExtraData`.

### Method 2 details
Client (`_ioSurfaceClientCreate(props, lookupID, machPort)`) **[C]**:
1. Allocates a 0x618-byte client struct and sets all 20 plane markers to 0xFFFF.
2. With props: if `kIOSurfaceClientAddress` is present, it requires `kIOSurfaceAllocSize`. If the Limits allow it (limits[0] != 0 and limits+0x20 bit0), it sends `ClientProvidedShmem = xpc_shmem_create(addr, allocSize)` and removes ClientAddress from the dict. Otherwise it logs "server does not support" and memcpy's afterwards. It then serializes the dict as `PropertiesDictionary`.
3. Else if lookupID != 0: `LookupID`. Else if the machPort is valid: `MachPort`.
4. On the reply: if `ErrorCode != 0` or there is no `MemoryObject`, it fails (returns NULL). Otherwise:
   `size = xpc_shmem_map(MemoryObject, &client->base)`. It **asserts** size != 0 ("Failed to create shared memory..."). Then it syncs `SurfaceState`.
5. Base address = `client->base + state.offset(0x18)`. Plane base = `client->base + plane.offset(0x0c)`. The client never computes the layout itself in remote mode. It is all from SurfaceState/PlaneState.
6. Release (Method 3): sync when in test mode, otherwise `send_message_with_reply` async. Then it does `munmap(base, size)` and `free`.

Host server **[C]**: lookup → `IOSurfaceClientLookup(id)` / `IOSurfaceClientLookupFromMachPort(port)` (error 0xE00002F0 if NULL). Create → deserialize the properties (0xE00002C2 if that fails). If ClientProvidedShmem is present, it maps it and sets `kIOSurfaceClientAddress`. Then `IOSurfaceClientCreate`. Then
`MemoryObject = xpc_shmem_create(unadjustedBaseAddress, allocSize)`, plus the per-client reference and SurfaceState.

### Cross-process sharing (client side) **[C]**
- `IOSurfaceCreateMachPort(s)` → Method 14 → `xpc_dictionary_copy_mach_send(reply,"MachPort")`.
- `IOSurfaceCreateXPCObject(s)` = `xpc_mach_send_create(IOSurfaceCreateMachPort(s))`, then it deallocates its own port ref.
- `IOSurfaceLookupFromXPCObject(o)` requires `XPC_TYPE_MACH_SEND`. It calls `xpc_mach_send_get_right` → `-[IOSurface initWithMachPort:]` → Method 2 with `MachPort`.
- `IOSurfaceLookup(id)` → Method 2 with `LookupID`.
- So a port is just a token that the server mints and recognises. Each process maps the same `MemoryObject` shmem.

## 5. Layout (needed by our server; copied from `IOSurfaceClientCreate_Emulated`) **[C]**
```
W = kIOSurfaceWidth, H = kIOSurfaceHeight, PF = kIOSurfacePixelFormat, cache = kIOSurfaceCacheMode
bpe = kIOSurfaceBytesPerElement (default 1); ew = ElementWidth (default 1); eh = ElementHeight (default 1)
subsampling = kIOSurfaceSubsampling (0)
flags.bit0 = kIOSurfacePixelSizeCastingAllowed; flags.bit2 = kIOSurfaceIsDisplayable
planes = kIOSurfacePlaneInfo array (<32), parsed by __parsePlaneInfo: per-plane Width/Height/BytesPerElement/
         ElementWidth/ElementHeight/BytesPerRow/Offset/Size (kIOSurfacePlane*)
offset = kIOSurfaceOffset (0)
bpr = kIOSurfaceBytesPerRow if given else align64(((W + ew - 1)/ew) * bpe)
alloc = kIOSurfaceAllocSize if given
        else planes ? align64(lastPlane.offset + lastPlane.size)
                    : align64(H*bpr + offset)
assert alloc != 0 and alloc >= offset + (H-1)*bpr + ((W+ew-1)/ew)*bpe
```
QuartzCore/CoreAnimation mostly passes explicit BytesPerRow/AllocSize/PixelFormat ('BGRA', bpe 4). For planar formats, compute plane offsets sequentially with align64. Plane formats are **[I]**, but the software renderer rarely uses them.

## 6. Recommended minimal server

Run it as a sim-runtime process under Darling (so it uses the same libxpc/IOKit as the clients). Register the name `com.apple.IOSurface.Remote` via a launchd plist with MachServices, or with `xpc_connection_create_mach_service(name, q, XPC_CONNECTION_MACH_SERVICE_LISTENER)`.

State:
```
struct Surf { u32 id; void *mem; size_t alloc; xpc_object_t shmem; SurfaceState st; PlaneState planes[n];
              CFMutableDictionaryRef values; mach_port_t port /*receive right, lazily*/; int refs;
              int useCount; u32 seed; };
map<u32,Surf*> byID; map<mach_port_t,Surf*> byPort;
per-connection: map<u32,int> refs   // release all on XPC_ERROR_CONNECTION_INVALID
```
Handler, on every message:
- `reply = xpc_dictionary_create_reply(msg)`. Switch on `Method`. Always set `ErrorCode` (0 = OK). For methods ≥ 4, append `SurfaceState`. Then `xpc_connection_send_message(peer, reply)`.
- **0**: reply `Limits` (see the suggested values above) and `ProtocolVersion=1`.
- **2**:
  - Create: `IOCFUnserializeBinary` the `PropertiesDictionary`. Compute the layout (section 5). Get page-aligned memory with `mach_vm_allocate(round_page(alloc))`, or map `ClientProvidedShmem`. `shmem = xpc_shmem_create(mem, round_page(alloc))`. Assign an ID (start at 1, never 0). Fill `st` (offset=0, seed=0, flags from props) and refs++. Reply `MemoryObject` + `SurfaceState`.
  - Lookup: `byID[LookupID]`, or for `MachPort` copy the right (`xpc_dictionary_copy_mach_send`) and look it up in `byPort`. Our own receive right comes back under the same name. Deallocate the extra send ref. Then add a ref for this connection and reply with the same `MemoryObject` shmem and SurfaceState. Unknown surface → `0xE00002F0` with no MemoryObject.
- **3**: refs--. Free when the total reaches 0 (also drop the port and the byPort entry). Reply ErrorCode (the client ignores it).
- **4/5**: Lock is a no-op. On Unlock without ReadOnly (Options & 1 == 0), seed++. Reply SurfaceState. (Seed semantics are close enough for CA.)
- **6**: reply `PlaneState` for PlaneIndex. For a non-planar surface at index 0, synthesize it from the surface fields.
- **9/10/11/12/18**: keep `values` (CFDictionary). 9 merges the deserialized dict. 10 serializes with `IOCFSerialize(...,1)` into `PropertiesDictionary`. 18 returns `Buffer` = bytes of the CFData value and `BufferSize` (or 0xE00002BD if too small).
- **14**: create a receive right (`mach_port_allocate`), insert a send right, store it in `byPort`, reply `xpc_dictionary_set_mach_send("MachPort", port)`.
- **15/16/22/23**: adjust useCount. **17/24/25**: reply `IsInUse` = useCount>0. **13**: reply `OldState`=0 (kIOSurfacePurgeableNonVolatile). **8**: `NeedsBindAccelerator`=false. **21**: reply nothing (client uses defaults) **[?]**. **26**: `YCbCrMatrixIndex`=0. **1**: empty arrays. Anything else: return 0 or 0xE00002C7.
- Never exit: clients `exit()` when they lose the connection.

Open questions / risks:
1. The wire effect of `sim2host` (platform=2) on the serializer **[?]**. Test that a sim-side listener decodes a message from a sim2host client.
2. SurfaceState +0x00 contents **[?]** (likely unused by the client; set it to 0). The meaning of Limits +0x18.
3. Whether `xpc_shmem_map` works in Darling's libxpc/mach for memory crossing processes. With emulation mode this is not needed.
4. `__parsePlaneInfo` details are not reproduced here. Disassemble it at `_IOSurfaceClientCreate_Emulated` → `__parsePlaneInfo` in the client if planar formats are needed.
