# Speed 35 (2026-11-02): codegen bump in place, split image A/B, image copy out of guests, Maps root cause

Phone: var/device-a64-ios272 (iPhone 17, 27.2 24B8117), image $SCRATCH/jc-24B8117. Method: logs first;
tests/arm64/gate/measure.sh (new: fresh first open, perfopen.sh, openm.sh N=2 Settings/x86ui/Cadence, tapm.sh 5 taps).
AttributeGraph read with `ipsw dyld disass -q`.

## 1. Codegen 0x1a: Speed 34's inline FRINT/FCVTA in the image
- kCodegenVersion 0x19 -> 0x1a (key.rs CODEGEN_VERSION too). A plain `update` refused: "slab full", because every
  truncated chunk changed key and was re-slotted.
- First try, `build --full`: the training set wants 1819 MB, so it could not fit. Full builds now drop the coldest
  library chunks (recorded as evicted). Packed to 1336 MB, that left 8 MB of per-process live room (code cache =
  min(image+384, 1344)), and SpringBoard/backboardd crash-looped on dynarmic's "shared code cache full". Full builds
  now leave FULL_LIVE_ROOM_MB = 128, and re-admission respects it. The result (1197 MB, 3.73M blocks) was much
  slower: cold Settings 6.5 s, Preferences 7.1-7.5M iTLB walks, because the full build loses coverage that the grown
  image's truncated slots keep.
- **Shipped instead: a codegen bump rebuilds every chunk in its own slot** (reslot rule: same library ident and
  address keeps its slot). The pool base now counts only kept, in-plan chunks; stale app containers kept the pool
  "nearly full". Result: 1015 chunks, 1224 MB, 4.23M blocks in 17 s, the same layout as before.
- After (post-update image, fresh boot): Settings cold 3.51 / 3.94 s (first after boot 3.44), x86ui 1.71, Cadence
  2.37 / 2.35, warm 0.73-0.76, taps 530-540 ms. Preferences cold: 80 L1i misses / 1k instructions, 5.9M walks.
  Neutral vs Speed 34 (3.88 / 3.44, x86ui 1.76, Cadence 2.80 / 1.84), as expected for FP-only changes.

## 2. Count-trained split image: A/B lost, not shipped
- Training: train.sh with X86CODE_A64_COUNT=1, NOUPDATE (87 .cnt files). The training gate itself had 7 fails: jetsam
  GONEs, Health/Safari timeouts, and one MercuryPosterExtension brk trap. This is the counting load; MercuryPoster did
  not recur in the default-config gates.
- Control: an in-place `update` on the counts (73 chunks, 4.24M blocks). Split: a reflink copy
  ($SCRATCH/jc-24B8117-split), hot_split_pct 20, `build --full` (834 chunks incl. cold tails, 3.64M blocks).

| | control | split 20% |
|---|---|---|
| Settings cold (2 rounds) | 3.62 / 3.44 s | 6.14 / 5.66 s |
| x86ui cold | 1.77 / 1.69 | 3.28 / 2.98 |
| Cadence cold | 2.62 / 2.49 | 4.07 / 3.77 |
| warm | 0.73-0.77 | 0.70-0.76 |
| warm taps SB CPU | 530-550 ms | 500-540 ms |
| Preferences cold L1i / 1k instr | 79 | 82 |
| Preferences cold iTLB walks (6 s) | 6.35M | 9.6-10.4M |
| Preferences instructions (6 s) | 54G | 77G (more live translation) |

- The split needs a full build, and a full build loses about 0.6M blocks of coverage (above), which swamps any
  layout gain. To isolate the layout, a split has to be done in place in the existing slots (hot head first, cold
  tail in the same slot). Opt-in as before.

## 3. tmpfs image copy: out of every guest
- New linux/arm64jit/a64hugecopy (installed next to libarm64jit.so). The first guest that finds no copy and no fresh
  `.building` marker starts it with a raw clone (no CLONE_VM, exit signal 0: the guest gets no SIGCHLD, and its
  wait4(-1) never sees the child; reaped with __WCLONE). The helper forks the copy away at once, setsid + nice 10, and
  takes the marker. The guest, and every process until the copy is linked in, maps the .map directly, so **no open
  waits for the copy**. Log: /tmp/x86code-a64-img/a64hugecopy.log.
- Live: after the image change the helper copied 1.28 GB in 1.9 s outside the guests, got small pages and wrote
  `.small`. This host has never had the 2 MB pages (Speed 28/29/34), so the old foreground copy was 2-5 s thrown away.
- Speed 34's background thread is removed (it ran inside UserEventAgent and died, likely with its host process).
  X86CODE_A64_AOT_HUGE_FG=1 keeps the old foreground copy.
- Doing it at image-build time is not possible: the .map (relocated, linked image) is made by the first guest of a
  layout.

