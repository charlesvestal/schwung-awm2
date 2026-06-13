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

## Next steps — to reach realtime (the only viable path)

Write a **lean H8S/2655 interpreter** to replace MAME's cycle-accurate one:
- Computed-goto / threaded dispatch, execute whole instructions at once, drop the
  sub-state machine + prefetch-pipeline modeling, approximate timing.
- Target ~2-3× on the H8 (precedent: `schwung-jv880`'s lean core runs realtime on
  the same CM4). With RT+pin that should clear realtime.
- **Reuse MAME's SWP30 DSP** as-is (the audio chip is fine and already correct).
- **Validate bit-against the MAME build** (kept as the golden oracle) so the lean
  core is provably accurate.
- Everything else here (module glue, MIDI→SCI, audio ring, aarch64 pipeline,
  scheduling, ROM handling) carries over unchanged.

Alternative (definitive but months): an H8→arm64 dynamic recompiler (MAME has
asmjit/drcuml infra but no H8 backend).

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
