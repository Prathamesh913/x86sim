#!/usr/bin/env python3
"""Small public progress page for the iOS-simulator-on-Linux project.
Builds a static site in $X86SIM_SITE (default ~/scratch/x86code/site) (only what's published lives there):
the live screen (refreshed every few seconds), recent milestone screenshots and the
app compatibility notes. Serve the directory with any static server (see cli/libexec/progress_tunnel.sh).
    cli/libexec/progress_site.py [--loop]"""
import html, os, re, shutil, struct, sys, time
from PIL import Image
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fbring

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "lib"))
import x86config  # noqa: E402
X = x86config.ROOT
SITE = x86config.get("X86SIM_SITE")
SCREENS = x86config.get("X86SIM_SCREENS")
FB = os.path.join(x86config.data(), "framebuffer.bgra")

MILESTONES = [  # (file, caption)
    ("01-boot-logo-full.png", "First pixels: boot logo through our SimFramebuffer"),
    ("12-home-screen.png", "SpringBoard reaches the home screen"),
    ("19-first-boot-colour-icons.png", "Colour icons, live Calendar"),
    ("24-metal-backboardd-home.png", "CoreAnimation on Metal → Vulkan (RTX 5090)"),
    ("36-metal-wallpaper-astc-fixed.png", "Wallpaper on Metal"),
    ("26-tap-opens-settings.png", "Touch input: tap opens Settings"),
    ("40-maps-tiles-over-network.png", "Networking: Maps tiles"),
    ("58-safari-wikipedia.png", "Safari browsing Wikipedia"),
    ("64-news-today.png", "Apple News, live stories"),
]
# x86Code: the arm64 effort (iOS 27 on x86_64 through the dynarmic JIT). Its milestone screenshots are the a64-*.png
# files in the scratch dir (X86SIM_A64_SHOTS), picked up automatically, newest first.
A64_SHOTS = os.environ.get("X86SIM_A64_SHOTS", x86config.get("X86SIM_SCRATCH"))
STAGES = [  # (stage, status) from docs/notebook/ARM64-PLAN.md
    ("0. Proof of concept: arm64 programs under the dynarmic JIT on x86_64", "done"),
    ("1. Inside Darling: Apple's dyld_sim + the iOS 27 shared cache, ObjC and Swift", "done"),
    ("2. Boot: launchd_sim → backboardd → SpringBoard home screen", "in progress: boot logo up"),
    ("3. Fast: native hot paths, persistent translation cache", "planned"),
    ("4. Near native: ahead-of-time x86 translation of the shared cache", "planned"),
]


