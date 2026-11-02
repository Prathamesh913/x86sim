# Speed 31 (2026-11-01), in progress

## 1. SpringBoard icon-parallax motion engine at idle (time box used up, no change made)
The plan was to make the simulator motion source behave the way it does on a Mac. Reading the UIKitCore code in the 27.2
shared cache (tools: $SCRATCH/s31/tools/cd2.py, an address-annotated llvm-mc disassembler with UIKit symbols) shows
there is nothing on the Mac side to copy:
- `_UIMotionEffectSimulatorMotionEventProvider` produces no events at all. start/stopGeneratingEvents only flip
  `_shouldGenerateEvents` on an NSOperationQueue, and nothing in the class posts an event. The engine always uses this
  provider class (`+_eventProviderClass` is a constant). The code is the same on a Mac, so the Mac gets no events either.
- `+_motionEffectsEnabled` returns NO only when `_AXSReduceMotionEnabled` is on, or when an internal-diagnostics build
  has UIMotionEffectsEnabled=NO. Reduce Motion is 0 on the Mac's simulator too.
- `_updateDisplayLinkIntervalForScreenState:` pauses the link only when the screen is dimmed. It drops to 5-8 fps only
  in slow mode, and slow mode is entered only from `_updateSlowModeForNewEvent:...`, which needs real events. Without
  events the link stays at 30 fps (60 or more on 120 Hz screens) and each callback returns early
  (`_hasReceivedAtLeastOneEventSinceStarting`=0).
- Speed 30's Mac evidence does not hold up. Its sample came from a phone that was not at the home screen: the Mac's
  27.0 phone is in Settings with a Maps location alert showing (screenshot $SCRATCH/s31/mac27.png). A 30 fps no-op
  callback would hardly show in `sample` anyway. That SpringBoard does ~70 context switches/s. lldb cannot attach to it
  non-interactively, so the hit count could not be measured there.
- Conclusion: the 30 fps engine link is Apple's own behaviour without motion hardware. It is not something our stand-ins
  break. Removing it would mean changing UIKit's behaviour (which counts as feature disabling under the rules), so it was
  not done. The real lever is the cost of each wakeup (the mach_msg / darlingserver path from Speed 30).
- No numbers were taken: nothing changed.
