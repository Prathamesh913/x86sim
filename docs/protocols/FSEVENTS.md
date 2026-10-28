# fseventsd: com.apple.FSEvents

`host/fseventsd/fseventsd.c` stands in for the Mac host's fseventsd. A simulator's `FSEvents.framework`
(`/System/Library/PrivateFrameworks/FSEvents.framework`) looks up `com.apple.FSEvents` with `bootstrap_look_up2` like a
Mac process does; the job `com.x86code.fseventsd` (cli/libexec/rundev.sh) registers that name in the simulator's
launchd. Events come from Linux inotify through Darling's private syscall 598 (`x86code_inotify.c` in darling-xnu).

Clients seen at boot: cfprefsd (Preferences directories, file events), profiled, nsurlsessiond, navd, MobileAsset,
searchd/corespotlight ("/" with exclusions), and LocalStorageFileProvider (the On My iPhone domain: volume UUID,
"since" streams for catch-up, file-event streams for live updates in Files).

The protocol is MIG, reconstructed from the runtime's FSEvents.framework stubs (`f2d_*_rpc`, `FSEventsD2F_server`,
`implementation_callback_rpc`). Requests carry a send-once reply port (`mig_get_special_reply_port`); replies are
`id + 100`.

## f2d (client to daemon), subsystem 66000

All structures are packed to 4 bytes; offsets are from the start of the message.

| id | routine | request | reply |
|---|---|---|---|
| 66000 | `f2d_register_rpc` (to the service port) | complex, 5 descriptors (below) | complex: 1 port descriptor (`MOVE_SEND`), the stream's private port |
| 66001 | `f2d_flush_rpc` (stream port) | header only | NDR, RetCode, u64 event id |
| 66002 | `f2d_get_server_uuid_rpc(dev)` | NDR, int dev | NDR, RetCode, char[36] UUID string ("NO-UUID..." = none) |
| 66003 | `f2d_get_current_event_id_rpc` | header only | NDR, RetCode, u64 |
| 66004 | `f2d_get_last_event_for_device_before_time_rpc(dev, time)` | NDR, int dev, f64 time | NDR, RetCode, u64 |
| 66005 | `f2d_purge_events_for_device_up_to_event_id_rpc(dev, id)` | NDR, int dev, u64 | NDR, RetCode, u32 |
| 66006 | `f2d_unregister_rpc` (stream port) | header only | NDR, RetCode |

`f2d_v1_register_rpc_v1` (the client's fallback when 66000 fails) is not answered.

Register request (0xa0 bytes):

```
0x00 header (bits: complex, COPY_SEND / MAKE_SEND_ONCE), id 66000
0x18 descriptor count = 5
0x1c port descriptor: the client's d2f receive port (MAKE_SEND): events go there
0x28 OOL: path offsets, u32[npaths] into the path blob
0x38 OOL: path blob, NUL-terminated paths
0x48 OOL: excluded path offsets, u32[nexcl]
0x58 OOL: excluded path blob
0x68 NDR
0x70 int dev           0 = absolute paths; else paths are relative to that device's mount point
0x74 int npaths
0x78 u32 offsets count, 0x7c u32 path blob size
0x80 int nexcl, 0x84 u32 excluded offsets count, 0x88 u32 excluded blob size
0x8c f64 latency
0x94 u32 FSEventStreamCreateFlags (0x10 FileEvents, 0x40 UseExtendedData, 0x100 WithDocID, ...)
0x98 u64 sinceWhen     UINT64_MAX = kFSEventStreamEventIdSinceNow
```

## d2f (daemon to client), subsystem 67000

`callback_rpc` (67000), sent to the client's d2f port (0x78 bytes; we send it without a reply port):

```
0x00 header (complex), id 67000
0x18 descriptor count = 4
0x1c OOL: path offsets, u32[n]
0x2c OOL: raw event flags, u32[n]
0x3c OOL: event ids, u64[n]
0x4c OOL: path blob
0x5c NDR
0x64 int n
0x68..0x74 the four counts (n, n, n, blob bytes)
```

The client converts the daemon's raw flags to `FSEventStreamEventFlags` (implementation_callback_rpc):

| raw bit | flag |
|---|---|
| 0, 1, 2, 3 | ItemCreated, ItemRemoved, ItemInodeMetaMod, ItemRenamed |
| 4, 5 | ItemModified |
| 6 | ItemFinderInfoMod + ItemXattrMod |
| 7 | ItemCreated |
| 8 | ItemChangeOwner |
| 9, 10 | ItemXattrMod |
| 14 | ItemCloned |
| 19, 20, 21 | IsLastHardlink, IsHardlink, OwnEvent |
| 22, 23, 24 | ItemIsSymlink, ItemIsFile, ItemIsDir |
| 25, 26 | Mount, Unmount |
| 27 | HistoryDone |
| 28 | EventIdsWrapped |
| 29, 30, 31 | MustScanSubDirs, UserDropped, KernelDropped |

File-event streams (flag 0x10) get the item's path; other streams get the directory that changed, with a trailing
`/`. A history-done event has an empty path.

## What the stand-in does

- One inotify instance; every registered path is watched with its directory tree (new directories are added as
  they appear). A path that doesn't exist yet is watched through its nearest existing ancestor. A stream on `/` or
  another ancestor of the device watches the device's data directory instead of all of Linux.
- Events are matched against each stream's paths and exclusions, coalesced per path within one read, and sent at
  once (the latency is not applied).
- Event ids increase across boots (a lease of 10,000 ids is kept in `$HOME/var/db/x86code-fseventsd/eventid`); the
  volume UUID is fixed per device (`.../uuid`). There is no event history: a stream that starts at an id from before
  this boot gets MustScanSubDirs for each of its paths and then HistoryDone.
- A client that dies (dead-name notification on its d2f port) stops getting events; its stream is removed when the
  stream port has no senders or the client unregisters.
- `X86CODE_FSEVENTS_LOG` or a file `verbose` in the state directory logs every inotify event and every send
  (`data/var/log/fseventsd.log`).
