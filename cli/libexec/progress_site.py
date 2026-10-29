#!/usr/bin/env python3
"""Small public progress page for the iOS-simulator-on-Linux project.
Builds a static site in $X86SIM_SITE (default ~/scratch/x86code/site) (only what's published lives there):
the live screen (refreshed every few seconds), recent milestone screenshots and the
app compatibility notes. Serve the directory with any static server (see cli/libexec/progress_tunnel.sh).
    cli/libexec/progress_site.py [--loop]"""
import html, json, os, re, shutil, struct, subprocess, sys, time
from PIL import Image
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fbring

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "lib"))
import x86config  # noqa: E402
X = x86config.ROOT
SITE = x86config.get("X86SIM_SITE")
SCREENS = x86config.get("X86SIM_SCREENS")
FB = os.path.join(x86config.data(), "framebuffer.bgra")

# x86Code: the arm64 effort (iOS 27 on x86_64 through the dynarmic JIT). Its status (summary, current work, metrics,
# stages, issues, milestones) lives in docs/progress.json so agents update data, not this file; screenshots in
# docs/screens/arm64 (X86SIM_SCREENS_A64).
SCREENS = os.environ.get("X86SIM_SCREENS_A64", os.path.join(X, "docs/screens/arm64"))
STATUS = os.path.join(X, "docs/progress.json")


def status():
    try:
        return json.load(open(STATUS))
    except (OSError, ValueError):
        return {}


def recent_commits(n=12):
    """The latest commits of the top repo (subjects only), newest first."""
    try:
        out = subprocess.run(["git", "-C", X, "log", "-%d" % n, "--format=%ar\t%s"], capture_output=True, text=True,
                             timeout=5).stdout
    except (OSError, subprocess.TimeoutExpired):
        return []
    return [l.split("\t", 1) for l in out.splitlines() if "\t" in l]

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


def thumb(name):
    src = os.path.join(SCREENS, name)
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
    st = status()
    e = html.escape
    badge = {"done": "done", "in progress": "now", "later": "later", "next": "next"}
    stages = "".join('<details%s><summary><span class="b %s">%s</span> %s</summary><p>%s</p></details>'
                     % (" open" if x.get("status") == "in progress" else "", badge.get(x.get("status"), "later"),
                        e(x.get("status", "")), e(x.get("name", "")), e(x.get("detail", "")))
                     for x in st.get("stages", []))
    metrics = "".join('<div class="m"><div class="k">%s</div><div class="v">%s</div><div class="n">%s</div></div>'
                      % (e(k), e(v), e(n)) for k, v, n in st.get("metrics", []))
    issues = "".join("<li>%s</li>" % e(i) for i in st.get("issues", []))
    cards = "".join('<figure><img src="%s" loading="lazy" alt=""><figcaption>%s</figcaption></figure>' % (thumb(f), e(c))
                    for f, c in st.get("milestones", []) if os.path.exists(os.path.join(SCREENS, f)))
    commits = "".join('<li><span class="t">%s</span> %s</li>' % (e(t), e(m[:160])) for t, m in recent_commits())
    page = """<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#0b0b0c"><title>%(title)s</title>
<style>
*{box-sizing:border-box}body{margin:0;font:16px/1.45 -apple-system,system-ui,sans-serif;background:#0b0b0c;color:#e6e6e6;
padding:16px max(16px,env(safe-area-inset-right)) 32px max(16px,env(safe-area-inset-left))}
main{max-width:720px;margin:0 auto}h1{font-size:24px;line-height:1.2;margin:8px 0}h2{font-size:18px;margin:28px 0 10px}
p{margin:6px 0}.sub{color:#aaa;font-size:14px}.now{background:#13261b;border:1px solid #1f5135;border-radius:12px;padding:12px;margin:14px 0}
.now b{color:#6fdc9a}video{width:100%%;max-width:420px;display:block;margin:0 auto;aspect-ratio:1206/2622;max-height:78vh;
background:#000;border-radius:28px;border:1px solid #333}.cap{text-align:center;color:#888;font-size:12px}
.ms{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:12px}.ms figure{margin:0}
.ms img{width:100%%;border-radius:14px;border:1px solid #333}figcaption{font-size:12px;color:#aaa;margin-top:4px}
.mt{display:grid;grid-template-columns:1fr 1fr;gap:10px}.m{background:#151517;border:1px solid #26262a;border-radius:12px;padding:10px}
.k{font-size:12px;color:#999}.v{font-size:18px;font-weight:600}.n{font-size:12px;color:#888}
details{background:#151517;border:1px solid #26262a;border-radius:12px;margin:8px 0;padding:10px 12px}
summary{cursor:pointer;font-weight:600}details p{color:#bbb;font-size:14px}
.b{display:inline-block;font-size:11px;font-weight:700;padding:2px 8px;border-radius:99px;margin-right:6px;vertical-align:1px}
.done{background:#1f5135;color:#9ff0c0}.now .b,.b.now{background:#5a4510;color:#ffd77a}.next,.later{background:#2a2a30;color:#bbb}
ul{padding-left:20px}li{margin:4px 0}.log li{font-size:13px;color:#bbb;list-style:none;margin-left:-20px;border-bottom:1px solid #1d1d20;padding:6px 0}
.t{color:#777;font-size:12px;display:block}footer{color:#666;font-size:12px;margin-top:28px}
@media(min-width:760px){.mt{grid-template-columns:repeat(3,1fr)}h1{font-size:30px}}
</style></head><body><main>
<h1>%(title)s</h1><p class="sub">%(summary)s</p>
<div class="now"><b>Working on now:</b> %(now)s</div>
<h2>Live</h2><video id="v" poster="live.jpg" controls autoplay muted playsinline></video>
<p class="cap">Live H.264 stream (~1-2 s behind). Test boots come and go, so the screen may hold the last frame. Last frame %(seq)s.</p>
<h2>Where it stands</h2><div class="mt">%(metrics)s</div>
<h2>Stages</h2>%(stages)s
<h2>Known issues</h2><ul>%(issues)s</ul>
<h2>Milestones</h2><div class="ms">%(cards)s</div>
<h2>Latest commits</h2><ul class="log">%(commits)s</ul>
<footer>Updated %(time)s · private project</footer></main>
<script src="https://cdn.jsdelivr.net/npm/hls.js@1"></script><script>
var v=document.getElementById('v'),u='live/stream.m3u8';
if(v.canPlayType('application/vnd.apple.mpegurl')){v.src=u}else if(window.Hls&&Hls.isSupported()){var h=new Hls({liveSyncDurationCount:2,liveMaxLatencyDurationCount:6,maxLiveSyncPlaybackRate:1.2,backBufferLength:4});h.loadSource(u);h.attachMedia(v);h.on(Hls.Events.ERROR,function(e,d){if(d.fatal){setTimeout(function(){h.loadSource(u)},2000)}})}
</script></body></html>""" % {"title": e(st.get("title", "iOS Simulator on Linux")), "summary": e(st.get("summary", "")),
                              "now": e(st.get("now", "")), "seq": seq if seq is not None else "—", "metrics": metrics,
                              "stages": stages, "issues": issues, "cards": cards, "commits": commits,
                              "time": time.strftime("%Y-%m-%d %H:%M")}
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
