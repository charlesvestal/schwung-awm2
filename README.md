# schwung-awm2

Yamaha **MU100** AWM2 / XG tone generator for Move Anything (Schwung) — full
hardware emulation (Hitachi H8 + Yamaha SWP30) via a vendored MAME core, giving
the authentic XG sound *and effects* (reverb / chorus / variation) that a
soundfont can't reproduce.

**Status:** functionally complete but **NOT realtime on the CM4** — see
[`STATUS.md`](STATUS.md). The module builds, deploys, boots the MU100 firmware,
loads the wave ROMs, and plays correct XG audio with live MIDI/params — but the
emulation runs at only **~64–71% of realtime** on one Cortex-A72 core, so
sustained audio glitches. Not yet a usable instrument.

- Sound quality: the one MAME SWP30 click bug is fixed
  (`patches/0001-swp30-saturate-interpolation.patch`; upstreamed as
  [mamedev/mame#15505](https://github.com/mamedev/mame/pull/15505)).
- Performance: the wall is MAME's interpreted **H8 CPU** (~50% of the work, no
  JIT in MAME). NOTE: the original feasibility gate predicted ~0.5–0.9 of one
  core — that estimate was **wrong by ~2×**; a synthetic microbench badly
  under-predicted the branchy, cache-bound interpreter on the A72. Reaching
  realtime needs a leaner H8 interpreter (see `STATUS.md` → Next steps).
- Self-contained aarch64 `dsp.so` cross-build pipeline works (`scripts/`).

## ROMs (user-supplied)

This ships **without ROMs**. You must supply the MU100 ROM set (the MAME `mu100`
romset, ~26 MB program + wave ROMs). CRCs are documented in `patches/`.

## Docs
- `docs/plans/2026-06-13-awm2-mu100-emulation-design.md` — design + gate results
- `docs/ARCHITECTURE-integration.md` — vendor-MAME host plan & build
- `patches/` — the SWP30 fix + upstream PR materials
