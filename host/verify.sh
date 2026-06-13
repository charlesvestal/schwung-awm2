#!/usr/bin/env bash
# Verify the headless host renders bit-identically to the MAME oracle.
# Renders each spike MIDI with both stock `mame -wavwrite` and our awm2_host,
# then cmp's the WAVs. Both must be byte-for-byte equal (same m_record_buffer,
# same swp30-fixed libs).
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SPIKE="${SPIKE:-$HERE/../spike}"
MAME="${MAME:-$HERE/../../mame-mame0288/mame}"
HOST="$HERE/build/awm2_host"
SECONDS_TO_RUN="${SECONDS_TO_RUN:-12}"
RATE="${RATE:-48000}"
TMP="$(mktemp -d)"

# Headless: stop the SDL3 OSD from opening/fullscreen-grabbing a window.
export SDL_VIDEODRIVER=dummy

if [ ! -x "$HOST" ]; then echo "build first: ./build_host.sh" >&2; exit 1; fi
if [ ! -x "$MAME" ]; then echo "missing oracle mame at $MAME" >&2; exit 1; fi

pass=0; fail=0
for mid in "$SPIKE"/*.mid; do
  [ -e "$mid" ] || { echo "no MIDI files in $SPIKE" >&2; exit 1; }
  name="$(basename "$mid" .mid)"
  args=(mu100 -rompath "$SPIKE/roms" -video none -sound none -nothrottle
        -seconds_to_run "$SECONDS_TO_RUN" -samplerate "$RATE" -midiin1 "$mid")
  ( cd "$SPIKE" && "$MAME" "${args[@]}" -wavwrite "$TMP/o_$name.wav" ) >/dev/null 2>&1
  ( cd "$SPIKE" && "$HOST" "$TMP/h_$name.wav" -- "${args[@]}" ) >/dev/null 2>&1
  if cmp -s "$TMP/o_$name.wav" "$TMP/h_$name.wav"; then
    echo "  PASS  $name ($(wc -c <"$TMP/h_$name.wav" | tr -d ' ') bytes)"; pass=$((pass+1))
  else
    echo "  FAIL  $name"; fail=$((fail+1))
  fi
done
rm -rf "$TMP"
echo "=== $pass identical, $fail differ ==="
[ "$fail" -eq 0 ]
