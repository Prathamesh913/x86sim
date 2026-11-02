# Speed 34 (2026-11-02): the "aged" tap was the keyboard, FRINTA/FRINTX/FCVTA inline, leftovers

Phone: var/device-a64-ios272 (27.2 24B8117; 27.0's runtime is missing). Image $SCRATCH/jc-24B8117. Method: logs first,
tapm.sh (5 warm taps), openm.sh, perf record on SpringBoard, guest sampler (GSAMPLE_ALL + SKIP=stubs + TRACE, a64-penv,
cleared after use). Scratch: $SCRATCH/s34.

## 1. FP rounding inline (dynarmic x64 backend)
- dynarmic sent FRINTA (ties away), FRINTX (exact), FCVTAS/FCVTAU and FP16 to `FP::FPRoundInt` / `FPToFixed` in
  software. New `emit_x64_frint.h`: ROUNDSS/SD/PS/PD for every mode. Ties away is t = trunc(x), d = x - t (exact),
  t + (|d| >= 0.5 ? copysign(1, x) : copysign(0, x)). Lanes with |x| >= 2^52 (2^23), inf or NaN are masked to d = 0
  with an integer compare first, so no lane raises a spurious IOC/IXC, NaNs pass through quiet, and -0 stays -0 in
  every FPCR rounding mode. Scalars use scalar ops, because the upper lanes may hold SNaNs. Bit 3 of the immediate is
  set only for the non-exact forms, so FRINTX/FCVT* raise IXC when inexact and FRINTN/P/M/Z/A never do. That also
  fixes a flag bug in upstream's FRINTN/P/M/Z path, which raised PE. Needs SSE4.1 (SSE4.2 for 64-bit: pcmpgtq);
  otherwise the software fallback runs as before. AVX-512 not used: no gain for a 1-2 element op.
- Test: `tests/arm64/frint/run.sh` runs FRINTN/P/M/Z/A/X/I scalar, FRINTA/X/N/M vector and FCVTAS/FCVTAU (W/X from S/D,
  4S/2D) through the JIT and compares with dynarmic's software functions. Inputs: edge cases plus 4000 random values
  per size around the integer boundary. FPCR: RN/RP/RM/RZ, DN, FZ. **1,196,902 checks, 0 failures** (values and
  IOC/IXC). The first run caught the -0 bug in round-toward-minus (t - +0 gave -0 for 0.3), now fixed. Two known
  inaccuracies in the shared conversion paths are excluded from the flag check: an out-of-range FCVT raises IXC as well
  as IOC, and the scalar unsigned 64-bit path raises IXC. Both are pre-existing and apply to every mode.
- Effect: FPRoundInt / FPRoundBase are **gone from the aged SpringBoard profile** (Speed 33: 0.62% of cycles; now only
  FPRecipEstimate at 0.03%). The live loop that called them (host 0x7fff7c3c55xx, 6.8% of the tap window) is
  libcompression's encoder writing the app snapshot (guest 0x25a9ddf..). It still runs, but no longer calls out. The
  tap CPU change from this alone is within noise (see section 2 for what moved the aged taps).
- kCodegenVersion was not bumped: image chunks keep the old (correct, slower) calls until rebuilt. The live JIT uses
  the inline code.

## 2. "Aged" taps: the attached hardware keyboard (fixed)
- Guest sampler, fresh vs aged (5 taps each, SpringBoard, the same instruments attached): the aged taps spend
  ~1150 frame-samples in UIKit's **focus system** (`_UIFocusNearestAncestorEnvironmentScrollableContainer`,
  `_UIFocusEnvironmentEnumerateAncestorEnvironments`, `_UIFocusEngineFirstScrollableContainerTupleThatScrollsForItem`,
  `_UIViewIsTransparentFocusItem`, ...). The fresh taps spend 0. The snapshot encode/decode (ImageIO + vImage +
  libcompression, ASTC) runs on every tap in **both** states, so it is not the difference. ASTC is native and
  invisible to SKIP=stubs.
