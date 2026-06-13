#!/usr/bin/env bash
# Validate the module plugin off-device: build the macOS dsp.dylib + test loader,
# then dlopen it like the Move runtime would — boot an instance, inject a note
# via on_midi(), pull audio via render_block(), and confirm the note sounds and
# params round-trip. (No device involved; this is the Mac stand-in for dsp.so.)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
SPIKE="${SPIKE:-$ROOT/spike}"
export SDL_VIDEODRIVER=dummy

"$HERE/build_plugin_mac.sh"
"$HERE/build/test_plugin" "$HERE/build/dsp.dylib" "$SPIKE/roms" "/tmp/awm2_plugin_test.wav"
