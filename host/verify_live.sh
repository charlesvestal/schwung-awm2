#!/usr/bin/env bash
# Verify the live MIDI path: MIDI injected from our own code (no SMF) reaches the
# SWP30 through the H8 SCI. Renders the built-in --live demo scale twice (must be
# bit-identical = deterministic), then checks onset timing and pitch.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SPIKE="${SPIKE:-$HERE/../spike}"
HOST="$HERE/build/awm2_host"
RATE="${RATE:-48000}"
TMP="$(mktemp -d)"
export SDL_VIDEODRIVER=dummy

[ -x "$HOST" ] || { echo "build first: ./build_host.sh" >&2; exit 1; }

args=(mu100 -rompath "$SPIKE/roms" -video none -sound none -nothrottle
      -seconds_to_run 10 -samplerate "$RATE")

( cd "$SPIKE" && "$HOST" --live "$TMP/live1.wav" -- "${args[@]}" ) >/dev/null 2>&1
( cd "$SPIKE" && "$HOST" --live "$TMP/live2.wav" -- "${args[@]}" ) >/dev/null 2>&1

if cmp -s "$TMP/live1.wav" "$TMP/live2.wav"; then
  echo "determinism: PASS (bit-identical across runs)"
else
  echo "determinism: FAIL"; rm -rf "$TMP"; exit 1
fi

python3 "$HERE/analyze_live.py" "$TMP/live1.wav"
rc=$?
rm -rf "$TMP"
exit $rc
