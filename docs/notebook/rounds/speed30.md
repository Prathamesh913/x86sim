# Speed 30 (2026-11-01): Maps route abort narrowed (not fixed), SpringBoard idle traffic identified, debug tooling

Phone: var/device-a64-ios272 (iPhone 17, 27.2 24B8117), image $SCRATCH/jc-24B8117 (unchanged). Method: logs first
(mapsroute.sh in a loop, JIT logs, abort dumps), guest code disassembly (cdis.py, ipsw a2s), arm64 interpose /
swizzle shims injected per process through a64-penv, Mac mini (read-only) for comparison.

## 1. Maps route abort: NOT fixed; several theories ruled out with evidence
Rate this round: 10 of 13 runs that got to a route verdict aborted, 3 showed the route (mixed configs: default plus
the debug knobs below, none of which changed the outcome);
several more runs were lost to Maps UI drift (a leftover "Add a Note" sheet / Work-favorite card), which mapsroute.sh
does not handle yet.
- Stack (symbolicated): SizeFittingState.applyChildren -> AGGraphGetValue -> DynamicLayoutComputer<_HStackLayout>
  -> DynamicContainerInfo.updateItems -> ViewList._Group.Init rule -> AGGraphGetValue -> update_attribute -> the
  "failed to set an initial value" check in AG::Graph::UpdateStack::update (after the rule ran: 0x1c950c138 ->
  0x1c950c4b8 -> 0x1c950c628). The failing attribute varies: _External, DynamicContainerInfo<DynamicLayoutViewAdaptor>,
  DynamicLayoutComputer<_VStackLayout>, but within one config often the same id (0x4001c0) run after run.
- **Not TLS**: new X86CODE_A64_ABORT_TSD=<key address> dumps AG's pthread key (340, from 0x22e74c980) at the abort:
  the current UpdateStack is the right one and its top frame is the failing node.
- **Not cancellation**: UpdateStack::cancel sets bit 3 of each frame's flags word; the failing frame's flags were 1.
  Debug probes (X86CODE_A64_AGDL=1 + LIVE_ONLY=Maps: SVCs planted by MemoryReadCode at Context::set_deadline,
  passed_deadline_slow's two "passed" exits, UpdateStack::cancel and AGGraphCancelUpdate) never fired in a crashing
  run. Note: Speed 27's agdiag interposition could never have seen these calls: SwiftUI -> AttributeGraph calls inside
  the shared cache are bound directly, so DYLD_INTERPOSE misses them (also why agnodediag saw no AGGraphCreateAttribute).
- **Not the guest heap scavenger** (X86CODE_A64_GHEAP_SCAVENGE=0 for Maps: still aborts).
- **Darling shared vm_remap race (real, fixed behind a flag, not the cause here)**: darlingserver's dtape_share_region
  shares private memory by copying it into a memfd and then mapping the memfd over the source, so stores other threads
  make in between are lost. AttributeGraph's data::table::grow_region uses exactly that (vm_remap copy=FALSE of its
  process-wide node table). h_vm_remap now (X86CODE_A64_REMAP_FREEZE=1) write-protects the source for a same-task
  copy=FALSE remap of private RW memory; a writer faults, waits in on_fault until the protection is back and retries.
  Maps does one 1 MB remap early and still aborted with it on, so it stays opt-in.
- What is left: the rule (Layout.updateLayoutComputer / DynamicContainerInfo.updateValue) runs and leaves no value, or
  an external input is read before SwiftUI sets it. Next step: trace AGGraphSetOutputValue / AGGraphGetOutputValue
  calls (JIT-level probes, not interposition) for the failing attribute in one run, and compare the order with the
  Mac (27.2 sim not installed on the mini; only 26.5 is booted there).

## 2. SpringBoard idle traffic: identified, not reduced yet
- X86CODE_A64_MSG_STATS now also histograms (sent id, received id, caller); X86CODE_A64_THUNK_HIST=sub,sub histograms
  any thunk by (name, lr, caller's lr). The "~810 mach_msg/s" of Speed 29 counted every thunk with "mach_msg" in its
  name: ~2/3 are voucher_mach_msg_adopt/revert. Real mach_msg: ~256/s, all on the main thread's
  __CFRunLoopServiceMachPort; ~60% are receive timeouts (CFRunLoop timer deadlines), the rest id-0 timer messages.
  mk_timer arms: CA display_timer_callback and UC::DriverCore::continueProcessing, ~32/s each.
- dldiag (new swizzle shim: CADisplayLink create/pause/add/invalidate + the live links every 10 s) shows two links live
  at the idle home screen: **SBUIPreciseClockTimer -_onDisplayLink:** (15 fps, from
  SBFLockScreenDateViewController _startUpdateTimer) and a 30 fps link of **_UIMotionEffectEngine** (icon parallax,
  event provider _UIMotionEffectSimulatorMotionEventProvider, which never received an event:
  _hasReceivedAtLeastOneEventSinceStarting = 0).
- Mac comparison (iOS 26.5 sim, `sample` of its SpringBoard, 5 s): SBUIPreciseClockTimer and display_timer_callback
  frames are there too (Apple's behaviour), no _UIMotionEffectEngine frames; top: ~50-70 context switches/s, ~0.5 idle
  wakeups/s. So the motion-effect link is the likely excess: on a Mac the simulator motion provider gets events from
  the host (or the engine stops); our SimulatorHID stand-in sends none. Next: find the provider's event source
  (_motionEventQueue) and feed it / let it report "no motion" the way the Mac's does. No fix this round; the
  darlingserver-bypass fast path was not started (the per-message cost question stays after the waste is removed).

## 3, 4. Not done
Persisting app/non-cache code, hot/cold layout, native ASTC/LZFSE SplashBoard decode, Siri audio input.

## Tooling added
- a64-penv: variables it sets now reach the guest environment (the guest env was built from envp; setenv only changed
  environ). Only when the file applied something (default unchanged).
- X86CODE_A64_ABORT_TSD, X86CODE_A64_AGDL (27.2 addresses), X86CODE_A64_THUNK_HIST, MSG_STATS histogram,
  X86CODE_A64_REMAP_FREEZE=1.
- host/simshim/diag: agnodediag.c (one AG attribute's creation / setValue; interposition, so cache-internal calls are
  invisible), dldiag.m (CADisplayLink + _UIMotionEffectEngine). Built on the Mac (arm64 sim), loaded with
  `<Proc> X86CODE_A64_DYLD_INSERT_LIBRARIES=<simshim>:<diag dylib>` in a64-penv. Note: a64-penv is shared by every
  phone using this libarm64jit; clear it after use.

## Gate
See the end of this file.

## Gate result
s30g1 (cold boot, default config): **0 failed**. Only one boot this round. Defaults are unchanged (every new behaviour is behind a knob), so the three-clean-boot rule did not apply. Maps route check not in the default gate (still aborts).
