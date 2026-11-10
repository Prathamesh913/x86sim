# simfb: display and frame output

`host/simfb/simfb.c` builds `SimFramebuffer.framework`, a stand-in for CoreSimulator's framework of the same name.
backboardd (CoreAnimation's render server) loads it from `SIMULATOR_FRAMEBUFFER_FRAMEWORK` (set by
`cli/libexec/mksimdevice.py`). The API it implements is declared in `host/simfb/SimFramebuffer.h` (reconstructed from
the runtime's QuartzCore/backboardd callers). The main display must be named `LCD`: QuartzCore treats any other
name as no main display.

There is no host: simfb reports one built-in display (1179x2556 at scale 3, the device model is iPhone17,3), hands CoreAnimation IOSurface-backed
swapchain buffers and, on every present (`SFBSwapchainSwapSubmit`), copies the frame out once the GPU work for it has
completed (with Metal, simfb waits for the MTLSimCommandBuffer completion; a 1 s watchdog copies anyway).

## Frame ring (`/dev/shm/x86sim-simfb.ring`)

Inside Darling the file is `/Volumes/SystemRoot/dev/shm/x86sim-simfb.ring` (`$SIMFB_RING` overrides); Linux readers
use `/dev/shm/x86sim-a64-simfb.ring` (`$SIMFB_RING_LINUX`). Readers: `cli/libexec/fbring.py` (library),
`simview.py` (viewer), `fb2png.py`, `fpsmeter.py`, `framecheck.py`.

```
x86Sim: reader for host/simfb's shared-memory frame ring (/dev/shm/x86sim-simfb.ring).
Layout (little-endian), written by simfb in SFBSwapchainSwapSubmit's copy:
  header (4096 bytes): magic 'SFBR' u32, version u32, nslots u32, slot_bytes u32, width u32, height u32,
      stride u32, pad u32, seq u64 (frames published), latest u32 (slot of frame seq), pad u32,
      then per slot (at 64 + 32*i): seq u64 (0 while being written), time_ns u64 (CLOCK_REALTIME at publish),
      width u32, height u32, stride u32, pad u32
  statistics at 192 (u64 each, since simfb's process started): submits, superseded (a newer present came while
      waiting for the GPU), watchdog (copied after 1 s without GPU completion), copied, ready_at_submit, wait_ns
      (sum of submit->GPU-complete waits of copied presents), wait_max_ns, copy_ns (sum of copy times), copy_max_ns, pid
  slot i at 4096 + i*slot_bytes: BGRA rows (stride bytes each).
simfb writes a slot other than the latest, then sets its slot seq and the header's latest/seq. A reader copies the
latest slot and checks the slot's seq is unchanged afterwards (it only changes once nslots-1 newer frames came).
Fallback: the old framebuffer.bgra file ({w, h, stride, seq} + rows), which simfb still writes every 2 s.
    cli/libexec/fbring.py            print the ring's state
```

**Wakeups (version 2).** After each publish simfb calls Linux `futex(FUTEX_WAKE)` on the low 32 bits of the
header's `seq` (offset 32; a shared futex, the page is a `/dev/shm` file; Darling processes make Linux syscalls
directly). Readers wait with `FUTEX_WAIT` on that word (`fbring.Ring.wait(seq, timeout)`, which falls back to 4 ms
polling for a version-1 ring), so nothing wakes while the screen is still.

The ring is recreated by each boot's simfb; the boot deletes the old file first so a stale frame is never taken
for the new boot's.

## framebuffer.bgra (legacy)

`$SIMFB_OUT` (default `$HOME/framebuffer.bgra`, i.e. `<device>/data/framebuffer.bgra` for backboardd): 16-byte header
`{u32 w, h, stride, seq}` + BGRA rows, written at most every 2 s. Kept for tools that predate the ring.
