# x86code: the iOS simulator on Linux

x86code boots Apple's **iOS 26.5 simulator runtime** (the x86_64 "universal" runtime, build 23F73) on **x86-64
Linux**. The runtime's binaries are Apple's, unmodified. They run under [Darling](https://github.com/darlinghq/darling),
which we extended for the iOS simulator. The parts a Mac and CoreSimulator normally provide are reimplemented here:
the launchd handshake, the display, touch input, Metal on Vulkan, cross-process IOSurfaces, DNS, network state and
host file services. The home screen, Settings, Safari (with real web pages), Maps, Photos, News, Health, Files,
Preview, Spotlight and the other runtime apps run with GPU rendering.

<p>
<img src="docs/screens/84-x86sim-boot-home.png" width="200" alt="home screen">
<img src="docs/screens/58-safari-wikipedia.png" width="200" alt="Safari">
<img src="docs/screens/71-spotlight-search-maps.png" width="200" alt="Spotlight and Maps">
<img src="docs/screens/64-news-today.png" width="200" alt="News">
</p>

The project is experimental. Read [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for how the pieces fit,
[docs/WORKAROUNDS.md](docs/WORKAROUNDS.md) for what is faked or approximated, and
[docs/notebook/FINDINGS.md](docs/notebook/FINDINGS.md) for the full lab notebook.

## Requirements

- **Linux x86-64** with a recent kernel. Darling needs its usual setup: FUSE/overlayfs and a setuid-root `darling`
  binary.
- **A Vulkan GPU**: NVIDIA (proprietary driver) or AMD (RADV), with `VK_EXT_external_memory_host`. Development used an
  NVIDIA RTX 5090. Without a usable GPU, `X86CODE_METAL=0` falls back to software rendering.
- **Darling's build dependencies**: clang/LLVM, cmake, ninja, bison, flex, FUSE, libcap, Python 3, and the rest of
  Darling's [build list](https://docs.darlinghq.org/build-instructions.html).
- **Rust** (stable, via rustup) for `linux/mtlvk` and metal2vulkan.
- **Python 3** with `PIL` (Pillow) and `dnspython`. The viewer needs PyGObject with GTK 4, the stream needs `ffmpeg`
  (NVENC), `x86sim dismiss` needs `tesseract`, and `x86sim site` needs `cloudflared`.
- **Apple's iOS 26.5 simulator runtime and Xcode**, which are not included: take the iOS 26.5 (23F73) simulator
  runtime image from Xcode on a Mac and extract it (see below). iOS 27 has no x86_64 runtime, so 26.5 is the target.
- **A Mac with Xcode** (reachable over ssh) to compile the small simulator-side binaries in `host/` and `tests/`.
  These are Mach-O files for the iOS simulator SDK, and only Apple's toolchain builds them. Running needs no Mac.

## Layout

```
cli/        x86sim front end; cli/libexec: the scripts it wraps; cli/lib: configuration
host/       simulator-side services (Mach-O, run inside Darling): simhost, simfb, simhid, iosurfaced,
            metalhostd (+ vkmtl), fpsim2host, fseventsd, netconfig, openurl, simshim/{compat,diag}
linux/      native Linux services: mtlvk (Rust, Metal on Vulkan), mdnsd (mDNSResponder), hostnet
tests/      probes (tests/probes), Metal (mtlprobe + shaders) and IOSurface (iosprobe) tests, smoke test
tools/      debug/: core dumps, stacks, stuck processes, Darling install (dinstall.sh), container cleanup;
            runtime/extract-runtime.py: Apple's runtime image -> RuntimeRoot on Linux
build/      Mac build scripts (mac-build-*.sh) for the host/ and tests/ binaries
docs/       ARCHITECTURE, WORKAROUNDS, METAL, protocols/, REPOS, notebook/ (FINDINGS lab notebook), screens/
external/   git submodules: darling and metal2vulkan (x86code branches of the designgears forks)
```

## Setup

1. **Clone with submodules.**
   ```sh
   git clone git@github.com:designgears/x86code.git && cd x86code
   make submodules
   cp x86sim.env.example x86sim.env      # then edit: paths, X86SIM_MAC, GPU settings
   ```
2. **Get and extract the runtime.** The runtime Xcode installs by default on Apple silicon is arm64-only; the
   *universal* variant has the x86_64 slices. On a Mac with Xcode 26 or later:
   ```sh
   xcodebuild -downloadPlatform iOS -buildVersion 26.5 -architectureVariant universal -exportPath ~/simrt
   ```
   This downloads iOS 26.5 (23F73, about 10 GB) as an exported bundle with `Restore/iOSSimulatorRuntime_Cryptex.dmg`
   (it also registers the runtime with simctl). Copy the bundle directory to Linux as
   `runtimes/ios-26.5-23F73-universal/`, then extract it there (7-Zip with APFS support, `7z`; about 23 GB):
   ```sh
   tools/runtime/extract-runtime.py runtimes/ios-26.5-23F73-universal runtimes/ios-26.5-23F73-universal/extracted
   ```
   The script runs `7z x -snld` and then makes every symlink of the image again from its listing: 7-Zip writes some
   links as empty files (a link to a link), and absolute links are made relative inside RuntimeRoot.
   `tools/runtime/extract-runtime.py --check ...` compares an extracted tree with the image without changing it.
   The default `X86SIM_RUNTIME` (x86sim.env.example) is that tree's `.../iOS 26.5.simruntime/Contents/Resources/RuntimeRoot`.
3. **Build and install Darling** (bin/darling must be setuid root, so this asks for sudo):
   ```sh
   make darling-install                  # configures external/darling -> $X86SIM_PREFIX, builds, installs
   ```
4. **Build mtlvk:** `make mtlvk`.
5. **Build the simulator-side binaries on the Mac.** Set `X86SIM_MAC=user@mac.local` (ssh key, BatchMode), then run
   `make mac`. This sends the sources over ssh, compiles them with `xcrun -sdk iphonesimulator clang` and copies the
   binaries back next to their sources. Nothing else stays on the Mac. The binaries are not committed. If you have no
   Mac, copy them from a machine that built them: see *Simulator-side binaries* below.
6. **Boot:** `cli/x86sim boot`, then `cli/x86sim view`. Put `cli/` on `PATH` or symlink `cli/x86sim` into `~/bin`.

Commands that need root (installing Darling, killing the setuid `darling` launcher, cores) read the password from
`SUDO_PASS` in the environment or use sudo's prompt or cache. The password is never stored in a file.

## Usage

```sh
x86sim boot                     # boot the standard device in the background, wait for the home screen
x86sim view [--bg]              # GTK window: mouse = touch, Ctrl/Shift+drag = two fingers, F1 = home
x86sim tap 590 1300             # device pixels (1179x2556)
x86sim swipe 900 1500 200 1500  # also: pinch, pan2, key, type "text", home, lock
x86sim openurl https://example.com
x86sim app com.apple.Preferences
x86sim dismiss                  # answer permission prompts on screen (OCR)
x86sim screenshot shot.png 3    # newest frame, scaled down 3x
x86sim stream                   # 60 fps HLS stream into $X86SIM_SITE/live
x86sim site                     # progress page + stream + http server (:8765) + cloudflared quick tunnel
x86sim status | logs [label] | fps 10
x86sim smoke                    # boot -> home screen -> Settings, Safari -> screenshot checks
x86sim probe metal|iosurface    # minimal devices with the Metal / IOSurface test clients
x86sim apptest out/             # open each app, answer prompts, screenshot, collect crashes
x86sim shutdown
```

Useful environment variables: `X86CODE_METAL=all|0|proc,...` (GPU for every process / software rendering / only the
listed processes), `X86CODE_NODT=all,-<label>` (os_log for one job), `X86CODE_INSERT=host/simshim/diag/libsyncdiag.dylib`
(diagnostic shims, built with `make mac-diag`), `X86CODE_MDNS=0`, `X86CODE_SEEDPERMS=0`. Debugging: `tools/debug/`
(`lastcrash.sh`, `stuck.sh <process>`, `allstacks.sh`, `sigctx.py`) and [docs/notebook/FINDINGS.md](docs/notebook/FINDINGS.md).

## Simulator-side binaries

The Mach-O files in `host/` and `tests/` (`simhost`, `SimFramebuffer.framework`, `SimulatorHID.framework`,
`libsimshim.dylib`, `iosurfaced`, `metalhostd`, `fpsim2host`, `fseventsd`, `netconfig`, `openurl`, `prefs`, `mtlprobe`, `iosprobe`, the
probes and diagnostic shims) are small and ours, but they are build outputs of Apple's SDK, so they are not committed.
Build them with `make mac`. To share a build with a machine that has no Mac, pack them:
`git ls-files --others --ignored --exclude-standard host tests | grep -v '\.new$' | tar czf x86code-host-bin.tgz -T -`,
then attach the archive to a release or copy it over.

## Status

- Boots to the home screen. Metal composites on the host GPU and the wallpaper, widgets and icons
  render.
- Touch, gestures, keyboard input and the hardware buttons work. Apps open by tap or with `x86sim app`/`openurl`.
- Networking works: Safari loads real pages over HTTPS, Maps loads tiles, News loads articles.
- Settings, Safari, Maps, Photos, News, Health, Calendar, Files, Preview, Reminders, Wallet, Messages, Contacts,
  Passwords, Shortcuts and Spotlight open. Known gaps are in [docs/WORKAROUNDS.md](docs/WORKAROUNDS.md) and the "Open"
  sections of the notebook. Files gets live updates (an FSEvents stand-in on Linux inotify) and hands documents to
  Preview; some daemons are left out.
- Tests: `x86sim probe metal` (46/46), `x86sim probe iosurface` (14/14), `x86sim smoke`.

## Licensing

- **Apple's iOS simulator runtime, SDK, Xcode and frameworks are not included and must not be added.** You need your
  own copy, under Apple's license terms. Only interface declarations needed for interoperability were reconstructed
  (for example `host/simfb/SimFramebuffer.h`).
- **Darling** (`external/darling`) is GPL-3.0 with its own component licenses. See its repository.
- **metal2vulkan** (`external/metal2vulkan`) is dual-licensed **GPL-2.0-or-later OR LGPL-3.0-or-later**. Its license
  files are kept. `linux/mtlvk` links it and uses the same terms.
- The rest of this repository is x86code's own work. No license has been chosen yet: the repository is private.
