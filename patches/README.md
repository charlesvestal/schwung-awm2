# SWP30 patches

Patches we apply to MAME's `src/devices/sound/swp30.cpp` for the schwung-awm2
port. Apply against MAME 0.288 (and likely later) from the MAME source root:

```
patch -p1 < patches/0001-swp30-saturate-interpolation.patch
```

## 0001 — saturate interpolation result

**Bug:** `streaming_block::step` stores the 4-tap cubic interpolation result into
an `s16` without saturation. Cubic interpolation overshoots past full scale near
loud peaks, so the store wraps (+32767 → −32768) — a one-sample full-scale sign
flip that is an audible click on loud notes. (Clean on quiet notes, which is why
it presented as "a click at the peak of the attack" on some notes but not
others.)

**Fix:** accumulate in `s32`, then saturate to the s16 range. Real hardware
saturates (a wrap would be audibly broken hardware), so this is also the
authentic behaviour.

**Verified:** impulse-clicks went 6-note test 3→0, full GM song 15+→0, with
non-overshooting audio bit-unchanged. Candidate to upstream to MAME.
