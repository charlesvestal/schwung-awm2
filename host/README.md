# schwung-awm2 headless MAME host

The integration core for the module: a custom `osd_common_t` subclass that boots
MAME's `mu100` machine with **no SDL UI**, taps the SWP30 stereo master mix, and
(for now) renders a MIDI file to WAV from our own code.

This is the foundation for the Move port: the WAV writer is later replaced by a
ring buffer feeding the Move audio callback, and the `-midiin1` SMF image device
is replaced by live MIDI injected into the H8 SCI.

## Files
- `awm2_host.cpp` — `Mu100Osd : public osd_common_t` + `main()` + WAV writer.
  Audio arrives via the `add_audio_to_recording` OSD hook, which receives the
  exact same `m_record_buffer` MAME's own `-wavwrite` path consumes.
- `build_host.sh` — compiles and links against the static libs from the
  `mu100` single-driver subtarget in `../../mame-mame0288`.

## Prerequisites
Build the MAME subtarget once (carries the swp30 saturation fix):
```bash
cd ../../mame-mame0288 && make SOURCES=src/mame/yamaha/ymmu100.cpp -j8
```

## Build
```bash
./build_host.sh            # -> host/build/awm2_host
```

## Run
```bash
awm2_host <out.wav> -- mu100 -rompath <romdir> -video none -sound none \
    -nothrottle -seconds_to_run <N> -samplerate 48000 -midiin1 <file.mid>
```

## Verify (bit-identical to the MAME oracle)
`verify.sh` renders each spike MIDI with both the stock `mame` binary
(`-wavwrite`) and our host, then `cmp`s them. They must be byte-for-byte equal
because both consume the same `m_record_buffer` from the same fixed libs.
```bash
./verify.sh
```

## Live MIDI (no SMF) — step 2
`--live` injects MIDI from our own code instead of an SMF image device. A
`MidiInjector` (in `awm2_host.cpp`) feeds raw bytes through a `LiveMidiPort`
(an `osd::midi_input_port` returned by `create_midi_input`); the midiin device
polls it at 1500 Hz and clocks the bytes onto the H8 SCI rxd line at 31250 baud.
The injector supports a deterministic time-gated **schedule** (used here for
off-device verification) and a lock-free **realtime ring** (`push_realtime`,
the path the Move module will drive). `-midiin1 <sentinel>` is auto-added so the
midiin device routes to our port rather than parsing an SMF.

```bash
awm2_host --live out.wav -- mu100 -rompath <romdir> -video none -sound none \
    -nothrottle -seconds_to_run 10 -samplerate 48000
./verify_live.sh   # renders the built-in C-major scale, checks determinism +
                   # onset timing + pitch (8/8 notes at the scheduled times)
```

## Module plugin (step 3)
`../src/dsp/awm2_plugin.cpp` implements the Move Anything plugin API v2
(`move_plugin_init_v2` → create/destroy/on_midi/set_param/get_param/get_error/
render_block). It boots mu100 once on a background thread (`-nothrottle`, no
`-seconds_to_run`); the SWP30 mix flows through `add_audio_to_recording` into a
ring buffer whose backpressure throttles the emulation to real time; the Move's
`render_block` drains it. `on_midi`/`set_param` inject MIDI in real time via the
shared `MidiInjector` (`push_realtime`). Native rate is 44100 = the Move rate
(`MOVE_SAMPLE_RATE`), so no resampling. `set_param` maps params to XG MIDI CCs
(program/bank → bank-select+PC; reverb/chorus/variation → CC91/93/94; cutoff/
resonance/attack/release → CC74/71/73/72; volume → CC7).

Ring depth sets output latency (~93 ms now): `-nothrottle` outruns the consumer
so backpressure pins the ring full; tighten once step-4 perf work lands.

```bash
./verify_plugin.sh   # builds dsp.dylib + test loader, dlopens it like the Move
                     # runtime, injects a note, checks audio + param round-trip
```
`build_plugin_mac.sh` is the macOS stand-in for the aarch64 Docker build
(`scripts/build.sh` → `dist/awm2/dsp.so`, step 4). `AWM2_DEBUG=1` logs audio
chunk/ring activity.

## Key implementation notes
- `main()` must pass a program-name element as `args[0]`; MAME's command-line
  parser begins at `args[1]`.
- The base `osd_common_t::init()` does **not** select the provider modules —
  the concrete OSD must call `init_subsystems()` itself (font/render/sound/...).
  With `-video none -sound none` these resolve to the "none" providers.
- The UI render path dereferences a render target (`render_manager::ui_aspect`),
  so we allocate one in `init()` even though nothing is displayed.
- `no_sound()` is overridden to `false` so the master mix is always computed
  regardless of the (none) OSD sound sink.
