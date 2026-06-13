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