- Cause: simhid attaches its keyboard service at the first host key (Speed 31). While a hardware keyboard is attached,
  UIKit runs in hardware-keyboard mode, and SpringBoard's focus engine walks the focus environment on every app
  transition. The gate types into Spotlight / Settings search, so every system after a gate (or after any typing in the
  launcher) was "aged". This explains Speed 33's puzzles: SpringBoard restarted in an aged system stays slow (backboardd
  keeps the service), and "4 min after a fresh boot" was slow only if something had typed.
- Proof on one boot: fresh 530-550 ms SpringBoard CPU per tap; after one Shift press, 630-720 ms (response
  0.42 -> 0.49-0.53 s).
- Fix (simhid.m): the on-demand keyboard is **detached after SIMHID_KB_IDLE_S (default 30 s, 0 = never)** with no key
  down, and the next key attaches it again (with the existing settle). SIMHID_KEYBOARD=1 (attached from start) is
  unchanged. Callbacks are NULL-safe: the first gate attempt (s34g1) crashed backboardd in v_close, called with a NULL
  target after _IOHIDServiceTerminate.
- After the detach: 540-580 ms, the same as fresh (target "aged within 5% of fresh": met once the keyboard is idle).

## 3. Count-trained split image
NUMBERS_SPLIT

## 4. Maps route: the 0x98 was not a return value
- `AGGraphSetOutputValue` (AttributeGraph 0x1c952c490) is `void`. Its last path is `bl <inner>; tbz w0, #0, ...; ret`,
  so x0 at return is whatever the inner call left (only bit 0 was tested, inside AG). No caller tests it, so it is not
  a thunk / setcc / zero-extension bug in our stack. The lead is closed. GATE_ROUTE stays off: the route abort itself
  is unchanged (Speed 33: the node at the abort equals the after-set state).

## 5. Leftovers
- **Evicted chunks re-admitted** (aot.rs): an update that builds (not --only) puts chunks evicted by an earlier update
  back into free gaps (hottest per byte first, never evicting for them).
- **Stale container rows pruned** (placeapp.py): on install, rows of the same .app name in another container are
  dropped when that container exists on no phone (var/device-*/data plus the installing phone). The jitcache dir is
  shared per runtime, so another phone's apps stay. Remaining slots never move. Dry run on a copy of the live
  placement.txt: drops the old x86ui container (692BAD54). Uninstall does not prune (its placement change would need
  an image update outside the transactional install path).
- **tmpfs copy on first open**: a background copy thread (X86CODE_A64_AOT_HUGE_BG=1) maps the .map directly and copies
  meanwhile. On the phone the thread (in UserEventAgent) vanished after ~350 MB, leaving the marker and the partial
  file (moved to $SCRATCH/trash/s34-img). Kept **opt-in**, so the foreground copy stays the default. Note: this boot's
  copy also hit `.small` (fragmented host memory), so the copy was skipped and the .map mapped directly.

## 6. The aged-gate STALL: gate logic
- gate.sh opens Settings at General > About. On an aged system jetsam's grace has doubled per kill, so Settings is
  still alive at the swipe-up / switcher checks and correctly resumes on About (iOS keeps the page). Cold-boot gates
  pass because jetsam killed it in between. The checks now accept the About page ("an app is showing"). The aged
  SKIPBOOT gate (age-gate1) passed: 0 failed.

## Gate
GATE_RESULT

## Defaults / opt-ins
- New defaults (gated): inline FRINT/FCVTA (no flag: exact-match tested), simhid keyboard idle detach (SIMHID_KB_IDLE_S).
- Opt-in: X86CODE_A64_AOT_HUGE_BG=1. Still opt-in: X86CODE_A64_COUNT, hot_split_pct, X86CODE_A64_AGOUT.

Rule check: nothing deleted (the stale /tmp image files were moved to $SCRATCH/trash/s34-img); a64-penv cleared.
