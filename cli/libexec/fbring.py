#!/usr/bin/env python3
"""x86Code: reader for host/simfb's shared-memory frame ring (/dev/shm/x86code-simfb.ring).
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
    cli/libexec/fbring.py            print the ring's state"""
import mmap, os, struct, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "lib"))
import x86config  # noqa: E402

RING = os.environ.get("SIMFB_RING_LINUX", "/dev/shm/x86code-simfb.ring")
FILE = os.path.join(x86config.data(), "framebuffer.bgra")
HDR = 4096


class Ring:
    def __init__(self, path=RING):
        self.path = path
        self.mm = None
        self.ino = None

    def _open(self):
        try:
            st = os.stat(self.path)
        except OSError:
            self.mm = None
            return False
        # (device, inode): a new ring after a reboot can get the old one's inode number on a new tmpfs instance; keyed by
        # inode alone, the futex stayed on the deleted ring's page and every reader spun at 100% CPU
        if self.mm is not None and (st.st_dev, st.st_ino) == self.ino and len(self.mm) == st.st_size:
            return True
        if st.st_size < HDR:
            return False
        fd = os.open(self.path, os.O_RDONLY)
        try:
            self.mm = mmap.mmap(fd, st.st_size, mmap.MAP_SHARED, mmap.PROT_READ)
        finally:
            os.close(fd)
        self.ino = (st.st_dev, st.st_ino)
        return True

    def seq(self):
        """Frames published so far (0: no ring)."""
        if not self._open() or self.mm[:4] != b"SFBR":
            return 0
        return struct.unpack_from("<Q", self.mm, 32)[0]

    STATS = ("submits", "superseded", "watchdog", "copied", "ready_at_submit", "wait_ns", "wait_max_ns", "copy_ns",
             "copy_max_ns", "pid")

    def stats(self):
        """simfb's present statistics (header offset 192, u64 each), or None."""
        if not self._open() or self.mm[:4] != b"SFBR":
            return None
        return dict(zip(self.STATS, struct.unpack_from("<10Q", self.mm, 192)))

    def latest(self, rows_width=None):
        """(w, h, stride, seq, t_ns, pixels) of the newest frame, or None. pixels: bytes of h*stride
        (or, with rows_width, h rows of rows_width*4 bytes, cropped)."""
        if not self._open() or self.mm[:4] != b"SFBR":
            return None
        mm = self.mm
        nslots, slot_bytes = struct.unpack_from("<II", mm, 8)
        for _ in range(4):
            seq, latest = struct.unpack_from("<QI", mm, 32)
            if seq == 0 or latest >= nslots:
                return None
            o = 64 + 32 * latest
            sseq, t, w, h, stride = struct.unpack_from("<QQIII", mm, o)
            if sseq == 0 or stride * h > slot_bytes:
                continue
            base = HDR + latest * slot_bytes
            if rows_width and rows_width * 4 != stride:
                mv = memoryview(mm)
                px = b"".join(mv[base + y * stride:base + y * stride + rows_width * 4] for y in range(h))
                mv.release()
            else:
                px = mm[base:base + stride * h]
            if struct.unpack_from("<Q", mm, o)[0] == sseq:
                return w, h, stride, sseq, t, px
        return None


    def wait(self, seq, timeout=1.0):
        """Block until the ring's seq differs from `seq` (or `timeout` s passed, or the ring was recreated); returns
        the current seq. A version-2 ring (simfb wakes futex waiters on the low word of seq at every publish) is
        waited on with FUTEX_WAIT: no polling, no wakeups while nothing changes. Older rings are polled every 4 ms."""
        end = time.monotonic() + timeout
        while True:
            cur = self.seq()
            if cur != seq or not self.mm:
                return cur
            left = end - time.monotonic()
            if left <= 0:
                return cur
            if struct.unpack_from("<I", self.mm, 4)[0] < 2 or not _futex_wait(self.path, self.ino, cur, min(left, 1.0)):
                time.sleep(min(left, 0.004))


_fx = {"ino": None, "mm": None, "addr": 0}


def _futex_wait(path, ino, seq, secs):
    """FUTEX_WAIT on the header's seq low word (a read-write mapping of the header page, kept per ring inode)."""
    import ctypes
    if _fx["ino"] != ino:
        try:
            fd = os.open(path, os.O_RDWR)
        except OSError:
            return False
        try:
            mm = mmap.mmap(fd, HDR, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)
        finally:
            os.close(fd)
        word = ctypes.c_uint32.from_buffer(mm, 32)
        _fx.update(ino=ino, mm=mm, word=word, addr=ctypes.addressof(word),
                   libc=ctypes.CDLL(None, use_errno=True))
    class TS(ctypes.Structure):
        _fields_ = [("s", ctypes.c_long), ("ns", ctypes.c_long)]
    ts = TS(int(secs), int((secs - int(secs)) * 1e9))
    # SYS_futex 202, FUTEX_WAIT 0 (shared): returns at once if the word is no longer seq
    _fx["libc"].syscall(202, ctypes.c_void_p(_fx["addr"]), 0, ctypes.c_uint32(seq & 0xffffffff), ctypes.byref(ts), None, 0)
    return True


def latest_frame():
    """(w, h, stride, seq, pixels) from the ring, else from framebuffer.bgra if written in the last 10 s; None if
    neither."""
    r = Ring().latest()
    if r:
        return r[0], r[1], r[2], r[3], r[5]
    try:
        if time.time() - os.stat(FILE).st_mtime > 10:   # an old dump (a dead sim's, or another device's) is no frame
            return None
        data = open(FILE, "rb").read()
        w, h, stride, seq = struct.unpack("<4I", data[:16])
        return w, h, stride, seq, data[16:16 + stride * h]
    except (OSError, struct.error):
        return None


if __name__ == "__main__":
    r = Ring()
    f = r.latest()
    if not f:
        print("no ring at", RING)
    else:
        print("ring %s: frame %d %dx%d stride %d, %.3f s old" % (RING, f[3], f[0], f[1], f[2], time.time() - f[4] / 1e9))
