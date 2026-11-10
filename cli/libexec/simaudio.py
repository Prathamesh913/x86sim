#!/usr/bin/env python3
"""x86Sim: the host side of the simulator's audio (the Mac's coreaudiod on a Linux host).

host/simaudio/libsimaudio.dylib (in every simulator process) presents an output and an input device and connects
here for each running stream: a 16-byte header ("x86A", u32 direction 0=output/1=input, u32 rate, u32 channels),
then float32 interleaved PCM. Output streams are played with `pw-cat --playback` (PipeWire, or PulseAudio through
pipewire-pulse), one PipeWire stream per simulator stream, so PipeWire mixes them; input streams get the default
source through `pw-cat --record`.

usage: simaudio.py SOCKET     env: X86SIM_AUDIO_VOLUME (0..1, default 0.5), X86SIM_AUDIO_MIC=0 (no microphone:
                                   input streams get silence)
"""
import os, socket, struct, subprocess, sys, threading, time

VOLUME = os.environ.get("X86SIM_AUDIO_VOLUME", "0.5")
MIC = os.environ.get("X86SIM_AUDIO_MIC", "1") != "0"


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def pwcat(mode, rate, ch, name):
    return ["pw-cat", mode, "-a", "--format", "f32", "--rate", str(rate), "--channels", str(ch),
            "--latency", "40ms", "-P", "{ node.name = \"%s\" media.name = \"iOS Simulator\" application.name = \"x86sim\" }" % name]


def serve(conn, n):
    try:
        hdr = b""
        while len(hdr) < 16:
            b = conn.recv(16 - len(hdr))
            if not b:
                return
            hdr += b
        magic, direction, rate, ch = hdr[:4], *struct.unpack("<III", hdr[4:])
        if magic != b"x86A" or ch not in (1, 2) or rate not in (44100, 48000):
            log("conn %d: bad header %r" % (n, hdr))
            return
        log("conn %d: %s %d Hz %d ch" % (n, "input" if direction else "output", rate, ch))
        if direction == 0:
            p = subprocess.Popen(pwcat("--playback", rate, ch, "x86sim-out-%d" % n) + ["--volume", VOLUME, "-"],
                                 stdin=subprocess.PIPE, stderr=subprocess.DEVNULL)
            total = 0
            while True:
                b = conn.recv(65536)
                if not b:
                    break
                total += len(b)
                try:
                    p.stdin.write(b)
                    p.stdin.flush()
                except BrokenPipeError:
                    break
            log("conn %d: output ended (%.1f s)" % (n, total / (4 * ch * rate)))
            try:
                p.stdin.close()
            except Exception:
                pass
            p.terminate()
        else:
            if not MIC:
                silence = bytes(4 * ch * rate // 100)
                while True:
                    conn.sendall(silence)
                    time.sleep(0.01)
            p = subprocess.Popen(pwcat("--record", rate, ch, "x86sim-in-%d" % n) + ["-"],
                                 stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
            try:
                while True:
                    b = p.stdout.read1(4096)
                    if not b:
                        break
                    conn.sendall(b)
            except (BrokenPipeError, ConnectionResetError):
                pass
            log("conn %d: input ended" % n)
            p.terminate()
    except (BrokenPipeError, ConnectionResetError):
        pass
    finally:
        conn.close()


def main():
    path = sys.argv[1]
    try:
        os.unlink(path)
    except FileNotFoundError:
        pass
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.bind(path)
    os.chmod(path, 0o777)
    s.listen(16)
    log("listening on", path, "volume", VOLUME, "mic", MIC)
    n = 0
    while True:
        c, _ = s.accept()
        n += 1
        threading.Thread(target=serve, args=(c, n), daemon=True).start()


if __name__ == "__main__":
    main()
