# schwung-awm2 — status

Yamaha **MU100** (Hitachi H8S/2655 + Yamaha SWP30) AWM2/XG tone generator for the
Ableton Move, by emulating the real hardware (MAME-derived) and feeding it live
MIDI. ROMs are user-supplied (not shipped).

## State: FUNCTIONALLY COMPLETE, NOT REALTIME on the CM4

The module **works end-to-end** on the Move — boots the MU100 firmware, loads the
SWP30 wave ROMs, plays correct XG audio, responds to live MIDI and parameter
changes — but the emulation runs at **~64-71% of realtime** on one Cortex-A72
core, so sustained audio underruns/glitches. It is not yet a usable instrument.

### What works (verified)
- **Headless MAME host** (`host/`): boots `mu100`/`mu100b` with a custom
  `osd_common_t` subclass, taps the SWP30 stereo mix via `add_audio_to_recording`.
  Output is **bit-identical** to MAME's own `-wavwrite` (`host/verify.sh`).
- **Live MIDI → H8 SCI** (`host/mu100_osd.h` `MidiInjector` + `LiveMidiPort`):
  MIDI injected from our code, not an SMF. Verified note timing + pitch
  (`host/verify_live.sh`).
- **Module** (`src/dsp/awm2_plugin.cpp`): Move plugin API v2
  (create/destroy/on_midi/set_param/get_param/render_block). Boots the machine on
  a background thread; SWP30 audio → backpressured ring → `render_block`;
  `set_param` → XG MIDI CCs. Prefers screenless `mu100b` if present.
- **Self-contained aarch64 `dsp.so`** (`scripts/`): Docker cross-build of the MAME
  `mu100` subtarget (glibc 2.34, no SDL), `move_plugin_init_v2` exported. Deployed
  and loads in Move Anything.