def live_frame():
    try:
        w, h, stride, seq, px = fbring.latest_frame()   # x86Code: the shm ring, else framebuffer.bgra
        img = Image.frombuffer("RGBA", (w, h), px, "raw", "BGRA", stride, 1)
        img.convert("RGB").resize((w // 3, h // 3), Image.LANCZOS).save(os.path.join(SITE, "live.tmp.jpg"), quality=85)
        os.replace(os.path.join(SITE, "live.tmp.jpg"), os.path.join(SITE, "live.jpg"))
        return seq
    except (OSError, struct.error, ValueError, TypeError):
        return None


def latest_screens(n=8):
    files = sorted((f for f in os.listdir(SCREENS) if f.endswith(".png")), key=lambda f: int(re.match(r"\d+", f).group()) if re.match(r"\d+", f) else 0)
    return files[-n:]


def a64_screens(n=12):
    """The arm64 milestones (a64-*.png in A64_SHOTS), newest first."""
    files = [f for f in os.listdir(A64_SHOTS) if f.startswith("a64-") and f.endswith(".png")]
    return sorted(files, key=lambda f: os.path.getmtime(os.path.join(A64_SHOTS, f)), reverse=True)[:n]


def thumb(name, folder=SCREENS):
    src = os.path.join(folder, name)
    dst = os.path.join(SITE, "s", os.path.splitext(name)[0] + ".jpg")
    if not os.path.exists(dst) or os.path.getmtime(dst) < os.path.getmtime(src):
        img = Image.open(src).convert("RGB")
        img.thumbnail((400, 900), Image.LANCZOS)
        img.save(dst, quality=85)
    return "s/" + os.path.basename(dst)


def compat_section():
    """The newest 'App compatibility' block from docs/notebook/FINDINGS.md, as preformatted text."""
    try:
        text = open(os.path.join(X, "docs/notebook/FINDINGS.md")).read()
    except OSError:
        return ""
    blocks = re.findall(r"(?ims)^#+ *[^\n]*compatib[^\n]*\n(.*?)(?=^#+ )", text + "\n# end")
    return blocks[-1].strip() if blocks else ""


def build():
    os.makedirs(os.path.join(SITE, "s"), exist_ok=True)
    seq = live_frame()
    cards = "".join('<figure><img src="%s" loading="lazy"><figcaption>%s</figcaption></figure>' % (thumb(f), html.escape(c))
                    for f, c in MILESTONES if os.path.exists(os.path.join(SCREENS, f)))
    a64 = "".join('<figure><img src="%s" loading="lazy"><figcaption>%s</figcaption></figure>' % (thumb(f, A64_SHOTS), html.escape(os.path.splitext(f)[0]))
                  for f in a64_screens())
    stages = "".join("<tr><td>%s</td><td>%s</td></tr>" % (html.escape(a), html.escape(b)) for a, b in STAGES)
    recent = "".join('<figure><img src="%s" loading="lazy"><figcaption>%s</figcaption></figure>' % (thumb(f), html.escape(os.path.splitext(f)[0]))
                     for f in reversed(latest_screens()))
    page = """<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>iOS Simulator on Linux — progress</title>
<style>body{font:15px system-ui,sans-serif;background:#111;color:#ddd;margin:2em auto;max-width:1100px;padding:0 1em}
h1{font-weight:600}h2{margin-top:2em;border-bottom:1px solid #333}a{color:#7ab}
.grid{display:flex;flex-wrap:wrap;gap:14px}figure{margin:0;width:200px}figure img{width:200px;border-radius:14px;border:1px solid #333}
figcaption{font-size:12px;color:#aaa;margin-top:4px}#live{width:393px;border-radius:24px;border:1px solid #444}
td{padding:4px 14px 4px 0;border-bottom:1px solid #222}pre{white-space:pre-wrap;background:#1b1b1b;padding:1em;border-radius:8px;font-size:12px}</style></head><body>
<h1>iOS Simulator on x86-64 Linux</h1>
<p>Apple's iOS Simulator runtimes, unmodified, on Linux inside Darling — no Mac at runtime. Updated %s.</p>
<h2>iOS 27 on x86_64 via the dynarmic JIT</h2>
<p>The arm64-only iOS 27 runtime, translated to x86-64 by an embedded JIT (dynarmic) inside Darling.</p>
<table>%s</table>
<h3>Milestones</h3><div class="grid">%s</div>
<h2>Live video</h2><video id="v" controls autoplay muted playsinline style="width:393px;border-radius:24px;border:1px solid #444"></video><p id="seq">last saved frame %s (stream: H.264, 60 fps, ~1-2 s behind)</p>
<h2>iOS 26.5 on x86-64: milestones</h2><div class="grid">%s</div>
<h2>Latest screenshots</h2><div class="grid">%s</div>
<h2>App compatibility (from the lab notes)</h2><pre>%s</pre>
<script src="https://cdn.jsdelivr.net/npm/hls.js@1"></script><script>
var v=document.getElementById('v'),u='live/stream.m3u8';
if(v.canPlayType('application/vnd.apple.mpegurl')){v.src=u}else if(window.Hls&&Hls.isSupported()){var h=new Hls({liveSyncDurationCount:2,liveMaxLatencyDurationCount:6,maxLiveSyncPlaybackRate:1.2,backBufferLength:4});h.loadSource(u);h.attachMedia(v);h.on(Hls.Events.ERROR,function(e,d){if(d.fatal){setTimeout(function(){h.loadSource(u)},2000)}})}
</script>
</body></html>""" % (time.strftime("%Y-%m-%d %H:%M"), stages, a64, seq if seq is not None else "—", cards, recent, html.escape(compat_section()))
    with open(os.path.join(SITE, "index.tmp.html"), "w") as f:
        f.write(page)
    os.replace(os.path.join(SITE, "index.tmp.html"), os.path.join(SITE, "index.html"))


if __name__ == "__main__":
    build()
    # x86Code: the live JPEG only when the screen changed (at most every 3 s; the ring's futex wakes us), the page
    # every 60 s
    ring, seen, built = fbring.Ring(), None, time.time()
    while "--loop" in sys.argv:
        t0 = time.time()
        seq = ring.wait(seen, max(1.0, 60 - (t0 - built))) if seen else ring.seq()
        if seq != seen or not seq:
            seen = live_frame() if seq else None
            if not seq:
                time.sleep(3)
        if time.time() - built >= 60:
            build()
            built = time.time()
        time.sleep(max(0.0, 3 - (time.time() - t0)))
