# schwung-awm2

Yamaha **MU100** AWM2 / XG tone generator for Move Anything (Schwung) — full
hardware emulation (Hitachi H8 + Yamaha SWP30) via a vendored MAME core, giving
the authentic XG sound *and effects* (reverb / chorus / variation) that a
soundfont can't reproduce.

**Status:** feasibility complete — see `docs/`. Module build in progress.

- Sound quality: the one MAME SWP30 click bug is fixed
  (`patches/0001-swp30-saturate-interpolation.patch`; upstreamed as
  [mamedev/mame#15505](https://github.com/mamedev/mame/pull/15505)).
- Performance: fits the CM4 Move (~0.5–0.9 of one A72 core; 4 cores available).
- DRC/JIT confirmed working on-device.

## ROMs (user-supplied)

This ships **without ROMs**. You must supply the MU100 ROM set (the MAME `mu100`
romset, ~26 MB program + wave ROMs). CRCs are documented in `patches/`.

## Docs
- `docs/plans/2026-06-13-awm2-mu100-emulation-design.md` — design + gate results
- `docs/ARCHITECTURE-integration.md` — vendor-MAME host plan & build
- `patches/` — the SWP30 fix + upstream PR materials