- **SWP30 interpolation-saturation fix**: `patches/0001` (upstreamed: MAME PR #15505).
- **Headless SDL-free OSD**: `patches/0002` (minimal provider set + null monitor).

### Performance findings (the blocker)
Measured on-device (CM4 @ 1.5 GHz `performance`, not throttled):

| Config | realtime |
|---|---|
| baseline | ~53-56% |
| + pin to a compute core (no privilege) | ~62-64% |
| + SCHED_FIFO RT (needs host rtprio limit) | ~67-71% |
| + LTO / mu100b / -mcpu | negligible / +3% / ~0 |

- **The wall is the H8 CPU interpreter (~50% of the work, mostly genuine firmware
  processing).** MAME interprets the H8 (no JIT).
- **It is NOT MAME's memory/framework overhead.** A de-risk that bypassed MAME's
  memory-handler dispatch with direct ROM pointers gave **no speedup** (589% vs
  568% on a fast host) — `m_cache` already returns a direct pointer. The cost is
  the per-instruction decode/dispatch/flags/cycle-accurate sub-state machine.
- A steady sub-100% deficit **cannot be buffered away** → continuous glitches.
- Device facts: `/etc/init.d/move` pins audio DMA/SPI IRQs to **core 3** (pin our
  thread to a compute core 0-2). `/data/UserData/move-anything` → symlink to
  `/data/UserData/schwung`. No PAM, so RT needs the host launched with an rtprio
  limit (root).

## Next steps — to reach realtime

A deep-research pass (2026-06-13, 23 sources, adversarially verified) established:
- **No off-the-shelf shortcut exists.** No open-source H8/H8S/H8SX JIT/DRC exists
  anywhere; no standalone/realtime MU-series or SWP30/AWM2/MEG emulator exists
  outside MAME. The S-YXG50 author calls a real MU/SWP emulation an unsolved
  "weeks or months" effort. (Negatives are search-bounded but well-supported.)
- **Correction to earlier notes:** in MAME, only the **MEG effects DSP** is JIT'd
  via drcuml; the **AWM2 sample/voice engine is plain C++** (`awm2_step`/
  `mixer_step`). So the SWP30's heaviest loop is already recompiled, and the
  realtime wall really is the **H8 CPU** — corroborating the profile.

### Stage-0 cost split (on-device CM4 profile, 2026-06-13)
Bucketed `perf` of the saturated emulation:
- **H8 CPU ~62%** (54.6% interpreter core + ops, plus ~7% H8-driven memory-handler
  dispatch a JIT would inline away).
- **SWP30 DSP only ~9.5%** — its MEG is already drcuml-JIT'd; the AWM2 voice loop is
  cheap. **Not worth NEON/optimizing.**
- Fixed floor ~30% (scheduler/timers ~2.5%, MEG *re-JIT churn* ~2-3% [minor bug —
  recompiles in steady state], sound-stream/rand/libc).

Projection from the 71% (RT+pin) baseline:
- **H8 JIT ×3 → ~125% realtime (comfortable).**
- H8 JIT ×2 → ~105% (clears, thin margin).
- **Lean interpreter ×1.5 → ~92% (still short — NOT enough on its own).**

=> **The JIT is the way.** A lean interpreter alone does not clear realtime; SWP30
NEON and multi-core are unnecessary (SWP30 is only ~9.5%). Decision: GO for the
JIT *if* the MU100 is worth the effort; the remaining risk is the
interruptibility-vs-DRC incompatibility.

Credible routes (combine, don't pick one — each is partial):
1. **Faster H8.** Either (a) a lean interpreter (computed-goto, drop the
   cycle-accurate sub-state/prefetch model) — ~1.5× proven on a *related* H8
   (Nuked-SC55, H8/500; transferability partial); or (b) an **H8S→UML drcuml
   frontend** — MAME's aarch64 DRC backend is already merged + Pi-4-tested
   (PR #13162, 0.274/0.275) and drcuml is proven retargetable (it already JITs the
   SWP30 MEG), so only the H8 *frontend* is new work. **RISK:** MAME docs state
   "interruptibility and DRC are entirely incompatible," and the H8 uses the
   interruptible mid-instruction-restart model — an H8 DRC must drop/work around
   that (does the MU100 actually need interruptible accesses? open question).
2. **NEON-vectorize the AWM2 voice loop** (it's plain C++, ~15-27% of cost) — an
   independent win not requiring any H8 work.
3. **Multi-core**: H8 on one A72 core, SWP30 audio/MEG on another (MAME's
   scheduler is serial today → needs surgery, but cores are available).
4. **RT + compute-core pin** (done; ~+10%).

Reuse MAME's SWP30 DSP as-is; keep the MAME build as the **bit-exact oracle** to
validate any faster core against. Everything else (module glue, MIDI→SCI, audio
ring, aarch64 pipeline, ROM handling) carries over unchanged.

Honest effort: still substantial (an H8 frontend or lean core is real,
bit-accuracy-sensitive work) and not guaranteed to clear realtime with margin —
but the framework groundwork (arm64 codegen, MEG-as-DRC precedent) is done, so
it's "write an H8 frontend," not "build a JIT from scratch."

**Recommendation:** the lean-interpreter rewrite is days-to-weeks of hard,
bit-accuracy-sensitive work for one borderline-realtime module. Worth it only if
a *playable* MU100 (XG + MEG effects, which a soundfont can't do) is specifically
wanted. Otherwise bank this (working pipeline + analysis) and revisit if the MU100
is prioritized or faster Move hardware appears.

## Build / install / test
- Build dsp.so: `./scripts/build.sh` (needs Docker). Output: `dist/awm2/dsp.so`.
- Install: `./scripts/install.sh` (scp to the Move). User supplies ROMs in `roms/`
  (`mu100.zip`, `swp30.zip`, `mulcd.zip`; optional `mu100b.zip` for screenless).
- Off-device verify: `host/verify.sh` (oracle bit-match), `host/verify_live.sh`
  (live MIDI), `host/verify_plugin.sh` (module via dlopen).
- On-device perf: `host/perf_probe.cpp` (cross-compile, run under `/data/UserData/`).
