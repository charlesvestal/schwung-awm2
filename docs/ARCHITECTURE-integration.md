# schwung-awm2 — integration architecture & build plan

Status: feasibility complete (all gates green); module build not yet executed.

## Approach: vendor MAME core + custom headless OSD

The MU100 emulation (H8 CPU + SWP30 + firmware) is MAME-coupled, so we vendor a
subset of MAME and drive it from our own host code — the pattern proven by
`../ensoniq-sd1-source` (MAME core + a custom `osd_common_t` subclass, no SDL UI).
Performance fits the CM4 even with the full framework (see perf gate), so the
vendor approach is viable; no clean-room shim required.

### Boot pattern (from ensoniq-sd1 PluginProcessor.cpp)
```
osd_options opts;
MyOsd osd(this, opts);                 // : public osd_common_t
cli_frontend fe(opts, osd);
fe.execute({ "mu100", "-rompath", romdir, "-video","none",
             "-keyboardprovider","none","-mouseprovider","none","-joystickprovider","none",
             "-noreadconfig","-skip_gameinfo","-samplerate","64000", ... });
```
`MyOsd` overrides the full `osd_interface`; the audio hook
`sound_stream_sink_update(id, const int16_t* buf, samples)` is where SWP30 stereo
arrives → push to a ring buffer for the Move audio callback. MIDI in is fed to
the H8 SCI (custom midi module or direct injection), replacing the SD-1 keyboard
input path.

## Vendored MAME file set (from the working subtarget build)
Static libs the `mu100` subtarget links (mame-mame0288/build/.../*.a):
`libemu libfrontend liboptional libmame_mame libdasm libformats libutils`
`libocore_sdl3 libosd_sdl3` (osd modules lib incl. `osd_common_t`) + 3rdparty:
`asmjit` (DRC), `softfloat3 expat zlib flac utf8proc lua lualibs sqlite3`
`linenoise zstd lib7z` (+ render libs bgfx/bimg/bx/libjpeg only if not stripped),
`portmidi` (optional), `ymfm wdlfft`.
Runtime-relevant source: `src/devices/cpu/h8/*`, `src/devices/sound/{swp30,meg}*`,
`src/mame/yamaha/ymmu100.cpp` + `mulcd` (stub display), `adc`.
**Carry `patches/0001-swp30-saturate-interpolation.patch`.**

## Performance plan (from schwung-jv880 PR #5)
- Build: `-mcpu=cortex-a72`, LTO, hidden visibility.
- Thread: SCHED_FIFO ~45, pin to cores 0-2, (FTZ only matters if we add float edge code — SWP30 is integer).
- **H8 SLEEP fast-forward** — biggest lever (cost is H8-idle-dominated).
- MEG via DRC (arm64 backend `drcbearm64`, W^X confirmed working on Move).
- Output: SWP30 44100 → Move 64000 via fixed-ratio NEON resampler
  (`resampler_fixed.h`, reusable from jv880).

## Build (Docker cross-compile, like other modules)
`scripts/build.sh` + `Dockerfile` cross-compile with `aarch64-linux-gnu-` to
`dist/awm2/dsp.so`. Plugin glue `src/dsp/awm2_plugin.cpp` implements module API
v2 and bridges params/MIDI/audio to the hosted MAME machine.

## ROMs
User-supplied (`requires` in module.json). Verified `mu100` set CRCs in
`patches/` docs. Engine ships without ROMs (JV-880 posture).

## Open items
1. Decide minimum vendored set (drop render/lua/sqlite if linkable without).
2. Custom `osd_common_t` host: implement full interface (ref ensoniq-sd1).
3. MIDI-in → H8 SCI path (live, not the `-midiin1` SMF image device).
4. Gold-standard on-device perf number once the .so runs on the Move.
