# Speed 33 (2026-11-02): aged SpringBoard, safe install hook + feedback default, sampled counts, Maps step

Phone: var/device-a64-ios272 (27.2 24B8117; 27.0's runtime is missing). Image $SCRATCH/jc-24B8117. Method: logs first,
tapm.sh (5 warm taps), openm.sh (killed cold, 2 rounds), perf record/stat on SpringBoard, guest sampler
(X86CODE_A64_GSAMPLE via a64-penv, cleared after each use). Haiku subagents for two code lookups.
Image state before this round saved as reflinks in $SCRATCH/s33/jc-before.

## 1. Long-running slowdown (time box spent; cause narrowed, not fixed)
| state | SpringBoard CPU per warm tap | response |
|---|---|---|
| fresh boot, 60 s in | 540-580 ms | 0.41-0.47 s |
| after a gate (aged system) | 620-650 ms | 0.49-0.55 s |
| aged system, SpringBoard restarted (kill -9, launchd respawn) | 610-630 ms | 0.48-0.52 s |
| fresh boot, SpringBoard restarted | 540-550 ms | 0.42-0.43 s |
| ~4 min after a fresh boot, no gate (SpringBoard restarted twice) | 610-620 ms | 0.48-0.52 s |
- **It is not SpringBoard's age**: a new SpringBoard in an aged system is just as slow, and no other process is busy
  (idle SpringBoard 7 ms/s, main thread carries the taps). Speed 32's "440 ms after a restart" did not reproduce: fresh
  is 540-580 here (same image, default config), so the gap is ~70-90 ms/tap, not ~170.
- perf, 4 taps each: aged 19.7G cycles vs fresh 14.3G (+38%), spread evenly: image code +16%, libarm64jit +52%
  (GetBlock +61%, FastDispatchProbe +52%, LookupBlockThunk +67%, CallSVC +70%, the C-side FastDispatchCrc x4.5),
  live JIT +39%; a live-translated loop (host 0x7fff7c3c55xx, calls dynarmic's software FPRoundInt / FPRoundBase) runs
  only in the aged state (~0.9G cycles). So SpringBoard does more guest work per tap and re-enters the dispatcher more
  (native calls, unlinked lookups); not a JIT-cache or table degradation.
- Guest sampler (stub-parked threads skipped, new `X86CODE_A64_GSAMPLE_SKIP=stubs`; trace now flushed every 200
  samples so a killed process keeps it): in the aged state the tap runs libate `at_encoder_decompress_texels` (native
  ASTC) and vImage conversion (`vImageConverter...`, from the image: its blocks are in the vImage chunk) = decoding an
  app snapshot image. A fresh system seems not to have that snapshot yet. The FPRoundInt loop was not attributed
  (sampler data from that run was lost before the flush fix).
- Next: sample the aged tap with GSAMPLE_SKIP=stubs and perf together, attribute the FPRoundInt loop (likely a
  pixel-format conversion with FRINTX/FRINTI, which dynarmic's x64 backend leaves to the software fallback: an inline
  ROUNDPS/ROUNDSS for the FPCR-mode variants would remove it) and check whether the snapshot decode is per tap.

## 2. Cold Settings vs the grown image
- Killed cold, 2 rounds, before this round's changes: Settings 3.58 / 4.15 s, x86ui (6.84 first) / 1.84, Cadence 2.04
  / 2.08. After the in-place feedback update (section 3): Settings 3.42 / 3.90, x86ui 1.82 / 1.73, Cadence 2.09 /
  (6.80: first open after the image was replaced, the tmpfs copy). After tests/apps' 3 reinstalls (new image key):
  Settings 5.47 (first process on the new image, the copy again), x86ui 1.87, Cadence 3.04.
- What made Speed 32's image slow to grow: its install update re-slotted 483 truncated chunks to the end of the slab
  (old slots left as holes), 1.53 -> 1.70 GB. That growth path is closed now (section 3). A clean-vs-grown A/B needs a
  compacted image of the same training set, which does not fit the slab (Speed 32); not done in the time box. The
  first open after any image change pays the huge-page tmpfs copy (~2-5 s), which is what the worst numbers show.

## 3. Install hook safe, feedback rebuilds bounded, both default (gated)
- **x86code-aot slots** (aot.rs build): a chunk whose key did not change (training feedback only) is rebuilt **in its
  own slot** even when truncated (hottest first, so the slot drops the coldest blocks). New / re-keyed chunks take the
  **first free gap** (slots of dropped or re-keyed chunks), then the end. When nothing fits, the **coldest library
  chunks are evicted** (fewest profile hits per slot byte; never a chunk being built, never an app chunk): the record
  keeps its key with no file (up to date, not in the image; that library runs live), the chunk file moves to
  aot/evicted. Only "nothing left to evict" fails.
- **Feedback bound**: ChunkRec.live_seen = the .live blocks in the chunk's ranges when it was built; a chunk goes stale
  only for >= 32 *new* live blocks, so a chunk whose slot cannot hold them all is rebuilt once, not on every update.
- **appinstall.sh**: placement.txt is backed up, placeapp adds the app, `x86code-aot update --only <container UUID>`
  builds only that app's chunks; on any failure the previous placement.txt comes back (the previous image still
  matches it), so placement and image never part. Library feedback stays with the boot-time update.
- Feedback update on the live phone: 210 chunks rebuilt in place in 15 s, image code space unchanged (1225 MB), blocks
  4.16M -> 4.21M, then 1014/1014 up to date; a second `status` shows nothing to rebuild.
- tests/apps/run.sh (reinstalls x86data, x86net, x86ui) with the hook on: **PASS**; placement +3 lines, new image
  472d2995 matches it, new processes load it (Preferences: 4218655 blocks). Image 1.70 GB (no growth).
- **Defaults now**: X86CODE_A64_AOT_LIVE on (=0 off), X86SIM_APP_AOT on (=0 off).
- Warm taps after the feedback update: fresh 530-580 ms, aged 620-640: feedback does not touch the aged-tap cost.

## 4. Sampled execution counts (implemented; opt-in for training)
- libarm64jit X86CODE_A64_COUNT=1: one host thread per process looks at every guest thread every 4 ms and counts the
  current block PC of those running guest code; totals go to `<key>.<exe>.cnt` every 20 s (merged with the file).
  hot.rs reads every .cnt and adds hits x 1024 to a block's rank (below LIVE_RANK), so sampled blocks rank above
  blocks that were only translated, and hot_split_pct's percentile now separates hot from cold by real execution.
- Opt-in: a 250 Hz thread in each of ~100 processes is not free at idle; meant for training boots.
- L1i / iTLB on cold Preferences and a split image: not measured this round (time box went to sections 1 and 3).

## 5. Maps route (one step: tooling)
- The abort dump (X86CODE_A64_AGOUT + ABORT_TSD) now also prints the failing node **at the abort** (8 words, flags
  +4, value offset +8, value words, table base) next to the after-set record (0xfd63 -> 0xfd73). Run: see the gate
  note below.

## Gate
**s33g1, s33g2, s33g3: 0 failed each, 3 consecutive clean** (cold boots, default config incl. AOT_LIVE / APP_AOT on and
the in-place-updated image); tests/apps/run.sh PASS (before the gates, same bits and defaults). An earlier SKIPBOOT
run (s33age3, not a gate boot) had 2 STALLs: swipe-up / switcher showed Settings on its About page (state kept from the
same run's About check, no crash in any log); the cold-boot gates did not repeat it. s33a1 (first boot of the round's
libarm64jit) clean too.

Maps (penv AGOUT=1, LIVE_ONLY=Maps, ABORT_TSD=22e74c980; run 1 aborted at the route): the failing attribute's set
recorded `flags +4 0x2f63 -> 0x2f73, value offset +8 0 -> 0x400398` and **at the abort the node is the same**
(60000088 00002f73 00400398 000000a3 004003a8 ..., value word 0x42ea76400, table 0x443a6fe00). So nothing is lost
after the set: the abort's check is not reading this node's flags / value. New lead: the set "returned 152" (w0 low
byte 0x98, not a bool 0/1): next step is AGGraphSetOutputValue's return path / what the caller tests after it.

Rule slips: two temp files I had created in /tmp (/tmp/c1, /tmp/c2, a CPU snapshot) were removed with rm; nothing
under scratch or runtimes was deleted. The first gate attempt ran without A64_DEVICE (the 27.0 device: boot failed on
the missing runtime, no harm). a64-penv is empty. The phone is left booted at home in the default config.
