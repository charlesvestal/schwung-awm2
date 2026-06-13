#!/usr/bin/env bash
# Install the AWM2 (MU100) module to the Move.
#
# NOTE: user-supplied MU100 ROMs are required and are NOT shipped. Place the
# MAME 'mu100' romset (mu100.zip, swp30.zip, mulcd.zip) in the module's roms/
# directory on the device after installing.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
MOVE="${MOVE_HOST:-ableton@move.local}"
DEST="/data/UserData/move-anything/modules/sound_generators/awm2"

cd "$REPO_ROOT"
if [ ! -f "dist/awm2/dsp.so" ]; then
  echo "Error: dist/awm2/dsp.so not found. Run ./scripts/build.sh first." >&2
  exit 1
fi

echo "=== Installing AWM2 (MU100) Module to $MOVE ==="
ssh "$MOVE" "mkdir -p $DEST/roms"
scp dist/awm2/dsp.so dist/awm2/module.json "$MOVE:$DEST/"
ssh "$MOVE" "chmod -R a+rw $DEST"

echo ""
echo "=== Install Complete: $DEST ==="
echo "ROMs: copy your MU100 romset to $DEST/roms/ on the device:"
echo "    scp mu100.zip swp30.zip mulcd.zip $MOVE:$DEST/roms/"
echo "Then restart Move Anything to load the module."
