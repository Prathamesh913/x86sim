#!/usr/bin/env python3
"""Live window showing the simulator's screen, with input.
Frames come from host/simfb's shared-memory ring (cli/libexec/fbring.py; /dev/shm/x86code-simfb.ring); without a ring,
from the framebuffer.bgra dump. x86Code: GTK 4: each new frame becomes a Gdk.MemoryTexture that the GPU scales to the
window, checked on every tick of the display's frame clock (so up to the display's rate, ~60 fps). The Tk version
spent ~200 ms redrawing each 590x1278 photo image (about 5 fps).
    cli/libexec/simview.py [framebuffer.bgra] [scale-divisor]   (default: 2; the window can be resized)
Mouse: click/drag = touch; Ctrl+drag = two-finger pinch around the window centre (fingers mirrored through the
centre); Shift+drag = two-finger pan (fingers 100 px apart at device scale, moving together). Keyboard: typed keys go
to the simulator's hardware keyboard (host/simhid). F1 or Ctrl+H = home button, F2 = lock button, F3/F4 = volume
up/down. Input goes through <device data>/simhid.sock (backboardd's SimulatorHID stand-in)."""
import os, socket, struct, sys, time
import gi
gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
from gi.repository import Gdk, Gio, GLib, Gtk
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "lib"))
import x86config  # noqa: E402
import fbring

here = os.path.dirname(os.path.abspath(__file__))
src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(x86config.data(), "framebuffer.bgra")
div = int(sys.argv[2]) if len(sys.argv) > 2 else 2
sockpath = os.environ.get("SIMHID_SOCKET", os.path.join(os.path.dirname(src), "simhid.sock"))

state = {"mtime": None, "size": (1179, 2556), "sock": None, "seq": None, "fps": [], "mode": "one", "start": (0, 0)}
ring = fbring.Ring()


def send(line):
    for _ in range(2):
        try:
            if state["sock"] is None:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.settimeout(0.5)
                s.connect(sockpath)
                state["sock"] = s
            state["sock"].sendall((line + "\n").encode())
            return
        except OSError:
            if state["sock"]:
                state["sock"].close()
            state["sock"] = None


# Tk keysym names (GDK's keyval names are the same X keysyms) -> HID keyboard usage (page 7)
USAGES = {"Return": 0x28, "Escape": 0x29, "BackSpace": 0x2a, "Tab": 0x2b, "space": 0x2c, "minus": 0x2d,
          "equal": 0x2e, "bracketleft": 0x2f, "bracketright": 0x30, "backslash": 0x31, "semicolon": 0x33,
          "apostrophe": 0x34, "grave": 0x35, "comma": 0x36, "period": 0x37, "slash": 0x38, "Caps_Lock": 0x39,
          "Right": 0x4f, "Left": 0x50, "Down": 0x51, "Up": 0x52, "Delete": 0x4c, "Home": 0x4a, "End": 0x4d,
          "Prior": 0x4b, "Next": 0x4e, "Page_Up": 0x4b, "Page_Down": 0x4e, "Shift_L": 0xe1, "Shift_R": 0xe5,
          "Control_L": 0xe0, "Control_R": 0xe4, "Alt_L": 0xe2, "Alt_R": 0xe6, "Super_L": 0xe3, "Super_R": 0xe7}
SHIFTED = {"exclam": "1", "at": "2", "numbersign": "3", "dollar": "4", "percent": "5", "asciicircum": "6",
           "ampersand": "7", "asterisk": "8", "parenleft": "9", "parenright": "0", "underscore": "minus",
           "plus": "equal", "braceleft": "bracketleft", "braceright": "bracketright", "bar": "backslash",
           "colon": "semicolon", "quotedbl": "apostrophe", "asciitilde": "grave", "less": "comma",
           "greater": "period", "question": "slash"}
BUTTONS = {"F1": "home", "F2": "lock", "F3": "volup", "F4": "voldown"}


def usage(sym):
    sym = SHIFTED.get(sym, sym)
    if len(sym) == 1 and sym.isalpha():
        return 4 + ord(sym.lower()) - ord("a")
    if len(sym) == 1 and sym.isdigit():
        return 0x27 if sym == "0" else 0x1e + int(sym) - 1
    return USAGES.get(sym)