## 4. darlingserver bypass: designed, floor measured, not built
- Floor (two Linux processes, shm ring + futex, wake only when the peer sleeps): **2.2 us round trip** (0.14 us if
  spinning), vs 18 us through darlingserver and 5.2 us on the Mac.
- Why it was not built this round: rights live in darlingserver's XNU IPC spaces. A fast path must keep ordering with
  slow-path messages on the same port, handle send-once replies, port sets, and kqueue/workloop receives (most
  libdispatch traffic arrives through kevent, not plain mach_msg receive), plus dead names and no-senders. A
  plain-receive-only path would speed up msgbench and little real traffic.
- Design for the next round:
  - darlingserver gives a receive right a "fast channel": a shm ring per port plus a generation word, mapped
    read-only into senders on their first send.
  - The client library caches name -> channel and checks the generation on each send. Any right change, port
    death or receiver move bumps the generation, and the client falls back.
  - Slow-path messages to a fast port are enqueued by darlingserver into the same ring as "fetch via RPC" markers,
    which keeps the order.
  - Receivers block on the ring futex. Port-set and kevent receivers get a per-set futex that darlingserver also
    signals.
  - Only simple messages (no rights besides MAKE_SEND_ONCE replies, no OOL, no vouchers) take the fast path; the
    rest use the RPC.
- Not measured: msgbench / idle CPU / taps / cold open with a fast path (none built).

## 5. Maps route abort: the condition and what makes it true
- AG::Graph::UpdateStack::update computes the node pointer once, before the rule:
  `x28 = *(0x22e74c908) + attr` (0x1c950bf90).
- After the rule (`blr x8` at 0x1c950c130) it reloads the flags **through x28** (`ldr w8,[x28,#4]`, 0x1c950c134).
  If bit 4 (value set) is clear and the value type's size (VWT +0x40) is non-zero, it reports "failed to set an
  initial value" (0x1c950c4b8 -> 0x1c950c628 -> 0x1c950c650).
- Probe X86CODE_A64_AGCHK=1 (SVC at 0x1c950c134; with LIVE_ONLY=Maps): the node table **moves during rules**
  (`grow_region`, vm_remap copy=FALSE). On the first move (0x317... -> 0x3ce...) the old address still aliases: same
  flags through both. At the abort (2 of 2 runs):
  `flags via x28 0x3763, value offset 0 (base then 0x3cef5fe00); via base now 0x3773, value 0x400418
  (base now 0x443a5be00)`.
  The set landed in the new table, and the old range no longer shares its pages.
- What in our stack makes it true: on XNU the source of a copy=FALSE remap keeps sharing pages with the
  destination. darlingserver's dtape_share_region does this only when the source is one private VMA or one whole
  memfd VMA. The second growth's source is the first growth's destination: a memfd VMA followed by fresh anonymous
  pages, or an entry it deems stale. That takes the "partly shared / snapshot" paths, which copy into a new memfd
  without keeping the source aliased.
- Fix (next round): for a same-task copy=FALSE remap, always map the new memfd over the whole source range (all its
  VMAs, under the REMAP_FREEZE write-protect), never a snapshot.

## 6. Leftovers
- **Uninstall prunes placement.txt**: `placeapp.py --prune JC DATA` drops Containers/ rows whose container exists on
  no phone. appinstall.sh uninstall runs it, then `x86code-aot update` (assembly only), and restores the placement
  if the update fails, as install does. Tested on a copy with a fake row. A live uninstall was not run (only the
  user's apps are installed).
- **Gate switcher check**: logs first (SpringBoard alive, no crash), then switcherlook.py when the full-frame OCR
  misses the label: OCR of the cropped, upscaled label band, else the card's shape (left strip vs card body > 40
  grey levels). On Speed 34's frames: s34h3's failed frame -> card (42 vs 250), passing switcher -> card, home and
  full-screen Settings -> none.

## Gate
**s35g1, s35g2, s35g3: 0 failed each, 3 consecutive clean** (cold boots, default config: codegen 0x1a in-place image,
a64hugecopy, AGCHK off). tests/apps/run.sh **PASS** afterwards (run-20261102-065758).

## Defaults / opt-ins
- New defaults (gated): codegen 0x1a image; huge copy through a64hugecopy (X86CODE_A64_AOT_HUGE_FG=1: old
  foreground copy); uninstall prune; aot reslot / pool / full-build room rules.
- Opt-in / debug: X86CODE_A64_AGCHK=1, X86CODE_A64_COUNT=1, hot_split_pct (lost the A/B).

Rule check: nothing deleted (test files moved to $SCRATCH/trash/s35; the cg19 image is in $SCRATCH/s35/img-cg19, the
full-build image in $SCRATCH/s35/img-full-cg1a, the split dir kept); a64-penv cleared; phone left booted at home in
the default config.
