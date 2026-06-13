# Ready-to-submit MAME PR

Target: `mamedev/mame`, branch `master`. The affected code in `master` is
identical to 0.288, so this applies cleanly.

---

## Title

sound/swp30: saturate AWM2 interpolation output to prevent full-scale wrap clicks

## Body

### Problem

On the SWP30 (MU100 etc.), loud notes produce an occasional one-sample
full-scale "click" — most noticeable on bright, sustained, near-full-scale
instruments (e.g. GM strings). Quiet notes are unaffected, so it presents as an
intermittent tick on some notes but not others.

### Root cause

In `swp30_device::streaming_block::step()` the 4-tap cubic interpolation result
is stored directly into an `s16`:

```cpp
s16 result = ( - interpolation_table[0][index ^ 2047] * val0
               + interpolation_table[1][index ^ 2047] * val1
               + interpolation_table[1][index       ] * val2
               - interpolation_table[0][index       ] * val3 ) >> 10;
```

Cubic interpolation overshoots the input range near peaks. When the source
samples approach full scale, the interpolated value exceeds the s16 range and
the store **wraps** (+32767 → −32768), i.e. a single-sample sign inversion =
an audible click. Example trace of the raw streamed value across one glitch
(values climbing smoothly, then wrapping):

```
... 26793 -> 30115 -> 32220 -> -32561 -> 32549 -> 31051 ...
                              ^ wrap here
```

### Fix

Accumulate in `s32` and saturate to the s16 range. Real hardware saturates
(a wrap would be audibly broken), so this also matches hardware behaviour.

```cpp
s32 racc = ( ... ) >> 10;
s16 result = racc < -0x8000 ? -0x8000 : (racc > 0x7fff ? 0x7fff : racc);
// or, matching modern MAME style:
// s16 result = std::clamp<s32>(racc, -0x8000, 0x7fff);
```

### Testing

Played GM patches through the `mu100` driver and recorded with `-wavwrite`.
An impulse (2nd-difference) click count over the rendered audio:

| material            | before | after |
|---------------------|:------:|:-----:|
| 6 sustained notes   |   3    |   0   |
| full GM song        |  15+   |   0   |
| quiet/decay patches |   0    |   0   |

Audio that never overshoots is bit-identical before/after; only the
previously-wrapping samples change (now saturated).

---

## Patch

See `0001-swp30-saturate-interpolation.patch` in this directory
(`patch -p1 < ...` from the MAME source root).

## Notes for submission

- Maintainers may prefer the `std::clamp<s32>(...)` form (recent swp30 commits
  already use `std::clamp`); functionally identical.
- This is distinct from issue #11976 (envelope release abruptness); it is a
  separate interpolation-overflow defect, though both manifest as "clicks".