def on_activate(app):
    win = Gtk.ApplicationWindow(application=app, title="x86Code iOS Simulator")
    pic = Gtk.Picture()
    pic.set_content_fit(Gtk.ContentFit.FILL)
    pic.set_size_request(1179 // div, 2556 // div)
    pic.set_can_shrink(True)
    win.set_child(pic)
    win.set_default_size(1179 // div, 2556 // div)

    def norm(x, y):
        w, h = pic.get_width() or 1, pic.get_height() or 1
        return min(max(x / w, 0.0), 1.0), min(max(y / h, 0.0), 1.0)

    def emit(phase, x, y):
        x, y = norm(x, y)
        if state["mode"] == "one":
            send("touch %s %.5f %.5f" % (phase, x, y))
            return
        if state["mode"] == "pinch":
            x2, y2 = 1.0 - x, 1.0 - y          # mirrored through the centre
        else:
            dx = 50.0 / state["size"][0]       # 100 device px apart
            x, x2, y2 = x - dx, x + dx, y
        send("touch2 %s %.5f %.5f %.5f %.5f" % (phase, x, y, x2, y2))

    drag = Gtk.GestureDrag()
    drag.set_button(1)

    def begin(g, x, y):
        m = g.get_current_event_state()
        state["mode"] = "pinch" if m & Gdk.ModifierType.CONTROL_MASK else "pan" if m & Gdk.ModifierType.SHIFT_MASK else "one"
        state["start"] = (x, y)
        emit("down", x, y)

    def update(g, dx, dy):
        emit("move", state["start"][0] + dx, state["start"][1] + dy)

    def end(g, dx, dy):
        emit("up", state["start"][0] + dx, state["start"][1] + dy)
        state["mode"] = "one"

    drag.connect("drag-begin", begin)
    drag.connect("drag-update", update)
    drag.connect("drag-end", end)
    pic.add_controller(drag)

    keys = Gtk.EventControllerKey()

    def key(down, keyval, mods):
        name = Gdk.keyval_name(keyval) or ""
        if name in BUTTONS:
            send("button %s %s" % (BUTTONS[name], "down" if down else "up"))
            return True
        if name.lower() == "h" and mods & Gdk.ModifierType.CONTROL_MASK:   # Ctrl+H: home
            send("button home %s" % ("down" if down else "up"))
            return True
        u = usage(name)
        if u is not None:
            send("key %s %d" % ("down" if down else "up", u))
        return True

    keys.connect("key-pressed", lambda c, kv, code, mods: key(True, kv, mods))
    keys.connect("key-released", lambda c, kv, code, mods: key(False, kv, mods))
    win.add_controller(keys)

    def show(w, h, stride, seq, px):
        tex = Gdk.MemoryTexture.new(w, h, Gdk.MemoryFormat.B8G8R8A8_PREMULTIPLIED, GLib.Bytes.new(px), stride)
        pic.set_paintable(tex)
        state["size"] = (w, h)
        now = time.monotonic()
        f = state["fps"] = [t for t in state["fps"] if t > now - 1.0] + [now]
        win.set_title("x86Code iOS Simulator — frame %d, %d fps" % (seq, len(f)))
        state["shown"] = state.get("shown", 0) + 1
        if now - state.setdefault("logged", now) >= 5:   # the draw rate, every 5 s while frames come (stdout)
            print("simview: %d of %d frames drawn in %.1f s (peak 1 s: %d fps)" % (state["shown"],
                  seq - state.get("logseq", seq - state["shown"]), now - state["logged"], max(state.get("peak", 0), len(f))),
                  flush=True)
            state["shown"], state["logged"], state["peak"], state["logseq"] = 0, now, 0, seq
        state["peak"] = max(state.get("peak", 0), len(f))

    # x86Code: one check per frame-clock tick (the display's rate); a frame is only built when the ring's seq changed
    def tick(widget, clock):
        try:
            seq = ring.seq()
            if seq:
                if seq != state["seq"]:
                    f = ring.latest()
                    if f:
                        show(f[0], f[1], f[2], f[3], f[5])
                        state["seq"] = seq
            else:
                st = os.stat(src)
                if st.st_mtime_ns != state["mtime"]:
                    data = open(src, "rb").read()
                    w, h, stride, seq = struct.unpack("<4I", data[:16])
                    show(w, h, stride, seq, data[16:16 + stride * h])
                    state["mtime"] = st.st_mtime_ns
        except (OSError, struct.error, ValueError, GLib.Error):
            pass
        return GLib.SOURCE_CONTINUE

    pic.add_tick_callback(tick)
    win.present()


app = Gtk.Application(application_id="org.x86code.simview", flags=Gio.ApplicationFlags.NON_UNIQUE)
app.connect("activate", on_activate)
app.run([sys.argv[0]])
