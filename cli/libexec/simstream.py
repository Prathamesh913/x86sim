#!/usr/bin/env python3
"""Live video of the simulator's screen: feeds each new frame of host/simfb's shared-memory ring
(cli/libexec/fbring.py) to ffmpeg (NVENC H.264, up to 60 fps) producing a low-latency HLS stream (0.75 s segments: a
0.5 s one made EXT-X-TARGETDURATION 0, which is invalid) in ~/scratch/x86code/site/live/stream.m3u8.
x86Code: event-driven, variable frame rate. It sleeps on the ring (a futex simfb wakes at each publish) and sends a
frame only when one came (at most FPS a second), with wallclock timestamps; on a still screen the last frame is
repeated every 0.25 s (KEEPALIVE), which keeps the segments and a keyframe every 0.25 s coming for late viewers. The
crop/scale runs on the GPU (hwupload_cuda + scale_cuda), so a still screen costs ffmpeg almost nothing.
Without a ring it falls back to the framebuffer.bgra dump.
    cli/libexec/simstream.py [fps]"""
import os, struct, subprocess, sys, threading, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "lib"))
import x86config  # noqa: E402
import fbring

OUT = os.path.join(x86config.get("X86SIM_SITE"), "live")
FPS = int(sys.argv[1]) if len(sys.argv) > 1 else 60
W, H = 1178, 2556   # even width for 4:2:0; the frame is 1179 wide (cropped by one column)
OW, OH = 590, 1278  # streamed at half size

os.makedirs(OUT, exist_ok=True)
for f in os.listdir(OUT):
    os.remove(os.path.join(OUT, f))

# x86Code: frames enter ffmpeg uncropped (stride-wide rows, cropped by the filter), so a frame is one slice of the
# ring, not 2556 row copies in Python. Segments of 0.75 s (GOP = 0.25 s), playlist of 6 segments.
ff = None


KEEPALIVE = 0.25


def start(stride):
    return subprocess.Popen([
        "ffmpeg", "-hide_banner", "-loglevel", "warning",
        "-f", "rawvideo", "-pix_fmt", "bgra", "-s", "%dx%d" % (stride // 4, H), "-framerate", "1000",
        "-use_wallclock_as_timestamps", "1", "-i", "-",
        "-vf", "crop=%d:%d:0:0,hwupload_cuda,scale_cuda=%d:%d" % (W, H, OW, OH),
        "-c:v", "h264_nvenc", "-preset", "p4", "-tune", "ll", "-rc", "cbr", "-b:v", "3000k", "-bf", "0",
        "-g", "600", "-forced-idr", "1", "-force_key_frames", "expr:gte(t,n_forced*%g)" % KEEPALIVE, "-fps_mode", "vfr",
        "-f", "hls", "-hls_time", "0.75", "-hls_list_size", "6",
        "-hls_flags", "delete_segments+independent_segments+omit_endlist",
        os.path.join(OUT, "stream.m3u8")], stdin=subprocess.PIPE)


ring = fbring.Ring()
frame, stride, seq = None, None, None
mtime = None
period = 1.0 / FPS
last = 0.0
while ff is None or ff.poll() is None:
    try:
        s = ring.wait(seq, KEEPALIVE) if seq else ring.seq()
        if s and s != seq:
            f = ring.latest()
            if f and f[1] == H and f[0] >= W:
                frame, seq = f[5], f[3]
                if stride is None:
                    stride = f[2]
                elif f[2] != stride:
                    frame = None
        elif not s:
            seq = None
            time.sleep(KEEPALIVE)
            st = os.stat(fbring.FILE)
            if st.st_mtime_ns != mtime:
                data = open(fbring.FILE, "rb").read()
                w, h, st_, sq = struct.unpack("<4I", data[:16])
                if h == H and w >= W and (stride is None or st_ == stride):
                    stride = st_
                    frame = data[16:16 + st_ * h]
                mtime = st.st_mtime_ns
    except (OSError, struct.error):
        pass
    if frame is not None:
        # at most FPS frames a second: a frame that comes sooner waits for the slot (and the newest one is sent)
        d = last + period - time.monotonic()
        if d > 0:
            time.sleep(d)
            f = ring.latest()
            if f and f[3] != seq and f[1] == H and f[2] == stride:
                frame, seq = f[5], f[3]
        if ff is None:
            ff = start(stride)
            # x86Code: a 1 MB pipe (F_SETPIPE_SZ, the default limit): a 12 MB frame in ~12 writes, not ~190
            try:
                import fcntl
                fcntl.fcntl(ff.stdin.fileno(), 1031, 1 << 20)
            except OSError:
                pass
        try:
            ff.stdin.write(frame)
            ff.stdin.flush()
        except BrokenPipeError:
            break
        last = time.monotonic()
